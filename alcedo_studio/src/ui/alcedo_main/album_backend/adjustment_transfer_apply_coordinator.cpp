//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp"

#include <QVariantList>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>

#include "app/adjustment_transfer_service.hpp"
#include "app/document_transfer_planner.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
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

AdjustmentTransferApplyCoordinator::AdjustmentTransferApplyCoordinator(ProjectModule* project,
                                                                       LibraryModule* library,
                                                                       QObject*       parent)
    : QObject(parent), project_(project), library_(library) {}

auto AdjustmentTransferApplyCoordinator::ApplyToTargets(
    const alcedo::AdjustmentTransferPackage& package,
    const std::vector<sl_element_id_t>& target_ids, const QString& version_display_name)
    -> QVariantMap {
  auto pipeline_service = project_->handler().pipeline_service();
  if (!pipeline_service) {
    return ErrorResult(Tr("Pipeline service is unavailable."));
  }

  alcedo::AdjustmentApplyResult result;
  for (sl_element_id_t element_id : target_ids) {
    if (element_id == 0) {
      continue;
    }

    std::shared_ptr<PipelineGuard> guard;
    try {
      guard = pipeline_service->LoadEditorPipeline(element_id);
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
      pipeline_service->SavePipeline(guard);
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
        pipeline_service->SavePipeline(guard);
        continue;
      }
      alcedo::DocumentTransferPasteOptions options;
      auto paste_result = alcedo::AdjustmentTransferService::PasteAsRootRelativeVersion(
          *graph, *guard->root_document_, package, version_display_name.toStdString(), options);
      if (paste_result.pasted) {
        std::string rebuild_error;
        if (!pipeline_service->RebuildActiveEditorPipeline(guard, &rebuild_error)) {
          restore_prior();
          result.failures_.push_back({element_id, rebuild_error.empty()
                                                      ? "Failed to rebuild pasted pipeline"
                                                      : std::move(rebuild_error)});
        } else {
          guard->serialized_state_needs_writeback_ = true;
          std::string persistence_error;
          if (!pipeline_service->PersistEditorHistoryState(
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
    pipeline_service->SavePipeline(guard);
  }

  pipeline_service->Sync();
  RefreshAppliedTargets(result);

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

void AdjustmentTransferApplyCoordinator::RefreshAppliedTargets(
    const alcedo::AdjustmentApplyResult& result) {
  bool hdr_metadata_dirty = false;
  for (sl_element_id_t element_id : result.applied_ids_) {
    RefreshOneTarget(element_id, &hdr_metadata_dirty);
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
}

void AdjustmentTransferApplyCoordinator::RefreshOneTarget(sl_element_id_t element_id,
                                                          bool*           hdr_metadata_dirty) {
  auto             thumbnail_service = project_->handler().thumbnail_service();
  const auto*      item              = library_->FindAlbumItem(element_id);
  const image_id_t image_id          = item != nullptr ? item->image_id : 0;
  if (image_id != 0) {
    try {
      auto pipeline_service = project_->handler().pipeline_service();
      if (pipeline_service) {
        auto guard = pipeline_service->LoadPipeline(element_id);
        if (guard && guard->document_) {
          const auto*               drt = guard->document_->Drt();
          const std::optional<bool> is_hdr =
              drt != nullptr ? std::optional<bool>(IsHdrExportEncoding(*drt)) : std::nullopt;
          pipeline_service->SavePipeline(guard);
          if (!is_hdr.has_value()) {
            throw std::runtime_error("Adjustment transfer target has no DRT node");
          }
          library_->PersistImageHdrFlag(element_id, image_id, *is_hdr);
          if (hdr_metadata_dirty != nullptr) {
            *hdr_metadata_dirty = true;
          }
        }
      }
    } catch (...) {
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
