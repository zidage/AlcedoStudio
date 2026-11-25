//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QVariantList>
#include <algorithm>
#include <atomic>
#include <future>
#include <memory>
#include <span>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/adjustment_transfer_service.hpp"
#include "app/document_transfer_planner.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

auto ErrorResult(const QString& message) -> QVariantMap {
  return {{"success", false}, {"message", message}};
}

/// Targets read and written together: one storage session reads their histories, and one
/// transaction writes their results.
constexpr std::size_t kTargetsPerWrite   = 32;

/// Upper bound of the threads that paste and replay the targets of one chunk.
constexpr std::size_t kMaxPrepareThreads = 8;

/// Call @p prepare for each index below @p count on up to @p workers threads, the calling thread
/// included, and return when every call has returned. @p prepare must not throw.
template <typename Prepare>
void ForEachIndexInParallel(std::size_t count, std::size_t workers, const Prepare& prepare) {
  std::atomic<std::size_t> next{0};
  const auto               run = [&] {
    for (std::size_t index = next.fetch_add(1); index < count; index = next.fetch_add(1)) {
      prepare(index);
    }
  };
  // std::thread with an explicit join: Apple libc++ has no std::jthread under -std=c++20.
  std::vector<std::thread> threads;
  struct JoinAll {
    std::vector<std::thread>& threads;
    ~JoinAll() {
      for (auto& thread : threads) thread.join();
    }
  } join_all{threads};
  for (std::size_t worker = 1; worker < std::min(workers, count); ++worker) {
    threads.emplace_back(run);
  }
  run();
}

auto SuccessResult(const QString& message = {}) -> QVariantMap {
  QVariantMap result{{"success", true}};
  if (!message.isEmpty()) {
    result.insert("message", message);
  }
  return result;
}

}  // namespace

AdjustmentTransferApplyCoordinator::AdjustmentTransferApplyCoordinator(
    ProjectModule* project, LibraryModule* library, BackgroundTaskController* background_tasks,
    QObject* parent)
    : QObject(parent), project_(project), library_(library), background_tasks_(background_tasks) {}

auto AdjustmentTransferApplyCoordinator::ApplyToTargets(
    const alcedo::AdjustmentTransferPackage& package,
    const std::vector<sl_element_id_t>& target_ids, const QString& version_display_name,
    std::optional<alcedo::EditCommitAttribution> attribution) -> QVariantMap {
  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    return ErrorResult(Tr("Pipeline service is unavailable."));
  }
  const auto outcome = ApplyPackageToTargets(*pipeline_service, package, target_ids,
                                             version_display_name.toStdString(), attribution);
  return FinishApply(outcome);
}

auto AdjustmentTransferApplyCoordinator::StartApplyToTargets(
    const alcedo::AdjustmentTransferPackage& package, std::vector<sl_element_id_t> target_ids,
    const QString& version_display_name, QString* error,
    std::optional<alcedo::EditCommitAttribution> attribution) -> bool {
  if (running_) {
    if (error != nullptr) *error = Tr("Adjustments are already being pasted.");
    return false;
  }
  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    if (error != nullptr) *error = Tr("Pipeline service is unavailable.");
    return false;
  }

  running_ = true;
  emit RunningChanged();
  if (background_tasks_ != nullptr) {
    const QString reason = Tr("Adjustments are being pasted.");
    BackgroundTaskSnapshot snapshot;
    snapshot.kind_             = BackgroundTaskKind::AdjustmentPaste;
    snapshot.state_            = BackgroundTaskState::Running;
    snapshot.title_            = Tr("Pasting adjustments");
    snapshot.detail_           = Tr("%1 images").arg(static_cast<int>(target_ids.size()));
    snapshot.progress_percent_ = -1;
    snapshot.cancelable_       = false;
    snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::WaitForFinish;
    for (sl_element_id_t id : target_ids) {
      snapshot.affected_targets_.push_back(static_cast<quint64>(id));
    }
    snapshot.locks_ = {
        {InteractionCapability::PasteAdjustments, 0, reason},
        {InteractionCapability::CloseProject, 0, reason},
        {InteractionCapability::DeleteImages, 0, reason},
        {InteractionCapability::SelectEditorImage, 0, reason},
        {InteractionCapability::SwitchWorkspace, 0, reason},
    };
    task_id_ = background_tasks_->RegisterTask(snapshot);
  }

  QPointer<AdjustmentTransferApplyCoordinator> self(this);
  std::thread([self, pipeline_service = std::move(pipeline_service), package,
               target_ids = std::move(target_ids), name = version_display_name.toStdString(),
               attribution = std::move(attribution)]() mutable {
    auto outcome = std::make_shared<TargetApplyOutcome>(
        ApplyPackageToTargets(*pipeline_service, package, target_ids, name, attribution));
    QMetaObject::invokeMethod(
        self.data(),
        [self, outcome]() {
          if (!self) {
            return;
          }
          const auto result = self->FinishApply(*outcome);
          const bool failed = !result.value("success").toBool();
          // Readers of the finished task (the automation task tracker) read this result.
          self->finished_task_id_ = self->task_id_;
          self->finished_result_  = result;
          if (self->background_tasks_ != nullptr && !self->task_id_.isEmpty()) {
            self->background_tasks_->FinishTask(
                self->task_id_,
                failed ? BackgroundTaskState::Failed : BackgroundTaskState::Succeeded,
                result.value("message").toString());
            self->task_id_.clear();
          }
          self->running_ = false;
          emit self->RunningChanged();
          emit self->ApplyFinished(result);
        },
        Qt::QueuedConnection);
  }).detach();
  return true;
}

auto AdjustmentTransferApplyCoordinator::ApplyPackageToTargets(
    alcedo::PipelineMgmtService& pipeline_service, const alcedo::AdjustmentTransferPackage& package,
    const std::vector<sl_element_id_t>& target_ids, const std::string& version_display_name,
    const std::optional<alcedo::EditCommitAttribution>& attribution) -> TargetApplyOutcome {
  TargetApplyOutcome outcome;
  auto&              result = outcome.result_;

  // A target is pasted once; a repeated id is the same photo.
  std::vector<sl_element_id_t>        ids;
  std::unordered_set<sl_element_id_t> seen;
  for (const auto element_id : target_ids) {
    if (element_id != 0 && seen.insert(element_id).second) {
      ids.push_back(element_id);
    }
  }

  const std::size_t workers =
      std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, kMaxPrepareThreads);
  const auto chunk_at = [&ids](std::size_t begin) {
    return std::span<const sl_element_id_t>(ids).subspan(
        begin, std::min(kTargetsPerWrite, ids.size() - begin));
  };
  const auto load = [&pipeline_service](std::span<const sl_element_id_t> chunk) {
    try {
      return pipeline_service.LoadHistorySnapshots(chunk);
    } catch (const std::exception& e) {
      // A failed storage query fails every target of the chunk.
      std::vector<alcedo::ImageHistorySnapshotRead> reads(chunk.size());
      for (std::size_t index = 0; index < chunk.size(); ++index) {
        reads[index].element_id = chunk[index];
        reads[index].error      = e.what();
      }
      return reads;
    }
  };

  // The histories of the next chunk load while the current chunk pastes and writes.
  std::future<std::vector<alcedo::ImageHistorySnapshotRead>> next_bases;
  if (!ids.empty()) {
    next_bases = std::async(std::launch::async, load, chunk_at(0));
  }
  for (std::size_t begin = 0; begin < ids.size(); begin += kTargetsPerWrite) {
    const auto chunk = chunk_at(begin);
    auto       bases = next_bases.get();
    if (begin + kTargetsPerWrite < ids.size()) {
      next_bases = std::async(std::launch::async, load, chunk_at(begin + kTargetsPerWrite));
    }

    struct PreparedTarget {
      std::optional<alcedo::PreparedHistoryWrite> write;
      std::optional<TargetEditState>              edit_state;
      std::string                                 error;
    };
    // Each target is pasted on a private copy of its stored history, on several threads. Storage
    // and every other reader see the targets unchanged until PersistHistoryWrites writes the
    // results of the chunk in one transaction, so a failure at any step leaves nothing to restore.
    std::vector<PreparedTarget> prepared(chunk.size());
    ForEachIndexInParallel(chunk.size(), workers, [&](std::size_t index) {
      auto& target = prepared[index];
      auto& base   = bases[index];
      if (!base.snapshot.has_value()) {
        target.error = base.error;
        return;
      }
      try {
        CommitGraph                          graph = *base.snapshot->graph_;
        // A root-relative Paste creates both immutable commits and a named Version.
        alcedo::DocumentTransferPasteOptions options;
        auto paste_result = alcedo::AdjustmentTransferService::PasteAsRootRelativeVersion(
            graph, base.snapshot->root_->document, package, version_display_name, options,
            attribution);
        if (!paste_result.pasted) {
          target.error = paste_result.error;
          return;
        }
        // The pasted Version is active and its head is the new commit, so the target is edited.
        const auto& version = graph.GetActiveVersionRef();
        if (version.head_commit_hash.has_value()) {
          if (const auto* head = graph.FindCommit(*version.head_commit_hash)) {
            target.edit_state = TargetEditState{static_cast<std::int64_t>(head->GetCreatedAtNs()),
                                                version.display_name};
          }
        }
        target.write = alcedo::PipelineMgmtService::PrepareHistoryWrite(*base.snapshot, graph);
      } catch (const std::exception& e) {
        target.error = e.what();
      }
    });

    std::vector<alcedo::PreparedHistoryWrite> writes;
    std::vector<std::size_t>                  write_index(chunk.size(), chunk.size());
    for (std::size_t index = 0; index < chunk.size(); ++index) {
      if (prepared[index].write.has_value()) {
        write_index[index] = writes.size();
        writes.push_back(std::move(*prepared[index].write));
      }
    }
    std::vector<alcedo::HistoryWriteResult> written;
    try {
      written = pipeline_service.PersistHistoryWrites(writes);
    } catch (const std::exception& e) {
      written.assign(writes.size(), alcedo::HistoryWriteResult{});
      for (auto& entry : written) {
        entry.error = e.what();
      }
    }

    for (std::size_t index = 0; index < chunk.size(); ++index) {
      const auto element_id = chunk[index];
      auto&      target     = prepared[index];
      if (write_index[index] == chunk.size()) {
        result.failures_.push_back({element_id, target.error});
        continue;
      }
      const auto& write = written[write_index[index]];
      if (!write.snapshot) {
        result.failures_.push_back({element_id, write.error});
        continue;
      }
      result.applied_ids_.push_back(element_id);
      if (target.edit_state.has_value()) {
        outcome.edit_state_by_target_[element_id] = std::move(*target.edit_state);
      }
      // The planner requires a DRT node on the target, so the pasted document always has one.
      if (const auto* drt = write.snapshot->Document().Drt()) {
        outcome.hdr_by_target_[element_id] = IsHdrExportEncoding(*drt);
      }
    }
  }
  return outcome;
}

auto AdjustmentTransferApplyCoordinator::FinishApply(const TargetApplyOutcome& outcome)
    -> QVariantMap {
  const auto& result             = outcome.result_;
  bool        hdr_metadata_dirty = false;
  for (sl_element_id_t element_id : result.applied_ids_) {
    const auto hdr = outcome.hdr_by_target_.find(element_id);
    RefreshOneTarget(element_id, hdr != outcome.hdr_by_target_.end() && hdr->second,
                     &hdr_metadata_dirty);
    if (const auto edit_state = outcome.edit_state_by_target_.find(element_id);
        edit_state != outcome.edit_state_by_target_.end()) {
      library_->SetAlbumItemEditState(element_id, 0, edit_state->second.edited_at_ns_,
                                      QString::fromStdString(edit_state->second.version_name_));
    }
    emit TargetRefreshed(static_cast<uint>(element_id));
  }
  if (hdr_metadata_dirty) {
    if (auto project = project_->handler().project()) {
      try {
        project->GetImagePoolService()->SyncWithStorage();
        QString ignored_error;
        if (project_->handler().PersistCurrentProjectState()) {
          (void)project_->handler().PackageCurrentProjectFiles(&ignored_error);
        }
      } catch (...) {
      }
    }
  }

  QVariantList applied_ids;
  for (sl_element_id_t element_id : result.applied_ids_) {
    applied_ids.push_back(static_cast<uint>(element_id));
  }
  QVariantList failures;
  for (const auto& failure : result.failures_) {
    failures.push_back(QVariantMap{{"elementId", static_cast<uint>(failure.file_id_)},
                                   {"message", QString::fromStdString(failure.message_)}});
  }

  QVariantMap response;
  if (result.applied_ids_.empty() && !result.failures_.empty()) {
    response = ErrorResult(QString::fromStdString(result.failures_.front().message_));
  } else if (!result.failures_.empty()) {
    response = SuccessResult(Tr("Adjustments pasted with some failures."));
  } else {
    response = SuccessResult(Tr("Adjustments pasted."));
  }
  response.insert("appliedCount", static_cast<int>(result.applied_ids_.size()));
  response.insert("unchangedCount", static_cast<int>(result.unchanged_ids_.size()));
  response.insert("failureCount", static_cast<int>(result.failures_.size()));
  response.insert("appliedIds", applied_ids);
  response.insert("failures", failures);
  return response;
}

auto AdjustmentTransferApplyCoordinator::FinishedResult(const QString& task_id) const
    -> std::optional<QVariantMap> {
  if (task_id.isEmpty() || task_id != finished_task_id_) {
    return std::nullopt;
  }
  return finished_result_;
}

void AdjustmentTransferApplyCoordinator::RefreshOneTarget(sl_element_id_t element_id,
                                                          bool            is_hdr,
                                                          bool*           hdr_metadata_dirty) {
  auto             thumbnail_service = project_->handler().thumbnail_service();
  const auto*      item              = library_->FindAlbumItem(element_id);
  const image_id_t image_id          = item != nullptr ? item->image_id : 0;
  // Only a changed HDR flag needs the project metadata write and package.
  if (image_id != 0 && item->is_hdr != is_hdr) {
    library_->PersistImageHdrFlag(element_id, image_id, is_hdr);
    if (hdr_metadata_dirty != nullptr) {
      *hdr_metadata_dirty = true;
    }
  }
  if (thumbnail_service) {
    try {
      thumbnail_service->InvalidateThumbnail(element_id);
    } catch (...) {
    }
  }
  if (image_id != 0) {
    if (!library_->thumbs().RefreshCurrentThumbnail(element_id, image_id)) {
      library_->thumbs().UpdateThumbnailState(element_id, QString(), false, false);
    }
  }
}

}  // namespace alcedo::ui
