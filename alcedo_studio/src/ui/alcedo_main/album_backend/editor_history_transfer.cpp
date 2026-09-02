//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_transfer.hpp"

#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "app/document_transfer.hpp"
#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/version_ref.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {
namespace {

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) *error = std::move(message);
  return false;
}

}  // namespace

EditorHistoryTransfer::EditorHistoryTransfer(EditorHistoryState& state) : state_(state) {}

namespace {

struct LivePastePriorState {
  alcedo::CommitGraph graph;  // includes logical head on active Version
  alcedo::MiniGitWorkingSelection selection;
  bool dirty = false;
  bool serialized = false;
  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  std::unordered_map<std::string, alcedo::EditorAdjustmentOperatorState> pending;
  bool recovered = false;
  std::optional<alcedo::PipelineDocument> document;
  std::optional<alcedo::EditorRenderReason> published_reason;
};

auto CaptureLivePastePrior(HistoryWorkingState& state, EditorHistoryState& history_state)
    -> LivePastePriorState {
  LivePastePriorState prior;
  prior.graph = *state.pipeline_guard->commit_graph_;
  prior.selection = state.history->WorkingSelection();
  prior.dirty = state.pipeline_guard->dirty_;
  prior.serialized = state.pipeline_guard->serialized_state_needs_writeback_;
  prior.snapshot = state.committed_snapshot;
  prior.pending = state.pending_before;
  prior.recovered = state.recovered_head;
  prior.published_reason = history_state.LastPublishedRenderReason();
  if (state.pipeline_guard->document_) {
    prior.document = alcedo::ClonePipelineDocument(*state.pipeline_guard->document_);
  }
  return prior;
}

void RestoreLivePastePrior(HistoryWorkingState& state, EditorHistoryState& history_state,
                           const LivePastePriorState& prior) {
  *state.pipeline_guard->commit_graph_ = prior.graph;
  state.history->PublishWorkingSelection(prior.selection);
  state.pipeline_guard->dirty_ = prior.dirty;
  state.pipeline_guard->serialized_state_needs_writeback_ = prior.serialized;
  state.committed_snapshot = prior.snapshot;
  state.pending_before = prior.pending;
  state.recovered_head = prior.recovered;
  history_state.RecordPublishedRenderReason(prior.published_reason);
  if (prior.document.has_value() && state.pipeline_guard->document_) {
    if (state.pipeline_guard->pipeline_) {
      std::unique_lock<std::mutex> render_lock(state.pipeline_guard->pipeline_->GetRenderLock());
      *state.pipeline_guard->document_ = alcedo::ClonePipelineDocument(*prior.document);
      state.pipeline_guard->pipeline_->SetPipelineDocument(state.pipeline_guard->document_, false);
    } else {
      *state.pipeline_guard->document_ = alcedo::ClonePipelineDocument(*prior.document);
    }
  }
}

}  // namespace

auto EditorHistoryTransfer::PasteLiveRootRelativeVersion(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package, std::string version_display_name,
    alcedo::AdjustmentPasteResult* result, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (result == nullptr) return SetError(error, "Paste result storage is required");
  *result = {};
  if (package.Empty()) return SetError(error, "Adjustment transfer package is empty");
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history ||
      !state->journal) {
    return SetError(error, "Editor live paste requires a complete history state");
  }
  if (!state->pipeline_guard->pipeline_) {
    return SetError(error, "Editor live paste requires a live pipeline executor");
  }
  if (!state->pipeline_guard->document_) {
    return SetError(error, "Editor live paste requires a live pipeline document");
  }

  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto prior_version_id = graph.GetActiveVersionId();
  const auto prior = CaptureLivePastePrior(*state, state_);

  if (!state->pipeline_guard->root_document_) {
    return SetError(error, "Editor live paste requires an immutable root document");
  }

  std::optional<alcedo::MaskStore> owned_mask_store;
  alcedo::DocumentTransferPasteOptions options;
  if (!package.mask_assets_.empty()) {
    if (state->mask_store != nullptr) {
      options.source_mask_store = state->mask_store;
      options.target_mask_store = state->mask_store;
    } else {
      owned_mask_store.emplace(alcedo::DefaultProductMaskStoreRoot());
      options.source_mask_store = &*owned_mask_store;
      options.target_mask_store = &*owned_mask_store;
    }
  }

  alcedo::PreparedDocumentPaste prepared;
  try {
    prepared = alcedo::PrepareDocumentPaste(package, *state->pipeline_guard->root_document_,
                                            options);
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }

  const auto expected_before_version = graph.GetImageEditState();
  alcedo::version_ref_id_t new_version_id{};
  try {
    new_version_id =
        graph.CreateVersionRefAtRoot(UniqueVersionName(graph, std::move(version_display_name)));
    graph.SetActiveVersionId(new_version_id);
  } catch (const std::exception& ex) {
    RestoreLivePastePrior(*state, state_, prior);
    return SetError(error, ex.what());
  }
  if (!state->history->SelectVersion(new_version_id, error)) {
    RestoreLivePastePrior(*state, state_, prior);
    return false;
  }

  bool version_persisted = false;
  alcedo::ImageEditState persisted_version_state{};
  if (auto pipeline_service = state_.PipelineMapper()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(
            state->pipeline_guard, expected_before_version, &persistence_error)) {
      RestoreLivePastePrior(*state, state_, prior);
      return SetError(error, persistence_error.empty() ? "Paste Version persistence failed"
                                                       : persistence_error);
    }
    version_persisted       = true;
    persisted_version_state = graph.GetImageEditState();
  }

  bool wal_published = false;
  auto rollback_after_version = [&]() -> bool {
    RestoreLivePastePrior(*state, state_, prior);
    if (version_persisted) {
      if (auto pipeline_service = state_.PipelineMapper()) {
        std::string persistence_error;
        if (!pipeline_service->PersistEditorHistoryState(
                state->pipeline_guard, persisted_version_state, &persistence_error)) {
          return SetError(error, persistence_error.empty()
                                     ? "Paste Version persistence rollback failed"
                                     : persistence_error);
        }
      }
    }
    if (wal_published && state->journal != nullptr) {
      std::string truncate_error;
      if (!state->journal->TruncateMaterialized(&truncate_error)) {
        return SetError(error, truncate_error.empty() ? "Paste WAL rollback failed"
                                                      : truncate_error);
      }
    }
    return true;
  };

  state->committed_snapshot = state->root_snapshot;
  state->pending_before.clear();

  {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    *state->pipeline_guard->document_ =
        alcedo::ClonePipelineDocument(*state->pipeline_guard->root_document_);
    alcedo::PipelineHistoryApplyContext context;
    context.mask_store = options.target_mask_store;
    if (!alcedo::ApplyPipelineEditBatch(*state->pipeline_guard->document_, prepared.batch,
                                        alcedo::PipelineEditApplyDirection::Forward, error,
                                        context)) {
      (void)rollback_after_version();
      return false;
    }
    state->pipeline_guard->pipeline_->SetPipelineDocument(state->pipeline_guard->document_, false);
    if (!alcedo::RemirrorCurrentPanelFromDocument(*state->pipeline_guard->pipeline_,
                                                  *state->pipeline_guard->document_, error)) {
      (void)rollback_after_version();
      return false;
    }
  }

  const auto prepared_edit = state->history->PrepareAppendEdit(prepared.batch);
  if (!prepared_edit.ready) {
    if (!rollback_after_version()) {
      return false;
    }
    return SetError(error, prepared_edit.error.empty() ? "Paste edit prepare failed"
                                                       : prepared_edit.error);
  }
  const auto appended = state->history->PublishPreparedEdit(prepared_edit);
  if (!appended.committed) {
    if (!rollback_after_version()) {
      return false;
    }
    return SetError(error, appended.error.empty() ? "Paste WAL append failed" : appended.error);
  }
  wal_published = true;

  {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    if (!MakeAdjustmentSnapshotFromLivePipeline(*state->pipeline_guard->pipeline_,
                                                &state->committed_snapshot, error)) {
      (void)rollback_after_version();
      return false;
    }
  }
  state_.RecordPublishedRenderReason(alcedo::RenderReasonForBatch(prepared.batch));
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->recovered_head = false;

  result->pasted = true;
  result->new_version_id = new_version_id;
  result->prior_version_id = prior_version_id;
  result->new_head = state->history->working_head().value_or(alcedo::commit_hash_t{});
  return true;
}

auto EditorHistoryTransfer::CancelLivePaste(const alcedo::EditorHistoryGuardHandle& guard,
                                            const alcedo::version_ref_id_t& prior_version_id,
                                            const alcedo::version_ref_id_t& paste_version_id,
                                            std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    return SetError(error, "Editor history graph is unavailable");
  }
  if (prior_version_id == alcedo::version_ref_id_t{}) {
    return SetError(error, "Paste cancel requires the prior Version id");
  }
  if (paste_version_id == alcedo::version_ref_id_t{}) {
    return SetError(error, "Paste cancel requires the paste Version id");
  }

  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  const auto prior_select = state->history->WorkingSelection();
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending = state->pending_before;
  const bool prior_recovered = state->recovered_head;

  auto restore_working = [&] {
    // Restoring the graph restores logical head; no separate head field on the guard.
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_select);
    state->pipeline_guard->dirty_ = prior_dirty;
    state->pipeline_guard->serialized_state_needs_writeback_ = prior_serialized;
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
  };

  try {
    graph.SetActiveVersionId(prior_version_id);
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
  if (!state->history->SelectVersion(prior_version_id, error)) {
    restore_working();
    return false;
  }

  alcedo::EditorRenderAdjustmentSnapshot restored_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph, graph.GetActiveVersionRef().head_commit_hash,
                      &restored_snapshot, error)) {
    restore_working();
    return false;
  }

  if (!graph.RemoveVersionRef(paste_version_id)) {
    restore_working();
    return SetError(error, "Paste Version could not be removed after cancel");
  }

  if (auto pipeline_service = state_.PipelineMapper()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                     graph_before.GetImageEditState(),
                                                     &persistence_error)) {
      restore_working();
      return SetError(error, persistence_error.empty() ? "Paste cancel persistence failed"
                                                      : persistence_error);
    }
  }

  if (state->pipeline_guard->pipeline_) {
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    if (!alcedo::ApplyEditorAdjustmentSnapshot(*state->pipeline_guard->pipeline_, restored_snapshot,
                                               error)) {
      restore_working();
      return false;
    }
  }

  // Paste commits were journaled against the abandoned Version; drop the WAL so
  // the next ordinary materialize does not try to fold them onto the prior head.
  if (state->journal && !state->journal->TruncateMaterialized(error)) {
    restore_working();
    return false;
  }
  state->history->PublishWorkingSelection({});

  state->committed_snapshot = std::move(restored_snapshot);
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

}  // namespace alcedo::ui
