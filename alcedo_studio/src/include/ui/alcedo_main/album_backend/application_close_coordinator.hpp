//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <cstdint>

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/workspace_router.hpp"

namespace alcedo::ui {

/// Owns the close decision of the application window and the editor finalize sequence that a
/// close runs before the window or the project closes. GUI thread only.
///
/// A close runs one editor finalize and waits for its terminal state on the editor session
/// change notification (EditorSessionController::StateChanged):
/// - It waits while the session is Saving or Switching, or a close or a persist is in flight.
/// - It aborts on a pending recovery, RetainedImageFailure, or Failed, with the session error.
/// - It finishes on NoImage or ShuttingDown, or at once when it only waits for a persist.
/// One close runs at a time.
class ApplicationCloseCoordinator final : public QObject {
  Q_OBJECT
  /// True while a close waits for the editor session.
  Q_PROPERTY(bool waiting READ waiting NOTIFY WaitingChanged)

 public:
  ApplicationCloseCoordinator(EditorSessionController* editor_session,
                              WorkspaceRouter* workspace_router, ProjectModule* project,
                              QObject* parent = nullptr);

  [[nodiscard]] auto waiting() const -> bool { return mode_ != Mode::kIdle; }

  /// True when the application must ask the user before it quits: a close already waits, the
  /// editor session persists or closes an image, or the Editor workspace shows an image.
  Q_INVOKABLE bool   CloseNeedsConfirmation() const;
  /// True while the editor session is Saving or Switching or runs a close or a persist.
  Q_INVOKABLE bool   EditorPersistBusy() const;

  /// Application quit with Save: finalizes the editor image with its changes, shows the
  /// Library, and emits ApplicationCloseFinished or ApplicationCloseAborted.
  Q_INVOKABLE void   BeginSave();
  /// Application quit with Discard. With a pending recovery that the user can discard, discards
  /// it and finishes at once. Otherwise finalizes the editor image without its changes.
  Q_INVOKABLE void   BeginDiscard();
  /// Application quit outside the Editor while a persist runs: waits for the persist only.
  Q_INVOKABLE void   BeginPersistWait();
  /// Stops waiting. The editor finalize that already started continues.
  Q_INVOKABLE void   Cancel();

  /// Closes the loaded project: finalizes the editor image (with its changes when @p persist)
  /// and then calls ProjectModule::CloseProject(@p persist). Returns the reason when the close
  /// cannot start (another close waits, or ProjectModule::ProjectCloseBlockReason is not
  /// empty); nothing changes then. Returns an empty string when the close started; then
  /// ProjectCloseFinished reports the result.
  auto               BeginProjectClose(bool persist) -> QString;

 signals:
  void WaitingChanged();
  void ApplicationCloseFinished();
  /// @p message is the editor session error; it can be empty.
  void ApplicationCloseAborted(const QString& message);
  /// @p closed is false when the editor finalize or the project close failed; @p message then
  /// holds the reason.
  void ProjectCloseFinished(bool closed, const QString& message);

 private:
  enum class Mode : std::uint8_t { kIdle, kApplicationExit, kProjectClose };

  void                              Start(Mode mode, bool awaiting_seal);
  void                              ScheduleEvaluation();
  void                              EvaluateEditorClose();
  void                              Finish();
  void                              Abort(const QString& message);

  QPointer<EditorSessionController> editor_session_;
  QPointer<WorkspaceRouter>         workspace_router_;
  QPointer<ProjectModule>           project_;
  Mode                              mode_                  = Mode::kIdle;
  // False when the close only waits for a running persist (no editor finalize started).
  bool                              awaiting_seal_         = false;
  bool                              project_close_persist_ = true;
};

}  // namespace alcedo::ui
