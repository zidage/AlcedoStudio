//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp"

#include <QMetaObject>
#include <QPointer>
#include <QVariantList>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "app/adjustment_transfer_service.hpp"
#include "app/document_transfer_planner.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

auto ErrorResult(const QString& message) -> QVariantMap {
  return {{"success", false}, {"message", message}};
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
    const std::vector<sl_element_id_t>& target_ids, const QString& version_display_name)
    -> QVariantMap {
  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    return ErrorResult(Tr("Pipeline service is unavailable."));
  }
  const auto outcome = ApplyPackageToTargets(*pipeline_service, package, target_ids,
                                             version_display_name.toStdString());
  return FinishApply(outcome);
}

auto AdjustmentTransferApplyCoordinator::StartApplyToTargets(
    const alcedo::AdjustmentTransferPackage& package, std::vector<sl_element_id_t> target_ids,
    const QString& version_display_name, QString* error) -> bool {
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
               target_ids = std::move(target_ids),
               name       = version_display_name.toStdString()]() mutable {
    auto outcome = std::make_shared<TargetApplyOutcome>(
        ApplyPackageToTargets(*pipeline_service, package, target_ids, name));
    QMetaObject::invokeMethod(
        self.data(),
        [self, outcome]() {
          if (!self) {
            return;
          }
          const auto result = self->FinishApply(*outcome);
          const bool failed = !result.value("success").toBool();
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
    const std::vector<sl_element_id_t>& target_ids, const std::string& version_display_name)
    -> TargetApplyOutcome {
  TargetApplyOutcome outcome;
  auto&              result = outcome.result_;
  for (sl_element_id_t element_id : target_ids) {
    if (element_id == 0) {
      continue;
    }

    std::shared_ptr<PipelineGuard> guard;
    try {
      guard = pipeline_service.LoadEditorPipeline(element_id);
    } catch (const std::exception& e) {
      result.failures_.push_back({element_id, e.what()});
      continue;
    }
    if (!guard || !guard->pipeline_) {
      result.failures_.push_back({element_id, "Pipeline was not available."});
      continue;
    }

    // Use the commit graph attached to the pipeline guard.
    CommitGraph* graph = guard->commit_graph_ ? guard->commit_graph_.get() : nullptr;
    if (graph == nullptr) {
      result.failures_.push_back({element_id, "Mini-Git graph was not available for paste."});
      pipeline_service.SavePipeline(guard);
      continue;
    }

    // A root-relative Paste creates both immutable commits and a named Version.
    // That transition cannot be represented by the edit/head-move journal, so
    // publish it through the same guarded graph materialization used by named
    // Version operations.
    // RebuildActiveEditorPipeline swaps in a new document and never changes the prior one, so a
    // restore binds the prior pointer back instead of replaying the prior Version again.
    const auto graph_before_paste = *graph;
    const bool prior_serialized   = guard->serialized_state_needs_writeback_;
    const bool prior_dirty        = guard->dirty_;
    const auto prior_document     = guard->document_;
    auto       restore_prior      = [&] {
      *graph = graph_before_paste;
      if (prior_document && prior_document != guard->document_) {
        std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
        (void)alcedo::BindLivePipelineDocument(*guard, prior_document);
      }
      guard->serialized_state_needs_writeback_ = prior_serialized;
      guard->dirty_                            = prior_dirty;
    };
    try {
      if (!guard->root_document_) {
        restore_prior();
        result.failures_.push_back({element_id, "Target root document was not available."});
        pipeline_service.SavePipeline(guard);
        continue;
      }
      alcedo::DocumentTransferPasteOptions options;
      auto paste_result = alcedo::AdjustmentTransferService::PasteAsRootRelativeVersion(
          *graph, *guard->root_document_, package, version_display_name, options);
      if (paste_result.pasted) {
        std::string rebuild_error;
        if (!pipeline_service.RebuildActiveEditorPipeline(guard, &rebuild_error)) {
          restore_prior();
          result.failures_.push_back({element_id, rebuild_error.empty()
                                                      ? "Failed to rebuild pasted pipeline"
                                                      : std::move(rebuild_error)});
        } else {
          guard->serialized_state_needs_writeback_ = true;
          std::string persistence_error;
          if (!pipeline_service.PersistEditorHistoryState(
                  guard, graph_before_paste.GetImageEditState(), &persistence_error)) {
            restore_prior();
            result.failures_.push_back({element_id, persistence_error.empty()
                                                        ? "Failed to persist pasted Version"
                                                        : std::move(persistence_error)});
          } else {
            // Persist writes the new Version and clears the stale serialized
            // checkpoint. The live pipeline was just rebuilt from that Version,
            // so SavePipeline must store the new checkpoint. Otherwise the next
            // editor open rebuilds the image correctly but projects an empty
            // adjustment snapshot (LUT panel highlights None).
            guard->serialized_state_needs_writeback_ = true;
            guard->dirty_                            = false;
            result.applied_ids_.push_back(element_id);
            // The planner requires a DRT node on the target, so the rebuilt
            // document always carries one.
            if (const auto* drt = guard->document_ ? guard->document_->Drt() : nullptr) {
              outcome.hdr_by_target_[element_id] = IsHdrExportEncoding(*drt);
            }
          }
        }
      } else {
        restore_prior();
        result.failures_.push_back({element_id, paste_result.error});
      }
    } catch (const std::exception& e) {
      restore_prior();
      result.failures_.push_back({element_id, e.what()});
    }
    // SavePipeline writes this target's document and checkpoint, then releases the pin.
    pipeline_service.SavePipeline(guard);
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
  response.insert("failures", failures);
  return response;
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
