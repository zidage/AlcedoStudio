//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"

#include <cstddef>
#include <utility>
#include <vector>

namespace alcedo::ui {

namespace {
// Keep at most this many finished tasks in the recent list so the popover can
// surface recent successes/failures. Active (Queued/Running/Canceling) tasks
// are always retained regardless of this cap.
constexpr size_t kMaxFinishedRetention = 8;
}  // namespace

BackgroundTaskController::BackgroundTaskController(QObject* parent)
    : QObject(parent) {}

QVariantList BackgroundTaskController::Tasks() const {
  QVariantList out;
  out.reserve(static_cast<int>(tasks_.size()));
  for (const auto& record : tasks_) {
    out.append(ToVariantMap(record));
  }
  return out;
}

QVariantMap BackgroundTaskController::PrimaryTask() const {
  // The first active task in registration order is the "primary" one the bar
  // summarizes; if none is active, fall back to the most recent finished task
  // so the bar briefly shows the last result.
  for (const auto& record : tasks_) {
    if (IsActive(record.snapshot_.state_)) {
      return ToVariantMap(record);
    }
  }
  if (!tasks_.empty()) {
    return ToVariantMap(tasks_.back());
  }
  return {};
}

int BackgroundTaskController::RunningCount() const {
  int n = 0;
  for (const auto& record : tasks_) {
    if (IsActive(record.snapshot_.state_)) ++n;
  }
  return n;
}

bool BackgroundTaskController::HasBlockingShutdownTasks() const {
  for (const auto& record : tasks_) {
    if (IsActive(record.snapshot_.state_) &&
        record.snapshot_.shutdown_policy_ == BackgroundTaskShutdownPolicy::CancelAndWait) {
      return true;
    }
  }
  return false;
}

std::vector<ActiveLock> BackgroundTaskController::ActiveLocks() const {
  std::vector<ActiveLock> out;
  for (const auto& record : tasks_) {
    if (!IsActive(record.snapshot_.state_)) {
      continue;
    }
    for (const auto& lock : record.snapshot_.locks_) {
      out.push_back(ActiveLock{record.snapshot_.id_, lock});
    }
  }
  return out;
}

bool BackgroundTaskController::CancelTask(const QString& taskId) {
  auto* record = Find(taskId);
  if (!record) return false;
  if (IsTerminal(record->snapshot_.state_) || !record->snapshot_.cancelable_) {
    return false;
  }
  if (!record->cancel_invoked_) {
    record->cancel_invoked_ = true;
    if (record->cancel_callback_) {
      record->cancel_callback_();
    }
  }
  return true;
}

void BackgroundTaskController::CancelAll() {
  for (auto& record : tasks_) {
    if (IsTerminal(record.snapshot_.state_) || !record.snapshot_.cancelable_) {
      continue;
    }
    if (!record.cancel_invoked_) {
      record.cancel_invoked_ = true;
      if (record.cancel_callback_) {
        record.cancel_callback_();
      }
    }
  }
  // The cancel callbacks reach the owning controllers, which mirror a
  // Canceling state back via UpdateTaskState (and emit there). Emit once more
  // so QML reflects the cancel even if a controller's path did not.
  EmitChanged();
}

QString BackgroundTaskController::RegisterTask(const BackgroundTaskSnapshot& snapshot,
                                               std::function<void()>        cancel_callback) {
  const QString id = QStringLiteral("bgtask-%1").arg(++next_id_);
  TaskRecord    record;
  record.snapshot_        = snapshot;
  record.snapshot_.id_    = id;
  record.cancel_callback_ = std::move(cancel_callback);
  record.cancel_invoked_  = false;
  tasks_.push_back(std::move(record));
  PruneFinished();
  EmitChanged();
  return id;
}

void BackgroundTaskController::UpdateTask(const QString& id, const QString& title,
                                          const QString& detail, int progress_percent) {
  auto* record = Find(id);
  if (!record || IsTerminal(record->snapshot_.state_)) return;
  record->snapshot_.title_            = title;
  record->snapshot_.detail_           = detail;
  record->snapshot_.progress_percent_ = progress_percent;
  EmitChanged();
}

void BackgroundTaskController::UpdateTaskState(const QString& id, BackgroundTaskState state) {
  auto* record = Find(id);
  if (!record || record->snapshot_.state_ == state) return;
  record->snapshot_.state_ = state;
  EmitChanged();
}

void BackgroundTaskController::FinishTask(const QString& id, BackgroundTaskState final_state,
                                          const QString& detail) {
  auto* record = Find(id);
  if (!record) return;
  record->snapshot_.state_ = final_state;
  if (!detail.isNull()) {
    record->snapshot_.detail_ = detail;
  }
  // A finished task cannot be canceled; drop the callback so a stale capture
  // can never fire after the owning controller is gone.
  record->cancel_callback_ = nullptr;
  PruneFinished();
  EmitChanged();
}

BackgroundTaskController::TaskRecord* BackgroundTaskController::Find(const QString& id) {
  for (auto& record : tasks_) {
    if (record.snapshot_.id_ == id) return &record;
  }
  return nullptr;
}

const BackgroundTaskController::TaskRecord* BackgroundTaskController::Find(
    const QString& id) const {
  for (const auto& record : tasks_) {
    if (record.snapshot_.id_ == id) return &record;
  }
  return nullptr;
}

auto BackgroundTaskController::ToVariantMap(const TaskRecord& record) -> QVariantMap {
  const auto& s = record.snapshot_;
  QVariantMap  m;
  m.insert(QStringLiteral("id"), s.id_);
  m.insert(QStringLiteral("kind"), KindToString(s.kind_));
  m.insert(QStringLiteral("state"), StateToString(s.state_));
  m.insert(QStringLiteral("title"), s.title_);
  m.insert(QStringLiteral("detail"), s.detail_);
  m.insert(QStringLiteral("progressPercent"), s.progress_percent_);
  m.insert(QStringLiteral("cancelable"), s.cancelable_);
  m.insert(QStringLiteral("shutdownPolicy"), ShutdownPolicyToString(s.shutdown_policy_));
  m.insert(QStringLiteral("affectedTargets"), s.affected_targets_);
  // Surface the interaction locks so QML (and debug tooling) can show why a
  // control is disabled. `elementId` is a quint64 so QML reads a real number,
  // not a raw enum int.
  QVariantList locks;
  locks.reserve(static_cast<int>(s.locks_.size()));
  for (const auto& lock : s.locks_) {
    QVariantMap lm;
    lm.insert(QStringLiteral("capability"), CapabilityToString(lock.capability_));
    lm.insert(QStringLiteral("elementId"), static_cast<quint64>(lock.element_id_));
    lm.insert(QStringLiteral("reason"), lock.reason_);
    locks.append(lm);
  }
  m.insert(QStringLiteral("locks"), locks);
  return m;
}

auto BackgroundTaskController::KindToString(BackgroundTaskKind kind) -> QString {
  switch (kind) {
    case BackgroundTaskKind::ImageAnalysis:
      return QStringLiteral("imageAnalysis");
    case BackgroundTaskKind::SemanticGeneration:
      return QStringLiteral("semanticGeneration");
    case BackgroundTaskKind::ModelDownload:
      return QStringLiteral("modelDownload");
    case BackgroundTaskKind::ModelActivation:
      return QStringLiteral("modelActivation");
    case BackgroundTaskKind::Import:
      return QStringLiteral("import");
    case BackgroundTaskKind::Export:
      return QStringLiteral("export");
    case BackgroundTaskKind::EditorSave:
      return QStringLiteral("editorSave");
  }
  return QStringLiteral("unknown");
}

auto BackgroundTaskController::StateToString(BackgroundTaskState state) -> QString {
  switch (state) {
    case BackgroundTaskState::Queued:
      return QStringLiteral("queued");
    case BackgroundTaskState::Running:
      return QStringLiteral("running");
    case BackgroundTaskState::Canceling:
      return QStringLiteral("canceling");
    case BackgroundTaskState::Succeeded:
      return QStringLiteral("succeeded");
    case BackgroundTaskState::Failed:
      return QStringLiteral("failed");
    case BackgroundTaskState::Canceled:
      return QStringLiteral("canceled");
  }
  return QStringLiteral("unknown");
}

auto BackgroundTaskController::ShutdownPolicyToString(BackgroundTaskShutdownPolicy policy)
    -> QString {
  switch (policy) {
    case BackgroundTaskShutdownPolicy::CancelAndWait:
      return QStringLiteral("cancelAndWait");
    case BackgroundTaskShutdownPolicy::WaitForFinish:
      return QStringLiteral("waitForFinish");
    case BackgroundTaskShutdownPolicy::DetachNotAllowed:
      return QStringLiteral("detachNotAllowed");
  }
  return QStringLiteral("unknown");
}

auto BackgroundTaskController::CapabilityToString(InteractionCapability capability) -> QString {
  switch (capability) {
    case InteractionCapability::EditImageDescription:
      return QStringLiteral("editImageDescription");
    case InteractionCapability::EditImageRating:
      return QStringLiteral("editImageRating");
    case InteractionCapability::EditImageRatingReason:
      return QStringLiteral("editImageRatingReason");
    case InteractionCapability::RunImageAnalysis:
      return QStringLiteral("runImageAnalysis");
    case InteractionCapability::CommitImageAnalysisResults:
      return QStringLiteral("commitImageAnalysisResults");
    case InteractionCapability::ChangeImageAnalysisProvider:
      return QStringLiteral("changeImageAnalysisProvider");
    case InteractionCapability::ChangeSemanticModel:
      return QStringLiteral("changeSemanticModel");
    case InteractionCapability::RunSemanticGeneration:
      return QStringLiteral("runSemanticGeneration");
    case InteractionCapability::ChangeModelDownloadSettings:
      return QStringLiteral("changeModelDownloadSettings");
    case InteractionCapability::DeleteImages:
      return QStringLiteral("deleteImages");
    case InteractionCapability::CloseProject:
      return QStringLiteral("closeProject");
    case InteractionCapability::SelectEditorImage:
      return QStringLiteral("selectEditorImage");
    case InteractionCapability::SwitchWorkspace:
      return QStringLiteral("switchWorkspace");
    case InteractionCapability::CheckoutVersion:
      return QStringLiteral("checkoutVersion");
    case InteractionCapability::PasteAdjustments:
      return QStringLiteral("pasteAdjustments");
  }
  return QStringLiteral("unknown");
}

auto BackgroundTaskController::IsTerminal(BackgroundTaskState state) -> bool {
  return state == BackgroundTaskState::Succeeded || state == BackgroundTaskState::Failed ||
         state == BackgroundTaskState::Canceled;
}

auto BackgroundTaskController::IsActive(BackgroundTaskState state) -> bool {
  return state == BackgroundTaskState::Queued || state == BackgroundTaskState::Running ||
         state == BackgroundTaskState::Canceling;
}

void BackgroundTaskController::PruneFinished() {
  size_t finished_count = 0;
  for (const auto& record : tasks_) {
    if (IsTerminal(record.snapshot_.state_)) ++finished_count;
  }
  if (finished_count <= kMaxFinishedRetention) return;
  const size_t        to_remove = finished_count - kMaxFinishedRetention;
  std::vector<size_t> remove_indices;
  for (size_t i = 0; i < tasks_.size() && remove_indices.size() < to_remove; ++i) {
    if (IsTerminal(tasks_[i].snapshot_.state_)) {
      remove_indices.push_back(i);
    }
  }
  // Erase from the highest index down so earlier indices stay valid.
  for (auto it = remove_indices.rbegin(); it != remove_indices.rend(); ++it) {
    tasks_.erase(tasks_.begin() + static_cast<std::ptrdiff_t>(*it));
  }
}

void BackgroundTaskController::EmitChanged() {
  emit TasksChanged();
}

}  // namespace alcedo::ui