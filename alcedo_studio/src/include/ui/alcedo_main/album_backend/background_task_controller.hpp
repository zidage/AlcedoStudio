//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <functional>
#include <vector>

namespace alcedo::ui {

/// Kind of a background task surfaced to the UI. `ModelActivation`, `Import`,
/// and `Export` are declared for completeness; Phase 1 only registers
/// `ImageAnalysis`, `SemanticGeneration`, and `ModelDownload`.
enum class BackgroundTaskKind {
  ImageAnalysis,
  SemanticGeneration,
  ModelDownload,
  ModelActivation,
  Import,
  Export,
  /// Phase 5E: editor session seal/persist while leaving an image.
  EditorSave,
};

/// Lifecycle state of a single background task record.
enum class BackgroundTaskState {
  Queued,
  Running,
  Canceling,
  Succeeded,
  Failed,
  Canceled,
};

/// How the shutdown path (Phase 5) treats a still-active task on app exit.
enum class BackgroundTaskShutdownPolicy {
  CancelAndWait,
  WaitForFinish,
  DetachNotAllowed,
};

/// A capability a running task can hold exclusively so the interaction policy
/// (Phase 2) can disable the matching UI controls. Declared now so task
/// snapshots already carry a `locks` field; no controller publishes locks in
/// Phase 1.
enum class InteractionCapability {
  EditImageDescription,
  EditImageRating,
  EditImageRatingReason,
  RunImageAnalysis,
  CommitImageAnalysisResults,
  ChangeImageAnalysisProvider,
  ChangeSemanticModel,
  RunSemanticGeneration,
  ChangeModelDownloadSettings,
  DeleteImages,
  CloseProject,
};

/// One interaction lock published by a running task. `element_id_ == 0` means
/// the lock is global for that capability (e.g. a whole-album analysis).
struct InteractionLock {
  InteractionCapability capability_ = InteractionCapability::EditImageDescription;
  quint64               element_id_ = 0;
  QString               reason_;
};

/// An interaction lock paired with the id of the task that holds it. Returned by
/// `BackgroundTaskController::ActiveLocks()` so `InteractionPolicyController`
/// (Phase 2) can populate `blockingTaskIds` in its policy answers.
struct ActiveLock {
  QString         task_id_;
  InteractionLock lock_;
};

/// Immutable-ish description of a task at a point in time. Owning controllers
/// build one at start, then push progress/title/detail updates into the
/// registry by id.
struct BackgroundTaskSnapshot {
  QString                      id_;
  BackgroundTaskKind           kind_            = BackgroundTaskKind::ImageAnalysis;
  BackgroundTaskState          state_           = BackgroundTaskState::Queued;
  QString                      title_;
  QString                      detail_;
  int                          progress_percent_ = 0;  // -1 for indeterminate.
  bool                         cancelable_       = false;
  BackgroundTaskShutdownPolicy shutdown_policy_ =
      BackgroundTaskShutdownPolicy::CancelAndWait;
  QVariantList                 affected_targets_;
  std::vector<InteractionLock> locks_;
};

/// UI-facing registry of long-running background tasks (advanced image
/// analysis, semantic generation, model download, ...). Owning controllers
/// keep their existing service jobs and PUSH snapshots/progress into this
/// registry; QML reads `tasks` / `primaryTask` / `runningCount` and cancels
/// via `CancelTask`.
///
/// Phase 1 is additive mirroring only — no modal behavior is removed and no
/// interaction locks are published (those land in Phase 2). The controller is
/// a standalone QObject with no host dependency so it is unit-testable in
/// isolation; owning controllers receive it via narrow constructor injection
/// (`ImageAnalysisController`, `SemanticGenerationController`,
/// `ModelDownloadController`).
///
/// All register/update/finish/cancel calls happen on the UI thread
/// (controller progress callbacks hop back via
/// `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`; QML runs on the UI
/// thread), so no internal mutex is required. Finished tasks are retained up
/// to a small cap so the popover can show recent results; running tasks are
/// always retained.
class BackgroundTaskController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList tasks READ Tasks NOTIFY TasksChanged)
  Q_PROPERTY(QVariantMap primaryTask READ PrimaryTask NOTIFY TasksChanged)
  Q_PROPERTY(int runningCount READ RunningCount NOTIFY TasksChanged)
  Q_PROPERTY(bool hasBlockingShutdownTasks READ HasBlockingShutdownTasks NOTIFY TasksChanged)

 public:
  explicit BackgroundTaskController(QObject* parent = nullptr);

  // ── QML-facing reads ──────────────────────────────────────────────────
  QVariantList Tasks() const;
  QVariantMap  PrimaryTask() const;
  int          RunningCount() const;
  bool         HasBlockingShutdownTasks() const;

  // ── QML-facing actions ─────────────────────────────────────────────────
  // Cancel one task by id. Returns true if a still-cancelable task was found
  // (its cancel callback was just invoked, or had already been invoked by a
  // prior cancel). Returns false for an unknown id or a task that already
  // reached a terminal state. The cancel callback fires at most once.
  Q_INVOKABLE bool CancelTask(const QString& taskId);
  // Cancel every still-cancelable task. Each task's cancel callback fires at
  // most once.
  Q_INVOKABLE void CancelAll();

  // ── C++ policy read (Phase 2: InteractionPolicyController consumes this) ─
  // All locks held by active (non-terminal) tasks, each paired with its task
  // id. `Canceling` counts as active so a cancel-in-flight task's locks stay
  // published until it reaches a terminal state (it may still be writing
  // results). Called only on the UI thread.
  std::vector<ActiveLock> ActiveLocks() const;

  // ── C++ registration helpers (called by owning controllers) ─────────────
  // Register a new task and return the assigned task id (the registry owns id
  // assignment; any `id_` on the input snapshot is overwritten). The cancel
  // callback, if non-null, is invoked at most once when CancelTask/CancelAll
  // targets this task while it is still cancelable.
  QString RegisterTask(const BackgroundTaskSnapshot& snapshot,
                       std::function<void()>        cancel_callback = nullptr);
  // Update progress/title/detail for a running task. No-op if the id is
  // unknown or the task already reached a terminal state.
  void UpdateTask(const QString& id, const QString& title, const QString& detail,
                  int progress_percent);
  // Change a task's state (e.g. Running -> Canceling). No-op if unknown.
  void UpdateTaskState(const QString& id, BackgroundTaskState state);
  // Move a task to a terminal state, keep it in the recent list, and clear its
  // cancel callback (a finished task cannot be canceled). `detail` is written
  // only when non-null. No-op if the id is unknown.
  void FinishTask(const QString& id, BackgroundTaskState final_state,
                  const QString& detail = QString());

 signals:
  void TasksChanged();

 private:
  struct TaskRecord {
    BackgroundTaskSnapshot snapshot_;
    std::function<void()>  cancel_callback_;
    bool                   cancel_invoked_ = false;
  };

  TaskRecord*       Find(const QString& id);
  const TaskRecord* Find(const QString& id) const;
  static auto ToVariantMap(const TaskRecord& record) -> QVariantMap;
  static auto KindToString(BackgroundTaskKind kind) -> QString;
  static auto StateToString(BackgroundTaskState state) -> QString;
  static auto ShutdownPolicyToString(BackgroundTaskShutdownPolicy policy) -> QString;
  static auto CapabilityToString(InteractionCapability capability) -> QString;
  static auto IsTerminal(BackgroundTaskState state) -> bool;
  static auto IsActive(BackgroundTaskState state) -> bool;
  void PruneFinished();
  void EmitChanged();

  std::vector<TaskRecord> tasks_;
  quint64                  next_id_ = 0;
};

}  // namespace alcedo::ui