//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/window_display_destination.hpp"

#include <QGuiApplication>
#include <QString>
#include <algorithm>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "ui/edit_viewer/display_color_transform.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {

using alcedo::diag::editorLog;

constexpr const char* kSrgbDescription = "sRGB";

auto ReadFileBytes(const std::filesystem::path& path) -> std::optional<std::vector<std::byte>> {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return std::nullopt;
  }
  std::vector<char> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  if (file.bad()) {
    return std::nullopt;
  }
  std::vector<std::byte> result(bytes.size());
  std::memcpy(result.data(), bytes.data(), bytes.size());
  return result;
}

/// UTF-8 text of @p path for log lines and error texts.
auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

auto SrgbDestination(DisplayDestinationKind kind, std::filesystem::path profile_path)
    -> DisplayDestination {
  DisplayDestination destination;
  destination.profile_path = std::move(profile_path);
  try {
    destination.profile_bytes = SrgbDisplayProfile();
    destination.kind          = kind;
    destination.description   = kSrgbDescription;
  } catch (const std::runtime_error& error) {
    destination.kind  = DisplayDestinationKind::Unusable;
    destination.error = error.what();
  }
  return destination;
}

auto KindName(DisplayDestinationKind kind) -> const char* {
  switch (kind) {
    case DisplayDestinationKind::MonitorProfile:
      return "monitor profile";
    case DisplayDestinationKind::SrgbAdvancedColor:
      return "sRGB (Windows Advanced Color)";
    case DisplayDestinationKind::SrgbNoProfile:
      return "sRGB (no monitor profile)";
    case DisplayDestinationKind::Unusable:
      return "unusable profile";
    case DisplayDestinationKind::NoWindow:
      return "no window";
  }
  return "unknown";
}

}  // namespace

auto ResolveDisplayDestination(const MonitorColorProfile& monitor) -> DisplayDestination {
  if (monitor.advanced_color_active) {
    return SrgbDestination(DisplayDestinationKind::SrgbAdvancedColor, monitor.profile_path);
  }
  if (monitor.profile_path.empty()) {
    return SrgbDestination(DisplayDestinationKind::SrgbNoProfile, {});
  }
  DisplayDestination destination;
  destination.profile_path = monitor.profile_path;
  auto bytes               = ReadFileBytes(monitor.profile_path);
  if (!bytes.has_value()) {
    destination.kind  = DisplayDestinationKind::Unusable;
    destination.error = "cannot read " + PathText(monitor.profile_path);
    return destination;
  }
  try {
    // A two-node table runs every check of a real conversion and reads the 'desc' text.
    const auto lut          = BuildDisplayTransformLut(ViewerDisplayConfig{}, *bytes, 2);
    destination.kind        = DisplayDestinationKind::MonitorProfile;
    destination.description = lut.display_description;
    destination.profile_bytes = std::move(*bytes);
  } catch (const std::runtime_error& error) {
    destination.kind  = DisplayDestinationKind::Unusable;
    destination.error = PathText(monitor.profile_path) + ": " + error.what();
  }
  return destination;
}

auto ConvertImageForDestination(const QImage& source, const ViewerDisplayConfig& encoding,
                                const DisplayDestination* destination) -> QImage {
  if (destination == nullptr || destination->profile_bytes.empty() ||
      !HasDisplayTransform(encoding) || source.isNull()) {
    return source;
  }
  QImage converted = source.copy();
  ConvertImageToDisplay(converted, encoding, destination->profile_bytes);
  return converted;
}

void DisplayImageConverter::SetDestination(std::shared_ptr<const DisplayDestination> destination) {
  // The previous destination and its conversions are released after the lock.
  std::shared_ptr<const DisplayDestination> released;
  std::vector<PreparedTransform>            released_transforms;
  std::lock_guard                           lock(mutex_);
  released = std::exchange(destination_, std::move(destination));
  released_transforms.swap(transforms_);
}

auto DisplayImageConverter::TransformFor(
    const std::shared_ptr<const DisplayDestination>& destination,
    const ViewerDisplayConfig& encoding) const -> std::shared_ptr<const DisplayImageTransform> {
  {
    std::lock_guard lock(mutex_);
    if (destination_ == destination) {
      for (const auto& prepared : transforms_) {
        if (prepared.encoding == encoding) {
          return prepared.transform;
        }
      }
    }
  }
  // Two threads can build the same conversion at once; the second result is not kept.
  auto transform =
      std::make_shared<const DisplayImageTransform>(encoding, destination->profile_bytes);
  std::lock_guard lock(mutex_);
  if (destination_ == destination && std::none_of(transforms_.begin(), transforms_.end(),
                                                  [&encoding](const PreparedTransform& prepared) {
                                                    return prepared.encoding == encoding;
                                                  })) {
    transforms_.push_back(PreparedTransform{encoding, transform});
  }
  return transform;
}

auto DisplayImageConverter::Destination() const -> std::shared_ptr<const DisplayDestination> {
  std::lock_guard lock(mutex_);
  return destination_;
}

auto DisplayImageConverter::Convert(const QImage& source, const ViewerDisplayConfig& encoding) const
    -> QImage {
  const auto destination = Destination();
  // The cases of ConvertImageForDestination that keep the source pixels.
  if (destination == nullptr || destination->profile_bytes.empty() ||
      !HasDisplayTransform(encoding) || source.isNull()) {
    return source;
  }
  try {
    QImage converted = source.copy();
    TransformFor(destination, encoding)->Apply(converted);
    return converted;
  } catch (const std::exception& error) {
    bool first = false;
    {
      std::lock_guard lock(mutex_);
      first = reported_failure_ != destination;
      if (first) {
        reported_failure_ = destination;
      }
    }
    if (first) {
      qCWarning(editorLog, "[DisplayProfile] image shown without display conversion: %s",
                error.what());
    }
    return source;
  }
}

DisplayDestinationTracker::DisplayDestinationTracker()
    : destination_(std::make_shared<const DisplayDestination>()) {}

auto DisplayDestinationTracker::Read(const MonitorColorProfile& monitor) -> MonitorReading {
  MonitorReading reading;
  reading.device_name           = monitor.device_name;
  reading.profile_path          = monitor.profile_path;
  reading.advanced_color_active = monitor.advanced_color_active;
  if (!monitor.profile_path.empty()) {
    std::error_code error;
    reading.profile_write_time = std::filesystem::last_write_time(monitor.profile_path, error);
    if (error) {
      reading.profile_write_time = {};
    }
    reading.profile_size = std::filesystem::file_size(monitor.profile_path, error);
    if (error) {
      reading.profile_size = 0;
    }
  }
  return reading;
}

auto DisplayDestinationTracker::Update(const std::optional<MonitorColorProfile>& monitor) -> bool {
  if (!monitor.has_value()) {
    if (!reading_.has_value()) {
      return false;
    }
    reading_.reset();
    destination_ = std::make_shared<const DisplayDestination>();
    return true;
  }
  auto reading = Read(*monitor);
  if (reading_.has_value() && *reading_ == reading) {
    return false;
  }
  reading_     = std::move(reading);
  destination_ = std::make_shared<const DisplayDestination>(ResolveDisplayDestination(*monitor));
  return true;
}

auto WindowDisplayDestination::ForWindow(QWindow* window) -> WindowDisplayDestination* {
  if (auto* existing =
          window->findChild<WindowDisplayDestination*>(QString(), Qt::FindDirectChildrenOnly)) {
    return existing;
  }
  return new WindowDisplayDestination(window);
}

WindowDisplayDestination::WindowDisplayDestination(QWindow* window)
    : QObject(window), window_(window) {
  connect(window_, &QWindow::screenChanged, this, [this](QScreen*) { Refresh(); });
  if (auto* application = qobject_cast<QGuiApplication*>(QCoreApplication::instance())) {
    connect(application, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
              if (state == Qt::ApplicationActive) {
                Refresh();
              }
            });
  }
  if (window_->isExposed()) {
    Refresh();
  } else {
    window_->installEventFilter(this);
  }
}

auto WindowDisplayDestination::eventFilter(QObject* watched, QEvent* event) -> bool {
  if (watched == window_ && event->type() == QEvent::Expose && window_->isExposed()) {
    // The first exposure only: the other triggers cover the later changes.
    window_->removeEventFilter(this);
    Refresh();
  }
  return QObject::eventFilter(watched, event);
}

void WindowDisplayDestination::Refresh() {
  std::optional<MonitorColorProfile> monitor;
#ifdef _WIN32
  // Only the Windows QPA has an HWND; winId() of the offscreen platform is a counter. A window
  // without its native window has no monitor yet, and winId() would create it.
  if (QGuiApplication::platformName() == QStringLiteral("windows") &&
      window_->handle() != nullptr) {
    monitor = QueryMonitorColorProfile(reinterpret_cast<void*>(window_->winId()));
  }
#endif
  ApplyMonitorReading(monitor);
}

void WindowDisplayDestination::ApplyMonitorReading(
    const std::optional<MonitorColorProfile>& monitor) {
  if (!tracker_.Update(monitor)) {
    return;
  }
  const auto& destination = *tracker_.Destination();
  const auto  device =
      monitor.has_value() ? QString::fromStdWString(monitor->device_name) : QString();
  if (destination.kind == DisplayDestinationKind::Unusable) {
    qCWarning(editorLog, "[DisplayProfile] %s: no display conversion: %s", qUtf8Printable(device),
              destination.error.c_str());
  } else {
    qCInfo(editorLog, "[DisplayProfile] %s: %s '%s' (%s)", qUtf8Printable(device),
           KindName(destination.kind), destination.description.c_str(),
           PathText(destination.profile_path).c_str());
  }
  emit DestinationChanged();
}

}  // namespace alcedo
