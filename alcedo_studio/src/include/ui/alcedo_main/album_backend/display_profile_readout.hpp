//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The data of the Display profile tile of Settings > LUTs and Color Management
// (docs/roadmap/alcedo_studio/ui/windows_display_icc_color_management_plan.md, W4).

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWindow>
#include <memory>

#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo::ui {

/**
 * @brief Reads the display destination of one window for QML.
 *
 * QML sets @ref window to the window of the tile. The readout follows
 * `WindowDisplayDestination::DestinationChanged` of that window and holds no copy of the
 * destination: each getter reads the current `DisplayDestination` of the window.
 *
 * Thread: the GUI thread.
 */
class DisplayProfileReadout : public QObject {
  Q_OBJECT
  Q_PROPERTY(QWindow* window READ window WRITE setWindow NOTIFY windowChanged)
  Q_PROPERTY(State state READ state NOTIFY destinationChanged)
  Q_PROPERTY(QString description READ description NOTIFY destinationChanged)
  Q_PROPERTY(QString profilePath READ profilePath NOTIFY destinationChanged)
  Q_PROPERTY(QString errorText READ errorText NOTIFY destinationChanged)

 public:
  /// The conversion state that the tile shows (plan section 3.2).
  enum class State {
    /// The window has no monitor yet (no window, before the first exposure, or not Windows).
    NoMonitor,
    /// The photo surfaces convert to the monitor profile.
    Applied,
    /// Windows Advanced Color is active: the destination is sRGB (plan decision D9).
    ManagedByWindows,
    /// Windows has no profile for the monitor: the destination is sRGB (plan decision D10).
    NoProfile,
    /// The monitor profile cannot be used: no conversion (plan decision D11).
    Unusable,
  };
  Q_ENUM(State)

  explicit DisplayProfileReadout(QObject* parent = nullptr);

  [[nodiscard]] auto window() const -> QWindow* { return window_; }
  /// Follow the destination of @p window; null follows no window (State::NoMonitor).
  void               setWindow(QWindow* window);

  [[nodiscard]] auto state() const -> State;
  /// The 'desc' text of the destination; `sRGB` for the two sRGB states; empty otherwise.
  [[nodiscard]] auto description() const -> QString;
  /// The file of the monitor profile, with native separators. Empty without a profile.
  [[nodiscard]] auto profilePath() const -> QString;
  /// The reason of State::Unusable; empty otherwise.
  [[nodiscard]] auto errorText() const -> QString;

 signals:
  void windowChanged();
  void destinationChanged();

 private:
  /// The destination of the window, or null without a window.
  [[nodiscard]] auto Destination() const -> std::shared_ptr<const DisplayDestination>;

  QPointer<QWindow>  window_;
  QPointer<WindowDisplayDestination> source_;
  QMetaObject::Connection            destination_connection_;
};

/// Register DisplayProfileReadout as `Alcedo.Main` 1.0 `DisplayProfileReadout`.
void RegisterDisplayProfileReadoutQmlType();

}  // namespace alcedo::ui
