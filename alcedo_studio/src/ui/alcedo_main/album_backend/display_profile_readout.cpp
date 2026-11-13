//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/display_profile_readout.hpp"

#include <qqml.h>

#include <QDir>

namespace alcedo::ui {

DisplayProfileReadout::DisplayProfileReadout(QObject* parent) : QObject(parent) {}

void DisplayProfileReadout::setWindow(QWindow* window) {
  if (window_ == window) {
    return;
  }
  QObject::disconnect(destination_connection_);
  window_ = window;
  source_ = window != nullptr ? WindowDisplayDestination::ForWindow(window) : nullptr;
  if (source_ != nullptr) {
    destination_connection_ = connect(source_, &WindowDisplayDestination::DestinationChanged, this,
                                      &DisplayProfileReadout::destinationChanged);
  }
  emit windowChanged();
  emit destinationChanged();
}

auto DisplayProfileReadout::Destination() const -> std::shared_ptr<const DisplayDestination> {
  return source_ != nullptr ? source_->Destination() : nullptr;
}

auto DisplayProfileReadout::state() const -> State {
  const auto destination = Destination();
  if (destination == nullptr) {
    return State::NoMonitor;
  }
  switch (destination->kind) {
    case DisplayDestinationKind::MonitorProfile:
      return State::Applied;
    case DisplayDestinationKind::SrgbAdvancedColor:
      return State::ManagedByWindows;
    case DisplayDestinationKind::SrgbNoProfile:
      return State::NoProfile;
    case DisplayDestinationKind::Unusable:
      return State::Unusable;
    case DisplayDestinationKind::NoWindow:
      return State::NoMonitor;
  }
  return State::NoMonitor;
}

auto DisplayProfileReadout::description() const -> QString {
  const auto destination = Destination();
  return destination != nullptr ? QString::fromStdString(destination->description) : QString();
}

auto DisplayProfileReadout::profilePath() const -> QString {
  const auto destination = Destination();
  if (destination == nullptr || destination->profile_path.empty()) {
    return {};
  }
  return QDir::toNativeSeparators(QString::fromStdWString(destination->profile_path.wstring()));
}

auto DisplayProfileReadout::errorText() const -> QString {
  const auto destination = Destination();
  return destination != nullptr ? QString::fromStdString(destination->error) : QString();
}

void RegisterDisplayProfileReadoutQmlType() {
  qmlRegisterType<DisplayProfileReadout>("Alcedo.Main", 1, 0, "DisplayProfileReadout");
}

}  // namespace alcedo::ui
