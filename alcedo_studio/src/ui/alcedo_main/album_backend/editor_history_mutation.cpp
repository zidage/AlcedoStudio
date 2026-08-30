//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_mutation.hpp"

#include <ctime>
#include <mutex>
#include <utility>
#include <vector>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {
namespace {

/// Refresh committed_snapshot from the live pipeline after a successful history mutation.
/// Prefer live GetOperator/GetParams over root_snapshot + SnapshotAtHead (plan §4.7).
auto RefreshCommittedSnapshotFromLive(HistoryWorkingState& state, std::string* error) -> bool {
  if (!state.pipeline_guard || !state.pipeline_guard->pipeline_) {
    if (error) *error = "Live pipeline unavailable while refreshing committed snapshot";
    return false;
  }
  try {
    // Read each field via GetOperator/GetParams under the render lock so panel
    // projection matches the live parameter table (not a secondary JSON scrape).
    std::unique_lock<std::mutex> render_lock(state.pipeline_guard->pipeline_->GetRenderLock());
    return MakeAdjustmentSnapshotFromLivePipeline(*state.pipeline_guard->pipeline_,
                                                  &state.committed_snapshot, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto RestoreLivePipelineParams(HistoryWorkingState& state, const nlohmann::json& prior_pipeline)
    -> void {
  if (!state.pipeline_guard || !state.pipeline_guard->pipeline_) {
    return;
  }
  try {
    auto restore_lock = LockLivePipeline(*state.pipeline_guard->pipeline_);
    state.pipeline_guard->pipeline_->ImportPipelineParams(prior_pipeline);
    state.pipeline_guard->pipeline_->SetExecutionStages();
  } catch (...) {
  }
}

auto RestoreLiveDocumentJson(HistoryWorkingState& state, const nlohmann::json& prior_document)
    -> void {
  if (!state.pipeline_guard || !state.pipeline_guard->document_) {
    return;
  }
  try {
    *state.pipeline_guard->document_ = alcedo::PipelineDocument::FromJson(prior_document);
  } catch (...) {
  }
}

auto ApplyDocumentFieldEdits(HistoryWorkingState& state, const std::vector<alcedo::EditCommit>& commits,
                             bool backward, std::string* error) -> bool {
  if (!state.pipeline_guard || !state.pipeline_guard->document_) {
    return true;
  }
  auto& live = *state.pipeline_guard->document_;
  for (const auto& commit : commits) {
    const auto found = state.document_edit_by_commit.find(commit.GetCommitHash());
    if (found == state.document_edit_by_commit.end()) {
      continue;
    }
    const auto& json = backward ? found->second.before_model_json : found->second.after_model_json;
    if (!alcedo::PublishEditorParameterPatch(live, found->second.target, json, error)) {
      return false;
    }
  }
  return true;
}

/// WAL-first head move: publish journal + unique history head, then rebuild the
/// unique live pipeline from defaults + first-parent chain (plan §4.7). On live
/// apply failure, revoke the WAL tail and restore the prior head.
auto ApplyPreparedHeadMoveOnLivePipeline(HistoryWorkingState& state,
                                         const alcedo::MiniGitPreparedHeadMove& prepared,
                                         std::string* error) -> bool {
  const auto prior_selection = state.history->WorkingSelection();
  nlohmann::json prior_pipeline;
  nlohmann::json prior_document;
  if (state.pipeline_guard && state.pipeline_guard->pipeline_) {
    try {
      // Structural head move: wait out Apply (incl. present) before locking.
      auto render_lock = LockLivePipeline(*state.pipeline_guard->pipeline_);
      prior_pipeline   = state.pipeline_guard->pipeline_->ExportPipelineParams();
    } catch (const std::exception& ex) {
      if (error) *error = ex.what();
      return false;
    }
  }
  if (state.pipeline_guard && state.pipeline_guard->document_) {
    prior_document = state.pipeline_guard->document_->ToJson();
  }

  const auto published = state.history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) {
      *error = published.error.empty() ? "mini-Git head-move publish failed" : published.error;
    }
    return false;
  }

  if (state.pipeline_guard && state.pipeline_guard->pipeline_ && state.pipeline_guard->commit_graph_) {
    auto render_lock = LockLivePipeline(*state.pipeline_guard->pipeline_);
    if (!alcedo::ApplyVersionHeadToLivePipeline(*state.pipeline_guard->pipeline_,
                                                *state.pipeline_guard->commit_graph_,
                                                prepared.target_head, error)) {
      render_lock.unlock();
      std::string abandon_error;
      (void)state.history->AbandonPublishedHeadMove(prepared, prior_selection, &abandon_error);
      RestoreLivePipelineParams(state, prior_pipeline);
      RestoreLiveDocumentJson(state, prior_document);
      return false;
    }
  }

  if (!ApplyDocumentFieldEdits(state, prepared.traversed_commits, prepared.backward, error)) {
    std::string abandon_error;
    (void)state.history->AbandonPublishedHeadMove(prepared, prior_selection, &abandon_error);
    RestoreLivePipelineParams(state, prior_pipeline);
    RestoreLiveDocumentJson(state, prior_document);
    return false;
  }

  if (!RefreshCommittedSnapshotFromLive(state, error)) {
    std::string abandon_error;
    (void)state.history->AbandonPublishedHeadMove(prepared, prior_selection, &abandon_error);
    RestoreLivePipelineParams(state, prior_pipeline);
    RestoreLiveDocumentJson(state, prior_document);
    return false;
  }
  state.pipeline_guard->dirty_ = true;
  state.pending_before.clear();
  state.pending_document_sequence.clear();
  state.recovered_head = false;
  return true;
}

}  // namespace

EditorHistoryMutation::EditorHistoryMutation(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryMutation::CaptureAdjustmentBeforePreview(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  const auto target_error =
      alcedo::DescribeEditorParameterTargetError(patch.target, patch.field_key);
  if (!target_error.empty()) {
    if (error) *error = target_error;
    return false;
  }
  if (!alcedo::ResolveEditorAdjustmentField(patch.field_key).has_value()) {
    if (error) *error = "Unknown editor adjustment field: " + patch.field_key;
    return false;
  }
  if (!state->pipeline_guard || !state->pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
    return false;
  }

  nlohmann::json patch_params;
  try {
    patch_params = patch.params_json.empty() ? nlohmann::json::object()
                                             : nlohmann::json::parse(patch.params_json);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  if (!patch_params.is_object()) {
    if (error) *error = "Editor adjustment params must be a JSON object";
    return false;
  }

  auto sequence = state->pending_document_sequence.find(patch.field_key);
  if (sequence == state->pending_document_sequence.end()) {
    HistoryWorkingState::DocumentFieldEdit edit;
    edit.target = patch.target;
    if (!alcedo::ReadEditorParameterJson(*state->pipeline_guard->document_, edit.target,
                                         &edit.before_model_json, error)) {
      return false;
    }
    sequence = state->pending_document_sequence.emplace(patch.field_key, std::move(edit)).first;
  }
  const auto& locked_target = sequence->second.target;
  if (!alcedo::PublishEditorParameterPatch(*state->pipeline_guard->document_, locked_target,
                                           patch_params, error)) {
    return false;
  }

  if (state->pending_before.contains(patch.field_key)) return true;

  // Before-values for the WAL come from the derived committed snapshot, not the
  // live executor: interactive preview may have already moved the pipeline, and
  // GetParams float round-trips must not redefine the previous committed edit.
  alcedo::EditorAdjustmentOperatorState before;
  if (!ReadCommittedAdjustmentState(state->committed_snapshot, patch.field_key, &before, error)) {
    (void)alcedo::PublishEditorParameterPatch(*state->pipeline_guard->document_, locked_target,
                                              sequence->second.before_model_json, nullptr);
    state->pending_document_sequence.erase(patch.field_key);
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
  const auto target_error =
      alcedo::DescribeEditorParameterTargetError(patch.target, patch.field_key);
  if (!target_error.empty()) {
    if (error) *error = target_error;
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
  const auto sequence = state->pending_document_sequence.find(patch.field_key);
  if (sequence == state->pending_document_sequence.end()) {
    if (error) *error = "Settled adjustment has no locked document target";
    return false;
  }
  if (!state->pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
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

  const auto locked_target = sequence->second.target;
  if (!alcedo::PublishEditorParameterPatch(*state->pipeline_guard->document_, locked_target,
                                           after_params, error)) {
    return false;
  }
  nlohmann::json after_model_json;
  if (!alcedo::ReadEditorParameterJson(*state->pipeline_guard->document_, locked_target,
                                       &after_model_json, error)) {
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

  // before == after: end without WAL, commit, or HEAD move.
  if (payload.before_value == payload.after_value &&
      payload.before_enabled == payload.after_enabled) {
    state->pending_before.erase(before);
    state->pending_document_sequence.erase(sequence);
    return true;
  }

  auto restore_before_on_live = [&] {
    if (state->pipeline_guard->document_) {
      (void)alcedo::PublishEditorParameterPatch(*state->pipeline_guard->document_, locked_target,
                                                sequence->second.before_model_json, nullptr);
    }
    if (!state->pipeline_guard->pipeline_) return;
    alcedo::EditorAdjustmentOperatorState before_state;
    before_state.params  = payload.before_value;
    before_state.enabled = payload.before_enabled;
    std::string restore_error;
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    (void)alcedo::ApplyEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_, *spec,
                                                     before_state, &restore_error);
  };

  // WAL-first settled edit: prepare → WAL+history publish → live SetOperator(after).
  // Interactive preview may already have applied after on the live pipeline; WAL
  // failure must restore before. History or SetOperator failure revokes the WAL
  // tail and restores before without clearing earlier recovery records.
  const auto prepared = state->history->PrepareAppendEdit(payload);
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    restore_before_on_live();
    return false;
  }

  const auto prior_selection = state->history->WorkingSelection();
  const auto append = state->history->PublishPreparedEdit(prepared);
  if (!append.committed) {
    if (error) *error = append.error;
    restore_before_on_live();
    return false;
  }

  if (state->pipeline_guard->pipeline_) {
    alcedo::EditorAdjustmentOperatorState after_state;
    after_state.params  = payload.after_value;
    after_state.enabled = payload.after_enabled;
    std::unique_lock<std::mutex> render_lock(state->pipeline_guard->pipeline_->GetRenderLock());
    if (!alcedo::ApplyEditorAdjustmentOperatorState(*state->pipeline_guard->pipeline_, *spec,
                                                    after_state, error)) {
      render_lock.unlock();
      std::string abandon_error;
      (void)state->history->AbandonPublishedEdit(prepared, prior_selection, &abandon_error);
      restore_before_on_live();
      return false;
    }
  }

  if (!RefreshCommittedSnapshotFromLive(*state, error)) {
    std::string abandon_error;
    (void)state->history->AbandonPublishedEdit(prepared, prior_selection, &abandon_error);
    restore_before_on_live();
    return false;
  }
  if (append.commit.has_value()) {
    HistoryWorkingState::DocumentFieldEdit recorded = sequence->second;
    recorded.after_model_json = std::move(after_model_json);
    state->document_edit_by_commit[append.commit->GetCommitHash()] = std::move(recorded);
  }
  state->pipeline_guard->dirty_ = true;
  state->pending_before.erase(before);
  state->pending_document_sequence.erase(patch.field_key);
  state->recovered_head = false;
  return true;
}

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
  return ApplyPreparedHeadMoveOnLivePipeline(*state, prepared, error);
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
  return ApplyPreparedHeadMoveOnLivePipeline(*state, prepared, error);
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
  return ApplyPreparedHeadMoveOnLivePipeline(*state, prepared, error);
}

auto EditorHistoryMutation::DiscardUnmaterializedChanges(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }

  if (state->pipeline_guard->document_) {
    for (const auto& [_, edit] : state->pending_document_sequence) {
      (void)alcedo::PublishEditorParameterPatch(*state->pipeline_guard->document_, edit.target,
                                                edit.before_model_json, nullptr);
    }
  }

  const auto materialized_head =
      state->pipeline_guard->commit_graph_->GetImageEditState().materialized_head_commit_hash;
  std::vector<alcedo::EditCommit> abandoned;
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
    abandoned.insert(abandoned.end(), prepared.traversed_commits.begin(),
                     prepared.traversed_commits.end());
    // Publish head moves first; live params + derived snapshot refresh once below.
    const auto published = state->history->PublishPreparedHeadMove(prepared);
    if (!published.moved) {
      if (error) *error = published.error.empty() ? "Discard head restore failed" : published.error;
      return false;
    }
  }

  if (state->pipeline_guard->pipeline_) {
    auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
    if (!alcedo::ApplyVersionHeadToLivePipeline(*state->pipeline_guard->pipeline_,
                                                *state->pipeline_guard->commit_graph_,
                                                state->history->working_head(), error)) {
      return false;
    }
  }
  if (!ApplyDocumentFieldEdits(*state, abandoned, true, error)) {
    return false;
  }
  if (!RefreshCommittedSnapshotFromLive(*state, error)) return false;

  if (state->journal && !state->journal->TruncateMaterialized(error)) return false;
  state->history->PublishWorkingSelection({});
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->pending_document_sequence.clear();
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
  const bool prior_dirty  = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending  = state->pending_before;
  const bool prior_recovered = state->recovered_head;

  auto restore_prior = [&] {
    // Graph restore rewinds logical head; no separate head field on the guard.
    graph = graph_before;
    state->history->PublishWorkingSelection(prior_select);
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

  // Logical head is graph.GetActiveVersionRef().head after SelectVersion above.

  nlohmann::json prior_pipeline;
  if (state->pipeline_guard->pipeline_) {
    try {
      // Sole ownership: queue on render_lock until the current frame (incl.
      // present) releases the live pipeline, then rebuild under that lock.
      auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
      prior_pipeline   = state->pipeline_guard->pipeline_->ExportPipelineParams();
      if (!alcedo::ApplyVersionHeadToLivePipeline(
              *state->pipeline_guard->pipeline_, graph,
              graph.GetActiveVersionRef().head_commit_hash, error)) {
        state->pipeline_guard->pipeline_->ImportPipelineParams(prior_pipeline);
        state->pipeline_guard->pipeline_->SetExecutionStages();
        restore_prior();
        return false;
      }
    } catch (const std::exception& ex) {
      if (error) *error = ex.what();
      restore_prior();
      return false;
    }
  }

  if (!RefreshCommittedSnapshotFromLive(*state, error)) {
    try {
      if (state->pipeline_guard->pipeline_) {
        auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
        state->pipeline_guard->pipeline_->ImportPipelineParams(prior_pipeline);
        state->pipeline_guard->pipeline_->SetExecutionStages();
      }
    } catch (...) {
    }
    restore_prior();
    return false;
  }

  if (auto pipeline_service = state_.PipelineMapper()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                     graph_before.GetImageEditState(),
                                                     &persistence_error)) {
      try {
        if (state->pipeline_guard->pipeline_) {
          auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
          state->pipeline_guard->pipeline_->ImportPipelineParams(prior_pipeline);
          state->pipeline_guard->pipeline_->SetExecutionStages();
        }
      } catch (...) {
      }
      restore_prior();
      if (error) *error = persistence_error;
      return false;
    }
  }

  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->recovered_head = false;
  return true;
}

}  // namespace alcedo::ui
