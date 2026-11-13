//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The ICC profile that the photo surfaces of one window convert to
// (docs/roadmap/alcedo_studio/ui/windows_display_icc_color_management_plan.md, section 6.2).

#pragma once

#include <QEvent>
#include <QImage>
#include <QObject>
#include <QWindow>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "ui/edit_viewer/display_color_transform.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "ui/edit_viewer/monitor_color_profile.hpp"

namespace alcedo {

/// What the destination of a window is, and why.
enum class DisplayDestinationKind : std::uint8_t {
  /// The ICC profile of the monitor.
  MonitorProfile = 0,
  /// sRGB: Windows Advanced Color is active on the monitor, and the Desktop Window Manager
  /// converts SDR application pixels from sRGB to the panel (plan decision D9).
  SrgbAdvancedColor,
  /// sRGB: Windows has no profile for the monitor (plan decision D10).
  SrgbNoProfile,
  /// No conversion: the monitor profile cannot be used (plan decision D11). `error` names it.
  Unusable,
  /// No conversion: the window has no native Windows window, such as on the Qt offscreen
  /// platform, or the platform is not Windows.
  NoWindow,
};

/**
 * @brief The one record of what the photo surfaces of a window convert to.
 *
 * Immutable after publication: @ref WindowDisplayDestination publishes it as
 * `std::shared_ptr<const DisplayDestination>`, and each consumer converts with the bytes that the
 * settings tile names. A consumer holds the pointer for one conversion.
 */
struct DisplayDestination {
  DisplayDestinationKind kind = DisplayDestinationKind::NoWindow;
  /// The ICC bytes of the destination. Empty for Unusable and NoWindow.
  std::vector<std::byte> profile_bytes;
  /// The 'desc' text of the destination profile; `sRGB` for the two sRGB kinds.
  std::string            description;
  /// The file of the monitor profile. Empty when the monitor has no profile.
  std::filesystem::path  profile_path;
  /// The reason, for Unusable.
  std::string            error;
};

/**
 * @brief The pixels of @p source as a photo surface with @p destination shows them.
 *
 * Converts a copy of @p source with ConvertImageToDisplay when @p destination has profile bytes
 * (MonitorProfile and the two sRGB kinds) and @p encoding is an SDR encoding. Returns @p source
 * for a null destination, for NoWindow and Unusable, and for an ST 2084 or HLG encoding.
 *
 * @param source An image of `QImage::Format_RGBA8888` with the pixels of @p encoding.
 * @throws std::invalid_argument and std::runtime_error as ConvertImageToDisplay.
 * @thread Any thread.
 */
[[nodiscard]] auto ConvertImageForDestination(const QImage&              source,
                                              const ViewerDisplayConfig& encoding,
                                              const DisplayDestination*  destination) -> QImage;

/**
 * @brief The destination of an image store and the conversion of its images (plan section 6.4).
 *
 * An image store keeps source pixels and their encoding. Its image provider converts each
 * requested image with @ref Convert, so a new destination needs no second render. A conversion
 * that fails gives the source pixels and writes one warning for each destination.
 *
 * The converter keeps one DisplayImageTransform for each encoding of the current destination, so
 * that a request does not build a LittleCMS conversion (Windows display ICC plan, W6
 * measurements). @ref SetDestination drops them.
 *
 * Thread: any thread. @ref SetDestination runs on the GUI thread, @ref Convert on the image
 * provider threads.
 */
class DisplayImageConverter {
 public:
  /// Convert to @p destination from now on. Null converts nothing.
  void               SetDestination(std::shared_ptr<const DisplayDestination> destination);
  [[nodiscard]] auto Destination() const -> std::shared_ptr<const DisplayDestination>;

  /// ConvertImageForDestination with the current destination; @p source when it throws.
  [[nodiscard]] auto Convert(const QImage& source, const ViewerDisplayConfig& encoding) const
      -> QImage;

 private:
  /// One prepared conversion of the current destination.
  struct PreparedTransform {
    ViewerDisplayConfig                          encoding{};
    std::shared_ptr<const DisplayImageTransform> transform;
  };

  /// The prepared conversion of @p encoding to @p destination; built outside the lock and kept
  /// while @p destination is current.
  [[nodiscard]] auto TransformFor(const std::shared_ptr<const DisplayDestination>& destination,
                                  const ViewerDisplayConfig&                       encoding) const
      -> std::shared_ptr<const DisplayImageTransform>;

  mutable std::mutex                                mutex_;
  std::shared_ptr<const DisplayDestination>         destination_;
  /// Prepared conversions of destination_, one for each encoding; few encodings exist.
  mutable std::vector<PreparedTransform>            transforms_;
  /// The destination whose failure is in the log already.
  mutable std::shared_ptr<const DisplayDestination> reported_failure_;
};

/**
 * @brief The destination of a monitor with the rules D9, D10, and D11.
 *
 * Reads the file `monitor.profile_path` and checks with LittleCMS that it is an RGB profile with
 * a conversion to the device.
 * @return Never NoWindow.
 * @thread Any thread.
 */
[[nodiscard]] auto ResolveDisplayDestination(const MonitorColorProfile& monitor)
    -> DisplayDestination;

/**
 * @brief Publishes a new destination only when the monitor state differs from the last read.
 *
 * The state is the device name, the profile path, the last-write time and the size of the profile
 * file, and the Advanced Color state. A changed file at the same path (a new calibration) is a
 * different state. The tracker starts with a NoWindow destination.
 *
 * Thread: one owner thread (the GUI thread of @ref WindowDisplayDestination).
 */
class DisplayDestinationTracker {
 public:
  DisplayDestinationTracker();

  /**
   * @brief Take the result of one monitor read.
   * @param monitor The monitor of the window, or std::nullopt when the window has none.
   * @return true when a new destination is published.
   */
  auto               Update(const std::optional<MonitorColorProfile>& monitor) -> bool;

  /// The published destination. Never null.
  [[nodiscard]] auto Destination() const -> const std::shared_ptr<const DisplayDestination>& {
    return destination_;
  }

 private:
  /// The values of one monitor read that decide whether the destination changes.
  struct MonitorReading {
    std::wstring                    device_name;
    std::filesystem::path           profile_path;
    std::filesystem::file_time_type profile_write_time{};
    std::uintmax_t                  profile_size          = 0;
    bool                            advanced_color_active = false;

    auto operator==(const MonitorReading& other) const -> bool = default;
  };

  static auto                               Read(const MonitorColorProfile& monitor)
      -> MonitorReading;

  /// std::nullopt while the window has no monitor.
  std::optional<MonitorReading>             reading_;
  std::shared_ptr<const DisplayDestination> destination_;
};

/**
 * @brief The destination of one QWindow, read again when it can change.
 *
 * Reads the monitor of the window on the first exposure of the window, on
 * `QWindow::screenChanged`, and when the application becomes active (the user can change the
 * monitor profile in the Windows settings meanwhile). Emits @ref DestinationChanged only when the
 * destination changes. On a platform other than the Windows QPA the destination stays NoWindow
 * and the object emits nothing.
 *
 * The object is a child of its window: @ref ForWindow creates it once.
 *
 * Thread: the GUI thread. @ref Destination returns a pointer that other threads can hold.
 */
class WindowDisplayDestination final : public QObject {
  Q_OBJECT

 public:
  /// The destination object of @p window; created on the first call. @p window must not be null.
  static auto        ForWindow(QWindow* window) -> WindowDisplayDestination*;

  /// The current destination. Never null.
  [[nodiscard]] auto Destination() const -> std::shared_ptr<const DisplayDestination> {
    return tracker_.Destination();
  }

  /// Read the monitor of the window now and apply it with @ref ApplyMonitorReading. Does not
  /// create the native window.
  void               Refresh();

  /**
   * @brief Take one read of the monitor of the window.
   *
   * Publishes a new destination, writes its `[DisplayProfile]` log line, and emits
   * @ref DestinationChanged when the monitor state differs from the last read. @ref Refresh
   * calls it with the result of the Windows query; a test gives it a monitor state, because the
   * offscreen platform has no monitor.
   * @param monitor The monitor of the window, or std::nullopt when the window has none.
   */
  void               ApplyMonitorReading(const std::optional<MonitorColorProfile>& monitor);

 signals:
  void DestinationChanged();

 protected:
  auto eventFilter(QObject* watched, QEvent* event) -> bool override;

 private:
  explicit WindowDisplayDestination(QWindow* window);

  QWindow*                  window_;
  DisplayDestinationTracker tracker_;
};

}  // namespace alcedo
