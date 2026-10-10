//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <cstdint>
#include <optional>

#include "ui/alcedo_main/album_backend/project_module.hpp"

namespace alcedo::ui {

/// Owns the project launch sequence: one queued open or create request, the launch state that
/// the loading overlay and the welcome surface read, and the rollback when a launch does not
/// start. GUI thread only.
///
/// A launch first sets launchPending and welcomeDismissedForLaunch, so the loading overlay
/// shows in the open window before the welcome surface closes. The request runs one repaint
/// interval later. When it does not start a load, launchPending clears, and the welcome surface
/// returns when no project was entered before the launch. When the load starts, the
/// ProjectModule load state takes over: launchPending clears when the load state changes, and
/// welcomeDismissedForLaunch clears when the load ends or the project changes.
class ProjectLaunchCoordinator final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool launchPending READ launch_pending NOTIFY LaunchStateChanged)
  Q_PROPERTY(
      bool welcomeDismissedForLaunch READ welcome_dismissed_for_launch NOTIFY LaunchStateChanged)
  /// The loading overlay shows for a pending launch and for an enter-mode load. A preview load
  /// runs under the welcome surface without the overlay.
  Q_PROPERTY(bool loadingOverlayVisible READ loading_overlay_visible NOTIFY LaunchStateChanged)
  /// True while a launch is queued or the loading overlay shows.
  Q_PROPERTY(bool launchBusy READ launch_busy NOTIFY LaunchStateChanged)

 public:
  /// The delay between the launch request and the run of the request. It lets the loading
  /// overlay paint before a file dialog or the load start blocks the GUI thread.
  static constexpr int kLaunchRepaintDelayMs = 16;

  explicit ProjectLaunchCoordinator(ProjectModule* project, QObject* parent = nullptr);

  [[nodiscard]] auto launch_pending() const -> bool { return launch_pending_; }
  [[nodiscard]] auto welcome_dismissed_for_launch() const -> bool {
    return welcome_dismissed_for_launch_;
  }
  [[nodiscard]] auto loading_overlay_visible() const -> bool;
  [[nodiscard]] auto launch_busy() const -> bool;

  /// Each Begin call queues one launch and returns true. Returns false and changes nothing when
  /// the accelerator preparation runs or another launch is queued.
  Q_INVOKABLE bool   BeginPromptOpen();
  Q_INVOKABLE bool   BeginPromptCreate();
  Q_INVOKABLE bool   BeginOpen(const QString& projectUrlOrPath);
  Q_INVOKABLE bool   BeginCreate(const QString& folderUrlOrPath, const QString& projectName);
  /// Starts the repaint delay of a queued launch when it has not started yet.
  Q_INVOKABLE void   StartPendingLaunch();
  /// Enters the previewed project (ProjectModule::EnterLoadedProject). When the preview load
  /// still runs, the welcome surface is dismissed so the loading overlay replaces it. Returns
  /// the result of EnterLoadedProject.
  Q_INVOKABLE bool   ContinueWelcomeProject();

 signals:
  void LaunchStateChanged();
  /// The queued request ran. @p started is true when it started a project load.
  void LaunchRequestFinished(bool started);
  // QML Connections maps a handler onFoo to the signal foo, so a signal that starts with an
  // uppercase letter needs a lowercase twin for QML.
  void launchStateChanged();

 private:
  enum class LaunchKind : std::uint8_t { kPromptOpen, kPromptCreate, kOpen, kCreate };
  struct LaunchRequest {
    LaunchKind kind = LaunchKind::kOpen;
    QString    path;
    QString    name;
  };

  auto                    Begin(LaunchRequest request) -> bool;
  void                    RunPendingLaunch();
  void                    OnProjectLoadStateChanged();
  void                    OnProjectChanged();
  void                    SetLaunchState(bool launch_pending, bool welcome_dismissed_for_launch);

  QPointer<ProjectModule> project_;
  QTimer                  launch_timer_;
  std::optional<LaunchRequest> pending_request_;
  bool                         launch_pending_                    = false;
  bool                         welcome_dismissed_for_launch_      = false;
  // A launch from the welcome surface (no entered project) restores it when it does not start.
  bool                         restore_welcome_on_launch_failure_ = false;
};

}  // namespace alcedo::ui
