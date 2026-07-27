//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_mutation.hpp"

#include <ctime>
#include <mutex>
#include <utility>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {

EditorHistoryMutation::EditorHistoryMutation(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryMutation::CaptureAdjustmentBeforePreview(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (state->pending_before.contains(patch.field_key)) return true;

  alcedo::EditorAdjustmentOperatorState before;
  bool resolved = false;
  for (const auto& committed : state->committed_snapshot.patches) {
    if (committed.field_key != patch.field_key) continue;
    try {
      before.params = committed.params_json.empty() ? nlohmann::json(nullptr)
                                                    : nlohmann::json::parse(committed.params_json);
    } catch (const std::exception& ex) {
      if (error) *error = ex.what();
      return false;
    }
    before.enabled = EnabledForAdjustmentParams(before.params);
    resolved = true;
    break;
  }

  if (!resolved) {
    if (!state->pipeline_guard || !state->pipeline_guard->pipeline_) {
      if (error) *error = "Editor pipeline is unavailable for adjustment capture";
      return false;
    }
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock(),
                                             std::try_to_lock);
    if (render_lock.owns_lock()) {
      if (!alcedo::ReadEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_,
                                                     patch.field_key, &before, error)) {
        return false;
      }
    } else {
      before.params = nlohmann::json::object();
      before.enabled = true;
    }
  }

  state->pending_before.emplace(patch.field_key, std::move(before));
  return true;
}

auto EditorHistoryMutation::CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::EditorAdjustmentPatch& patch,
                                             std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  const auto spec = alcedo::ResolveEditorAdjustmentField(patch.field_key);
  if (!spec) {
    if (error) *error = "Unknown editor adjustment field: " + patch.field_key;
    return false;
  }
  auto before = state->pending_before.find(patch.field_key);
  if (before == state->pending_before.end()) {
    if (error) *error = "Settled adjustment has no captured committed state";
    return false;
  }
  nlohmann::json after_params;
  try {
    after_params = nlohmann::json::parse(patch.params_json);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  if (!after_params.is_object()) {
    if (error) *error = "Editor adjustment params must be a JSON object";
    return false;
  }

  alcedo::OrdinaryEditPayload payload;
  payload.operator_type = spec->operator_type;
  payload.stage_name = spec->stage_name;
  payload.field_name = "$operator_params";
  payload.before_value = before->second.params;
  payload.after_value = after_params;
  payload.before_enabled = before->second.enabled;
  payload.after_enabled = EnabledForAdjustmentParams(after_params);

  if (state->pipeline_guard && state->pipeline_guard->pipeline_) {
    alcedo::EditorAdjustmentOperatorState after{after_params, payload.after_enabled};
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock(),
                                             std::try_to_lock);
    if (render_lock.owns_lock()) {
      if (!alcedo::ApplyEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_, *spec,
                                                      after, error)) {
        return false;
      }
    }
  }

  const auto append = state->history->AppendEdit(std::move(payload));
  if (!append.committed) {
    if (error) *error = append.error;
    return false;
  }
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  UpsertCommittedSnapshot(&state->committed_snapshot, patch.field_key, after_params);
  state->pending_before.erase(before);
  state->recovered_head = false;
  return true;
}

auto EditorHistoryMutation::Undo(const alcedo::EditorHistoryGuardHandle& guard,
                                 std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto prepared = state->history->PrepareUndo();
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    return false;
  }
  if (prepared.is_noop) return true;
  if (!ApplyPreparedHeadMovePipeline(*state->pipeline_guard, &state->committed_snapshot,
                                     *state->pipeline_guard->commit_graph_, prepared, error)) {
    return false;
  }
  const auto published = state->history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) *error = published.error.empty() ? "mini-Git undo publish failed" : published.error;
    return false;
  }
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorHistoryMutation::Redo(const alcedo::EditorHistoryGuardHandle& guard,
                                 std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto prepared = state->history->PrepareRedo();
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    return false;
  }
  if (prepared.is_noop) return true;
  if (!ApplyPreparedHeadMovePipeline(*state->pipeline_guard, &state->committed_snapshot,
                                     *state->pipeline_guard->commit_graph_, prepared, error)) {
    return false;
  }
  const auto published = state->history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) *error = published.error.empty() ? "mini-Git redo publish failed" : published.error;
    return false;
  }
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorHistoryMutation::MoveHeadToCommit(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::commit_hash_t& commit_id,
                                             std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  // Prepare → apply pipeline/snapshot (fallible) → publish journal/head/redo.
  // Failure before publish leaves the full working-state tuple unchanged.
  const auto prepared = state->history->PrepareMoveHeadToCommit(commit_id);
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    return false;
  }
  if (prepared.is_noop) return true;
  if (!ApplyPreparedHeadMovePipeline(*state->pipeline_guard, &state->committed_snapshot,
                                     *state->pipeline_guard->commit_graph_, prepared, error)) {
    return false;
  }
  const auto published = state->history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) {
      *error = published.error.empty() ? "mini-Git head-move publish failed" : published.error;
    }
    return false;
  }
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorHistoryMutation::CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                            const alcedo::Hash128& version_id,
                                            std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port = state_.PipelinePort();
  if (!pipeline_port) {
    if (error) *error = "Editor pipeline port is unavailable for Version checkout";
    return false;
  }
  auto pipeline_service = pipeline_port->PipelineService();
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for Version checkout";
    return false;
  }

  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  // Capture graph + redo before any live mutation. On failure, restore both so
  // SelectVersion cannot erase the prior redo suffix.
  const auto graph_before = graph;
  const auto prior_selection = state->history->WorkingSelection();
  const auto prior_head = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending = state->pending_before;
  const bool prior_recovered = state->recovered_head;

  auto restore_prior = [&] {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->history->PublishWorkingSelection(prior_selection);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
  };

  if (!pipeline_port->CheckoutVersion(guard.element_id, version_id, error)) {
    // CheckoutVersion restores its own graph/pipeline; keep redo selection.
    state->history->PublishWorkingSelection(prior_selection);
    return false;
  }

  if (!state->history->SelectVersion(version_id, error)) {
    restore_prior();
    return false;
  }

  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &next_snapshot, error)) {
    restore_prior();
    return false;
  }

  std::string persistence_error;
  if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                   graph_before.GetImageEditState(),
                                                   &persistence_error)) {
    restore_prior();
    if (error) *error = persistence_error;
    return false;
  }

  // Durable named-ref publication succeeded: empty redo (SelectVersion) and clean flags.
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->committed_snapshot = std::move(next_snapshot);
  state->recovered_head = false;
  return true;
}

}  // namespace alcedo::ui
