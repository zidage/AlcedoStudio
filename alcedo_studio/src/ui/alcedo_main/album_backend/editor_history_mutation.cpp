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
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {

EditorHistoryMutation::EditorHistoryMutation(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryMutation::CaptureAdjustmentBeforePreview(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (state->pending_before.contains(patch.field_key)) return true;

  // Before-values for the WAL come from the derived committed snapshot, not the
  // live executor: interactive preview may have already moved the pipeline, and
  // GetParams float round-trips must not redefine the previous committed edit.
  alcedo::EditorAdjustmentOperatorState before;
  if (!ReadCommittedAdjustmentState(state->committed_snapshot, patch.field_key, &before, error)) {
    return false;
  }
  state->pending_before.emplace(patch.field_key, std::move(before));
  return true;
}

auto EditorHistoryMutation::CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::EditorAdjustmentPatch& patch,
                                             std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto spec = alcedo::ResolveEditorAdjustmentField(patch.field_key);
  if (!spec) {
    if (error) *error = "Unknown editor adjustment field: " + patch.field_key;
    return false;
  }
  const auto before = state->pending_before.find(patch.field_key);
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
  payload.after_enabled = patch.enabled;
  if (after_params.contains("enabled") && after_params.at("enabled").is_boolean()) {
    payload.after_enabled = after_params.at("enabled").get<bool>();
  }

  // Live pipeline is the mutation target; committed_snapshot is derived afterwards.
  if (state->pipeline_guard->pipeline_) {
    alcedo::EditorAdjustmentOperatorState after_state;
    after_state.params  = payload.after_value;
    after_state.enabled = payload.after_enabled;
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    if (!alcedo::ApplyEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_, *spec,
                                                    after_state, error)) {
      return false;
    }
  }

  auto candidate = state->committed_snapshot;
  if (!ApplyCommittedPayloadToSnapshot(&candidate, payload, true, error)) return false;
  const auto prepared = state->history->PrepareAppendEdit(payload);
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    return false;
  }
  const auto append = state->history->PublishPreparedEdit(prepared);
  if (!append.committed) {
    if (error) *error = append.error;
    return false;
  }

  state->committed_snapshot = std::move(candidate);
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pending_before.erase(before);
  state->recovered_head = false;
  return true;
}

namespace {

auto ApplyDerivedSnapshotToLivePipeline(HistoryWorkingState& state,
                                        const alcedo::EditorRenderAdjustmentSnapshot& snapshot,
                                        std::string* error) -> bool {
  if (!state.pipeline_guard || !state.pipeline_guard->pipeline_) return true;
  std::unique_lock<std::mutex> render_lock(state.pipeline_guard->pipeline_->GetRenderLock());
  return alcedo::ApplyEditorAdjustmentSnapshot(*state.pipeline_guard->pipeline_, snapshot, error);
}

}  // namespace

auto EditorHistoryMutation::Undo(const alcedo::EditorHistoryGuardHandle& guard,
                                 std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
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
  auto candidate = state->committed_snapshot;
  if (!ApplyPreparedHeadMoveToSnapshot(&candidate, *state->pipeline_guard->commit_graph_, prepared,
                                       error)) {
    return false;
  }
  if (!ApplyDerivedSnapshotToLivePipeline(*state, candidate, error)) return false;
  const auto published = state->history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) *error = published.error.empty() ? "mini-Git undo publish failed" : published.error;
    return false;
  }
  state->committed_snapshot = std::move(candidate);
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
  auto candidate = state->committed_snapshot;
  if (!ApplyPreparedHeadMoveToSnapshot(&candidate, *state->pipeline_guard->commit_graph_, prepared,
                                       error)) {
    return false;
  }
  if (!ApplyDerivedSnapshotToLivePipeline(*state, candidate, error)) return false;
  const auto published = state->history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) *error = published.error.empty() ? "mini-Git redo publish failed" : published.error;
    return false;
  }
  state->committed_snapshot = std::move(candidate);
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
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto prepared = state->history->PrepareMoveHeadToCommit(commit_id);
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    return false;
  }
  if (prepared.is_noop) return true;
  auto candidate = state->committed_snapshot;
  if (!ApplyPreparedHeadMoveToSnapshot(&candidate, *state->pipeline_guard->commit_graph_, prepared,
                                       error)) {
    return false;
  }
  if (!ApplyDerivedSnapshotToLivePipeline(*state, candidate, error)) return false;
  const auto published = state->history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) {
      *error = published.error.empty() ? "mini-Git head-move publish failed" : published.error;
    }
    return false;
  }
  state->committed_snapshot = std::move(candidate);
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorHistoryMutation::DiscardUnmaterializedChanges(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }

  const auto materialized_head =
      state->pipeline_guard->commit_graph_->GetImageEditState().materialized_head_commit_hash;
  while (state->history->working_head() != materialized_head) {
    const auto prepared = materialized_head.has_value()
                              ? state->history->PrepareMoveHeadToCommit(*materialized_head)
                              : state->history->PrepareUndo();
    if (!prepared.ready) {
      if (error) *error = prepared.error;
      return false;
    }
    if (prepared.is_noop) {
      if (error) *error = "Materialized history head could not be restored";
      return false;
    }
    auto candidate = state->committed_snapshot;
    if (!ApplyPreparedHeadMoveToSnapshot(&candidate, *state->pipeline_guard->commit_graph_,
                                         prepared, error)) {
      return false;
    }
    const auto published = state->history->PublishPreparedHeadMove(prepared);
    if (!published.moved) {
      if (error) *error = published.error.empty() ? "Discard head restore failed" : published.error;
      return false;
    }
    state->committed_snapshot = std::move(candidate);
  }

  if (!ApplyDerivedSnapshotToLivePipeline(*state, state->committed_snapshot, error)) return false;

  if (state->journal && !state->journal->TruncateMaterialized(error)) return false;
  state->history->PublishWorkingSelection({});
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

auto EditorHistoryMutation::CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                            const alcedo::Hash128& version_id,
                                            std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }

  auto& graph              = *state->pipeline_guard->commit_graph_;
  const auto graph_before  = graph;
  const auto prior_select = state->history->WorkingSelection();
  const auto prior_head   = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain  = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty  = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending  = state->pending_before;
  const bool prior_recovered = state->recovered_head;

  auto restore_prior = [&] {
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_select);
    state->pipeline_guard->working_head_commit_hash_ = prior_head;
    state->pipeline_guard->transaction_chain_hash_ = prior_chain;
    state->pipeline_guard->dirty_ = prior_dirty;
    state->pipeline_guard->serialized_state_needs_writeback_ = prior_serialized;
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
  };

  try {
    graph.SetActiveVersionId(version_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  if (!state->history->SelectVersion(version_id, error)) {
    restore_prior();
    return false;
  }

  // PersistEditorHistoryState requires the pipeline guard's working head/chain to match the
  // live graph's active Version. Sync before persistence so checkout from a pasted (or any
  // non-root) Version back to Default does not trip the live-identity guard.
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();

  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph, graph.GetActiveVersionRef().head_commit_hash,
                      &next_snapshot, error)) {
    restore_prior();
    return false;
  }

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

  if (!ApplyDerivedSnapshotToLivePipeline(*state, next_snapshot, error)) {
    restore_prior();
    return false;
  }

  state->committed_snapshot = std::move(next_snapshot);
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

}  // namespace alcedo::ui
