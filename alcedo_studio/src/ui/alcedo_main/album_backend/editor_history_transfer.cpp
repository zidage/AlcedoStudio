//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_transfer.hpp"

#include <mutex>
#include <utility>

#include "app/adjustment_transfer_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {

EditorHistoryTransfer::EditorHistoryTransfer(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryTransfer::PasteAdjustments(
    const alcedo::EditorHistoryGuardHandle& guard,
    const alcedo::AdjustmentTransferPackage& package, std::string version_display_name,
    alcedo::AdjustmentPasteResult* result, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port = state_.PipelinePort();
  auto pipeline_service = pipeline_port ? pipeline_port->PipelineService() : nullptr;
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for editor Paste";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  const auto prior_head = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  alcedo::AdjustmentPasteResult paste_result;
  try {
    paste_result = alcedo::AdjustmentTransferService::PasteAsRootRelativeVersion(
        graph, *pipeline_service, guard.element_id, package, std::move(version_display_name));
  } catch (const std::exception& ex) {
    RestoreGraphAndPipeline(*state->pipeline_guard->commit_graph_, graph_before, *pipeline_service,
                            state->pipeline_guard, prior_head, prior_chain, prior_dirty,
                            prior_serialized);
    if (error) *error = ex.what();
    return false;
  }
  if (!paste_result.pasted) {
    if (error) *error = paste_result.error;
    return false;
  }
  std::string rebuild_error;
  if (!pipeline_service->RebuildActiveEditorPipeline(state->pipeline_guard, &rebuild_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    if (error)
      *error =
          rebuild_error.empty() ? "Failed to rebuild pasted pipeline" : std::move(rebuild_error);
    return false;
  }
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = paste_result.new_head;
  state->pipeline_guard->transaction_chain_hash_ = graph.ChainHashForHead(paste_result.new_head);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &state->committed_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    return false;
  }
  if (result) *result = paste_result;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorHistoryTransfer::BeginMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                       const alcedo::AdjustmentTransferPackage& package,
                                       std::string incoming_version_display_name,
                                       alcedo::AdjustmentMergePreview* preview,
                                       std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port = state_.PipelinePort();
  auto pipeline_service = pipeline_port ? pipeline_port->PipelineService() : nullptr;
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for editor Merge";
    return false;
  }
  if (preview == nullptr) {
    if (error) *error = "Merge preview storage is required";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  auto merge_preview = alcedo::AdjustmentTransferService::InitiateMerge(
      graph, *pipeline_service, guard.element_id, package,
      std::move(incoming_version_display_name));
  if (!merge_preview.error.empty()) {
    graph = graph_before;
    if (error) *error = merge_preview.error;
    return false;
  }
  *preview = std::move(merge_preview);
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = graph.GetActiveVersionRef().head_commit_hash;
  state->pipeline_guard->transaction_chain_hash_ =
      graph.ChainHashForHead(state->pipeline_guard->working_head_commit_hash_);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  return true;
}

auto EditorHistoryTransfer::CompleteMerge(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::AdjustmentMergePreview& preview,
    const std::vector<alcedo::AdjustmentMergeResolution>& resolutions,
    alcedo::AdjustmentMergeResult* result, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port = state_.PipelinePort();
  auto pipeline_service = pipeline_port ? pipeline_port->PipelineService() : nullptr;
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for editor Merge";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  const auto prior_head = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  alcedo::AdjustmentMergeResult merge_result;
  try {
    merge_result = alcedo::AdjustmentTransferService::CompleteMerge(graph, *pipeline_service,
                                                                    preview, resolutions);
  } catch (const std::exception& ex) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    if (error) *error = ex.what();
    return false;
  }
  if (!merge_result.merged) {
    if (error) *error = merge_result.error;
    return false;
  }
  std::string rebuild_error;
  if (!pipeline_service->RebuildActiveEditorPipeline(state->pipeline_guard, &rebuild_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    if (error)
      *error =
          rebuild_error.empty() ? "Failed to rebuild merged pipeline" : std::move(rebuild_error);
    return false;
  }
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = merge_result.merge_commit_hash;
  state->pipeline_guard->transaction_chain_hash_ =
      graph.ChainHashForHead(merge_result.merge_commit_hash);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &state->committed_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    return false;
  }
  if (result) *result = merge_result;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorHistoryTransfer::CancelMerge(const alcedo::EditorHistoryGuardHandle& guard,
                                        const alcedo::AdjustmentMergePreview& preview,
                                        std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto mutable_preview = preview;
  alcedo::AdjustmentTransferService::CancelMerge(*state->pipeline_guard->commit_graph_,
                                                 mutable_preview);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  return true;
}

}  // namespace alcedo::ui
