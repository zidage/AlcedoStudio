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

void SyncUnsettledPreviewFlag(HistoryWorkingState& state) {
  if (state.pipeline_guard) {
    state.pipeline_guard->unsettled_preview_ = !state.pending_document_sequence.empty();
  }
}

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

/// Read-only UI/log representation of actual normalized Model values. No stage is read or written.
auto HistoryParams(const EditorParameterTarget& target, nlohmann::json params) -> nlohmann::json {
  if (target.field_key == "exposure" && params.contains("exposure_ev")) {
    params["exposure"] = params.at("exposure_ev");
    params.erase("exposure_ev");
  }
  return params;
}

/// Update only the affected panel projection; it never becomes the source of an edit.
void ProjectDocumentEdit(HistoryWorkingState&                          state,
                         const HistoryWorkingState::DocumentFieldEdit& edit, bool backward) {
  const auto& params = backward ? edit.before_model_json : edit.after_model_json;
  UpsertCommittedSnapshot(&state.committed_snapshot, edit.target.field_key,
                          HistoryParams(edit.target, params), true);
  state.committed_snapshot.params_json.clear();
}

/// Restore local before-values in reverse order under the caller's render lock.
/// Report a restoration error and stop; never continue using a substitute pipeline.
auto RestoreDocumentFields(HistoryWorkingState&                                       state,
                           const std::vector<HistoryWorkingState::DocumentFieldEdit>& fields,
                           std::string* error) -> bool {
  for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
    std::string restore_error;
    if (!ApplyEditorParameterPatch(*state.pipeline_guard->document_, it->target,
                                   it->before_model_json, &restore_error)) {
      if (error) *error = "Document parameter restoration failed: " + restore_error;
      return false;
    }
  }
  return true;
}

/// WAL-first same-session head move. Keep only affected values; hold the render lock
/// across document reads, history publication, writes and rollback (caller owns the lock).
auto ApplyPreparedHeadMoveOnLivePipeline(HistoryWorkingState&           state,
                                         const MiniGitPreparedHeadMove& prepared,
                                         std::string*                   error) -> bool {
  if (!state.pipeline_guard->pipeline_ || !state.pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
    return false;
  }
  std::vector<HistoryWorkingState::DocumentFieldEdit> changes;
  changes.reserve(prepared.traversed_commits.size());
  for (const auto& commit : prepared.traversed_commits) {
    const auto found = state.document_edit_by_commit.find(commit.GetCommitHash());
    if (found == state.document_edit_by_commit.end()) {
      if (error)
        *error = "History commit has no same-session document target; typed replay requires NM4";
      return false;
    }
    auto change = found->second;
    change.after_model_json =
        prepared.backward ? found->second.before_model_json : found->second.after_model_json;
    if (!ReadEditorParameterJson(*state.pipeline_guard->document_, change.target,
                                 &change.before_model_json, error))
      return false;
    changes.push_back(std::move(change));
  }
  const auto prior_selection = state.history->WorkingSelection();
  const auto published = state.history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) *error = published.error;
    return false;
  }
  for (const auto& change : changes) {
    if (!ApplyEditorParameterPatch(*state.pipeline_guard->document_, change.target,
                                   change.after_model_json, error)) {
      std::string abandon_error;
      if (!state.history->AbandonPublishedHeadMove(prepared, prior_selection, &abandon_error) &&
          error)
        *error += "; history restoration failed: " + abandon_error;
      (void)RestoreDocumentFields(state, changes, error);
      return false;
    }
  }
  for (const auto& change : changes) ProjectDocumentEdit(state, change, false);
  state.pipeline_guard->dirty_ = true;
  state.pending_before.clear();
  state.pending_document_sequence.clear();
  state.recovered_head = false;
  SyncUnsettledPreviewFlag(state);
  return true;
}

}  // namespace

EditorHistoryMutation::EditorHistoryMutation(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryMutation::CaptureAdjustmentBeforePreview(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::EditorAdjustmentPatch& patch,
    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
    return false;
  }
  if (!alcedo::ResolveEditorAdjustmentField(patch.field_key).has_value()) {
    if (error) *error = "Unknown editor adjustment field: " + patch.field_key;
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

  if (!state->pipeline_guard->pipeline_) {
    if (error) *error = "Live pipeline executor is unavailable";
    return false;
  }
  auto       render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  const auto sequence    = state->pending_document_sequence.find(patch.field_key);
  HistoryWorkingState::DocumentFieldEdit edit;
  if (sequence == state->pending_document_sequence.end()) {
    if (patch.target.owner_kind == alcedo::EditorParameterOwnerKind::Unspecified) {
      auto filled = alcedo::CompleteCurrentPanelParameterTarget(*state->pipeline_guard->document_,
                                                                patch.field_key, error);
      if (!filled.has_value()) return false;
      edit.target = std::move(*filled);
    } else {
      const auto target_error =
          alcedo::DescribeEditorParameterTargetError(patch.target, patch.field_key);
      if (!target_error.empty()) {
        if (error) *error = target_error;
        return false;
      }
      edit.target = patch.target;
    }
    if (!ReadEditorParameterJson(*state->pipeline_guard->document_, edit.target,
                                 &edit.before_model_json, error))
      return false;
  } else {
    edit = sequence->second;
  }
  if (!ApplyEditorParameterPatch(*state->pipeline_guard->document_, edit.target, patch_params,
                                 error)) {
    return false;
  }
  // Only the first successful patch locks a target. Rejected input does not capture state.
  if (sequence == state->pending_document_sequence.end()) {
    state->pending_document_sequence.emplace(patch.field_key, std::move(edit));
  }
  SyncUnsettledPreviewFlag(*state);
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
  if (patch.target.owner_kind != alcedo::EditorParameterOwnerKind::Unspecified) {
    const auto target_error =
        alcedo::DescribeEditorParameterTargetError(patch.target, patch.field_key);
    if (!target_error.empty()) {
      if (error) *error = target_error;
      return false;
    }
  }
  const auto spec = alcedo::ResolveEditorAdjustmentField(patch.field_key);
  if (!spec) {
    if (error) *error = "Unknown editor adjustment field: " + patch.field_key;
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

  if (!state->pipeline_guard->pipeline_) {
    if (error) *error = "Live pipeline executor is unavailable";
    return false;
  }
  auto       render_lock   = LockLivePipeline(*state->pipeline_guard->pipeline_);
  const auto locked_target = sequence->second.target;
  if (!ApplyEditorParameterPatch(*state->pipeline_guard->document_, locked_target, after_params,
                                 error))
    return false;
  HistoryWorkingState::DocumentFieldEdit recorded = sequence->second;
  if (!ReadEditorParameterJson(*state->pipeline_guard->document_, locked_target,
                               &recorded.after_model_json, error))
    return false;

  OrdinaryEditPayload payload;
  payload.operator_type = spec->operator_type;
  payload.stage_name = spec->stage_name;
  payload.field_name = "$operator_params";
  payload.before_value   = HistoryParams(locked_target, recorded.before_model_json);
  payload.after_value    = HistoryParams(locked_target, recorded.after_model_json);
  payload.before_enabled = true;
  payload.after_enabled  = true;

  if (recorded.before_model_json == recorded.after_model_json) {
    ProjectDocumentEdit(*state, recorded, false);
    state->pending_document_sequence.erase(sequence);
    SyncUnsettledPreviewFlag(*state);
    return true;
  }
  const auto restore_before = [&] { return RestoreDocumentFields(*state, {recorded}, error); };
  const auto prepared = state->history->PrepareAppendEdit(payload);
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    (void)restore_before();
    return false;
  }
  const auto append = state->history->PublishPreparedEdit(prepared);
  if (!append.committed) {
    if (error) *error = append.error;
    (void)restore_before();
    return false;
  }
  if (append.commit.has_value()) {
    state->document_edit_by_commit[append.commit->GetCommitHash()] = recorded;
  }
  ProjectDocumentEdit(*state, recorded, false);
  state->pipeline_guard->dirty_ = true;
  state->pending_document_sequence.erase(patch.field_key);
  state->recovered_head = false;
  SyncUnsettledPreviewFlag(*state);
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
  if (!state->pipeline_guard->pipeline_ || !state->pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
    return false;
  }
  auto       render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
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
  if (!state->pipeline_guard->pipeline_ || !state->pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
    return false;
  }
  auto       render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
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
  if (!state->pipeline_guard->pipeline_ || !state->pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
    return false;
  }
  auto       render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
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

  if (!state->pipeline_guard->pipeline_ || !state->pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
    return false;
  }
  auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  {
    for (const auto& [_, edit] : state->pending_document_sequence) {
      if (!ApplyEditorParameterPatch(*state->pipeline_guard->document_, edit.target,
                                     edit.before_model_json, error))
        return false;
      ProjectDocumentEdit(*state, edit, true);
    }
    state->pending_document_sequence.clear();
  }
  const auto materialized_head =
      state->pipeline_guard->commit_graph_->GetImageEditState().materialized_head_commit_hash;
  while (state->history->working_head() != materialized_head) {
    const auto prepared = materialized_head.has_value()
                              ? state->history->PrepareMoveHeadToCommit(*materialized_head)
                              : state->history->PrepareUndo();
    if (!prepared.ready || prepared.is_noop) {
      if (error)
        *error =
            prepared.ready ? "Materialized history head could not be restored" : prepared.error;
      return false;
    }
    if (!ApplyPreparedHeadMoveOnLivePipeline(*state, prepared, error)) return false;
  }

  if (state->journal && !state->journal->TruncateMaterialized(error)) return false;
  state->history->PublishWorkingSelection({});
  state->pipeline_guard->dirty_ = false;
  state->pipeline_guard->serialized_state_needs_writeback_ = false;
  state->pending_before.clear();
  state->pending_document_sequence.clear();
  state->recovered_head = false;
  SyncUnsettledPreviewFlag(*state);
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
