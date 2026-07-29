//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_transfer.hpp"

#include <utility>

#include "app/adjustment_transfer_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {

EditorHistoryTransfer::EditorHistoryTransfer(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryTransfer::PasteAdjustments(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::AdjustmentTransferPackage& package,
                                             std::string version_display_name,
                                             alcedo::AdjustmentPasteResult* result,
                                             std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  if (!state->journal || !state->journal->Snapshot().records.empty()) {
    if (error) *error = "Paste requires pending editor changes to finish saving";
    return false;
  }

  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  const auto prior_selection = state->history->WorkingSelection();
  const auto prior_head = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending = state->pending_before;

  const auto paste_result = alcedo::AdjustmentTransferService::PasteAsRootRelativeVersion(
      graph, package, std::move(version_display_name));
  if (!paste_result.pasted) {
    if (error) *error = paste_result.error;
    return false;
  }
  if (!state->history->SelectVersion(paste_result.new_version_id, error)) {
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_selection);
    return false;
  }
  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph, paste_result.new_head, &next_snapshot, error)) {
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_selection);
    return false;
  }

  auto restore_prior = [&] {
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_selection);
    state->pipeline_guard->working_head_commit_hash_ = prior_head;
    state->pipeline_guard->transaction_chain_hash_ = prior_chain;
    state->pipeline_guard->dirty_ = prior_dirty;
    state->pipeline_guard->serialized_state_needs_writeback_ = prior_serialized;
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
  };
  if (auto pipeline_service = state_.PipelineService()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                     graph_before.GetImageEditState(),
                                                     &persistence_error)) {
      restore_prior();
      if (error) *error = persistence_error;
      return false;
    }
  }

  state->committed_snapshot = std::move(next_snapshot);
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->recovered_head = false;
  if (result) *result = paste_result;
  return true;
}

auto EditorHistoryTransfer::BeginMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                       const alcedo::AdjustmentTransferPackage& package,
                                       std::string incoming_version_display_name,
                                       alcedo::AdjustmentMergePreview* preview,
                                       std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (preview == nullptr) {
    if (error) *error = "Merge preview storage is required";
    return false;
  }
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  if (!state->journal || !state->journal->Snapshot().records.empty()) {
    if (error) *error = "Merge requires pending editor changes to finish saving";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  auto merge_preview = alcedo::AdjustmentTransferService::InitiateMerge(
      graph, package, state->committed_snapshot, std::move(incoming_version_display_name));
  if (!merge_preview.error.empty()) {
    graph = graph_before;
    if (error) *error = merge_preview.error;
    return false;
  }
  *preview = std::move(merge_preview);
  return true;
}

auto EditorHistoryTransfer::CompleteMerge(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentMergePreview& preview,
    const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
    alcedo::AdjustmentMergeResult* result, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  if (!state->journal || !state->journal->Snapshot().records.empty()) {
    if (error) *error = "Merge requires pending editor changes to finish saving";
    return false;
  }

  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  const auto prior_selection = state->history->WorkingSelection();
  const auto prior_head = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending = state->pending_before;

  const auto merge_result =
      alcedo::AdjustmentTransferService::CompleteMerge(graph, preview, resolutions);
  if (!merge_result.merged) {
    if (error) *error = merge_result.error;
    return false;
  }
  if (!state->history->SelectVersion(graph.GetActiveVersionId(), error)) {
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_selection);
    return false;
  }
  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph,
                      graph.GetActiveVersionRef().head_commit_hash, &next_snapshot, error)) {
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_selection);
    return false;
  }

  auto restore_prior = [&] {
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_selection);
    state->pipeline_guard->working_head_commit_hash_ = prior_head;
    state->pipeline_guard->transaction_chain_hash_ = prior_chain;
    state->pipeline_guard->dirty_ = prior_dirty;
    state->pipeline_guard->serialized_state_needs_writeback_ = prior_serialized;
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
  };
  if (auto pipeline_service = state_.PipelineService()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                     graph_before.GetImageEditState(),
                                                     &persistence_error)) {
      restore_prior();
      if (error) *error = persistence_error;
      return false;
    }
  }

  state->committed_snapshot = std::move(next_snapshot);
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->recovered_head = false;
  if (result) *result = merge_result;
  return true;
}

auto EditorHistoryTransfer::CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                        const alcedo::AdjustmentMergePreview& preview,
                                        std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto mutable_preview = preview;
  alcedo::AdjustmentTransferService::CancelMerge(*state->pipeline_guard->commit_graph_,
                                                   mutable_preview);
  return true;
}

}  // namespace alcedo::ui
