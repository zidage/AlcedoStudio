//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_mutation.hpp"

#include <ctime>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/mask/mask_store.hpp"
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
auto RefreshCommittedSnapshotFromLive(HistoryWorkingState& state, std::string* error,
                                      bool holds_render_lock) -> bool {
  if (!state.pipeline_guard || !state.pipeline_guard->pipeline_) {
    if (error) *error = "Live pipeline unavailable while refreshing committed snapshot";
    return false;
  }
  try {
    std::unique_lock<std::mutex> render_lock;
    if (!holds_render_lock) {
      render_lock = LockLivePipeline(*state.pipeline_guard->pipeline_);
    }
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

auto NodeDisplayName(const PipelineDocument& document, const NodeId& node_id) -> std::string {
  const auto* node = document.Graph().FindNode(node_id);
  if (node == nullptr) {
    return {};
  }
  return std::string{node->DisplayName()};
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

auto ApplyContext(MaskStore* mask_store) -> PipelineHistoryApplyContext {
  PipelineHistoryApplyContext context;
  context.mask_store = mask_store;
  return context;
}

auto ApplyCommitToLiveDocument(HistoryWorkingState& state, const EditCommit& commit, bool backward,
                               MaskStore* mask_store, std::string* error) -> bool {
  const auto use_after        = !backward;
  auto       restore_document = [&]() -> bool {
    if (IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
      try {
        const auto batch = PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
        const auto direction =
            backward ? PipelineEditApplyDirection::Forward : PipelineEditApplyDirection::Inverse;
        return ApplyPipelineEditBatch(*state.pipeline_guard->document_, batch, direction, error,
                                      ApplyContext(mask_store));
      } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return false;
      }
    }
    const auto found = state.document_edit_by_commit.find(commit.GetCommitHash());
    if (found == state.document_edit_by_commit.end()) {
      return true;
    }
    const auto& json = backward ? found->second.after_model_json : found->second.before_model_json;
    return ApplyEditorParameterPatch(*state.pipeline_guard->document_, found->second.target, json,
                                     error);
  };
  if (IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
    try {
      const auto batch = PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
      const auto direction =
          backward ? PipelineEditApplyDirection::Inverse : PipelineEditApplyDirection::Forward;
      if (!ApplyPipelineEditBatch(*state.pipeline_guard->document_, batch, direction, error,
                                  ApplyContext(mask_store))) {
        return false;
      }
    } catch (const std::exception& ex) {
      if (error) *error = ex.what();
      return false;
    }
  } else {
    const auto found = state.document_edit_by_commit.find(commit.GetCommitHash());
    if (found == state.document_edit_by_commit.end()) {
      if (error)
        *error = "History commit has no same-session document target; typed replay requires stored "
                 "batch payload";
      return false;
    }
    const auto& json = backward ? found->second.before_model_json : found->second.after_model_json;
    if (!ApplyEditorParameterPatch(*state.pipeline_guard->document_, found->second.target, json,
                                   error)) {
      return false;
    }
  }
  if (!ApplyHistoryCommitToLivePipeline(*state.pipeline_guard->pipeline_,
                                        *state.pipeline_guard->commit_graph_, commit, use_after,
                                        error)) {
    (void)restore_document();
    return false;
  }
  return true;
}

auto InverseApplyCommitToLiveDocument(HistoryWorkingState& state, const EditCommit& commit,
                                      bool original_backward, MaskStore* mask_store,
                                      std::string* error) -> bool {
  return ApplyCommitToLiveDocument(state, commit, !original_backward, mask_store, error);
}

/// WAL-first same-session head move. Hold the render lock across document reads,
/// history publication, writes and rollback (caller owns the lock).
auto ApplyPreparedHeadMoveOnLivePipeline(HistoryWorkingState&           state,
                                         EditorHistoryState&            history_state,
                                         const MiniGitPreparedHeadMove& prepared,
                                         MaskStore*                     mask_store,
                                         std::string*                   error) -> bool {
  if (!state.pipeline_guard->pipeline_ || !state.pipeline_guard->document_) {
    if (error) *error = "Live pipeline document is unavailable";
    return false;
  }
  const auto prior_selection = state.history->WorkingSelection();
  const auto published       = state.history->PublishPreparedHeadMove(prepared);
  if (!published.moved) {
    if (error) *error = published.error;
    return false;
  }
  std::vector<EditCommit> applied;
  applied.reserve(prepared.traversed_commits.size());
  for (const auto& commit : prepared.traversed_commits) {
    if (!ApplyCommitToLiveDocument(state, commit, prepared.backward, mask_store, error)) {
      std::string restore_error;
      for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        if (!InverseApplyCommitToLiveDocument(state, *it, prepared.backward, mask_store,
                                              &restore_error) &&
            error) {
          *error += "; document restoration failed: " + restore_error;
        }
      }
      std::string abandon_error;
      if (!state.history->AbandonPublishedHeadMove(prepared, prior_selection, &abandon_error) &&
          error) {
        *error += "; history restoration failed: " + abandon_error;
      }
      return false;
    }
    applied.push_back(commit);
  }
  for (const auto& commit : prepared.traversed_commits) {
    const auto found = state.document_edit_by_commit.find(commit.GetCommitHash());
    if (found != state.document_edit_by_commit.end()) {
      ProjectDocumentEdit(state, found->second, prepared.backward);
    }
  }
  if (!RefreshCommittedSnapshotFromLive(state, error, true)) {
    std::string restore_error;
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
      (void)InverseApplyCommitToLiveDocument(state, *it, prepared.backward, mask_store,
                                             &restore_error);
    }
    std::string abandon_error;
    (void)state.history->AbandonPublishedHeadMove(prepared, prior_selection, &abandon_error);
    return false;
  }
  history_state.RecordPublishedRenderReason(RenderReasonForHeadMove(prepared.traversed_commits));
  state.pipeline_guard->dirty_ = true;
  state.pending_before.clear();
  state.pending_document_sequence.clear();
  state.recovered_head = false;
  SyncUnsettledPreviewFlag(state);
  return true;
}

auto PublishAppliedTypedBatch(HistoryWorkingState& state, EditorHistoryState& history_state,
                              const PipelineEditBatch& batch, bool document_already_at_after,
                              MaskStore* mask_store, std::string* error) -> bool {
  if (!document_already_at_after) {
    if (!ApplyPipelineEditBatch(*state.pipeline_guard->document_, batch,
                                PipelineEditApplyDirection::Forward, error,
                                ApplyContext(mask_store))) {
      return false;
    }
  }
  const auto prior_selection = state.history->WorkingSelection();
  const auto prepared        = state.history->PrepareAppendEdit(batch);
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    if (!document_already_at_after) {
      (void)ApplyPipelineEditBatch(*state.pipeline_guard->document_, batch,
                                   PipelineEditApplyDirection::Inverse, error,
                                   ApplyContext(mask_store));
    }
    return false;
  }
  const auto append = state.history->PublishPreparedEdit(prepared);
  if (!append.committed) {
    if (error) *error = append.error;
    if (!document_already_at_after) {
      (void)ApplyPipelineEditBatch(*state.pipeline_guard->document_, batch,
                                   PipelineEditApplyDirection::Inverse, error,
                                   ApplyContext(mask_store));
    }
    return false;
  }
  if (!append.commit.has_value()) {
    if (error) *error = "Published typed edit is missing the commit object";
    std::string abandon_error;
    (void)state.history->AbandonPublishedEdit(prepared, prior_selection, &abandon_error);
    if (!document_already_at_after) {
      (void)ApplyPipelineEditBatch(*state.pipeline_guard->document_, batch,
                                   PipelineEditApplyDirection::Inverse, error,
                                   ApplyContext(mask_store));
    }
    return false;
  }
  if (!ApplyHistoryCommitToLivePipeline(*state.pipeline_guard->pipeline_,
                                        *state.pipeline_guard->commit_graph_, *append.commit, true,
                                        error)) {
    std::string abandon_error;
    (void)state.history->AbandonPublishedEdit(prepared, prior_selection, &abandon_error);
    if (!document_already_at_after) {
      (void)ApplyPipelineEditBatch(*state.pipeline_guard->document_, batch,
                                   PipelineEditApplyDirection::Inverse, error,
                                   ApplyContext(mask_store));
    }
    return false;
  }
  if (!RefreshCommittedSnapshotFromLive(state, error, true)) {
    std::string abandon_error;
    (void)state.history->AbandonPublishedEdit(prepared, prior_selection, &abandon_error);
    if (!document_already_at_after) {
      (void)ApplyPipelineEditBatch(*state.pipeline_guard->document_, batch,
                                   PipelineEditApplyDirection::Inverse, error,
                                   ApplyContext(mask_store));
    }
    return false;
  }
  history_state.RecordPublishedRenderReason(RenderReasonForBatch(batch));
  state.pipeline_guard->dirty_ = true;
  state.recovered_head         = false;
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

  if (recorded.before_model_json == recorded.after_model_json) {
    ProjectDocumentEdit(*state, recorded, false);
    state->pending_document_sequence.erase(sequence);
    SyncUnsettledPreviewFlag(*state);
    return true;
  }
  const auto restore_before = [&] { return RestoreDocumentFields(*state, {recorded}, error); };
  PipelineEditBatch batch;
  try {
    batch = MakeSetParameterBatch(locked_target, recorded.before_model_json,
                                  recorded.after_model_json, true, true,
                                  NodeDisplayName(*state->pipeline_guard->document_,
                                                   locked_target.node_id));
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    (void)restore_before();
    return false;
  }
  if (!PublishAppliedTypedBatch(*state, state_, batch, true, state->mask_store, error)) {
    (void)restore_before();
    return false;
  }
  ProjectDocumentEdit(*state, recorded, false);
  state->pending_document_sequence.erase(patch.field_key);
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
  return ApplyPreparedHeadMoveOnLivePipeline(*state, state_, prepared, state->mask_store, error);
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
  return ApplyPreparedHeadMoveOnLivePipeline(*state, state_, prepared, state->mask_store, error);
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
  return ApplyPreparedHeadMoveOnLivePipeline(*state, state_, prepared, state->mask_store, error);
}

auto EditorHistoryMutation::CommitPipelineEditBatch(const alcedo::EditorHistoryGuardHandle& guard,
                                                    alcedo::PipelineEditBatch batch,
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
  auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
}

auto EditorHistoryMutation::AddColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                          const alcedo::NodeId& before_node_id,
                                          const alcedo::NodeId& new_id, std::string* error)
    -> bool {
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
  try {
    auto change = CaptureAddColorGradeChange(*state->pipeline_guard->document_, before_node_id,
                                             new_id);
    return PublishAppliedTypedBatch(*state, state_, MakeAddColorGradeBatch(std::move(change)), false,
                                    state->mask_store, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto EditorHistoryMutation::RemoveColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::NodeId& node_id, std::string* error)
    -> bool {
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
  try {
    auto change = CaptureRemoveColorGradeChange(*state->pipeline_guard->document_, node_id);
    return PublishAppliedTypedBatch(*state, state_, MakeRemoveColorGradeBatch(std::move(change)),
                                    false, state->mask_store, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto EditorHistoryMutation::ReconnectColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                                const alcedo::NodeId& node_id,
                                                const alcedo::NodeId& new_predecessor_id,
                                                const alcedo::NodeId& new_successor_id,
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
  auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  try {
    auto change = CaptureReconnectColorGradeChange(*state->pipeline_guard->document_, node_id,
                                                   new_predecessor_id, new_successor_id);
    return PublishAppliedTypedBatch(*state, state_, MakeReconnectColorGradeBatch(std::move(change)),
                                    false, state->mask_store, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto EditorHistoryMutation::RenameColorGrade(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::NodeId& node_id, std::string display_name,
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
  auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->pipeline_guard->document_->Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  auto batch = MakeRenameColorGradeBatch(node_id, std::string{grade->DisplayName()},
                                         std::move(display_name));
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
}

auto EditorHistoryMutation::SetColorGradeEnabled(const alcedo::EditorHistoryGuardHandle& guard,
                                                 const alcedo::NodeId& node_id, bool enabled,
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
  auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->pipeline_guard->document_->Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  auto batch = MakeSetNodeEnabledBatch(node_id, PipelineEditNodeKind::ColorGrade, grade->Enabled(),
                                       enabled);
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
}

auto EditorHistoryMutation::SetColorGradeMix(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::NodeId& node_id, float mix,
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
  auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->pipeline_guard->document_->Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  auto batch = MakeSetNodeMixBatch(node_id, grade->Mix(), mix);
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
}

auto EditorHistoryMutation::AddMask(const alcedo::EditorHistoryGuardHandle& guard,
                                    const alcedo::NodeId& node_id, alcedo::MaskModel mask,
                                    std::uint32_t display_index, std::string* error) -> bool {
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
  const auto mask_id = mask.id;
  auto       json    = MaskModelToJson(mask);
  auto batch = MakeAddMaskBatch(node_id, mask_id, std::move(json), display_index);
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
}

auto EditorHistoryMutation::RemoveMask(const alcedo::EditorHistoryGuardHandle& guard,
                                       const alcedo::NodeId& node_id, const alcedo::MaskId& mask_id,
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
  auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(
      state->pipeline_guard->document_->Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  std::optional<std::uint32_t> index;
  for (std::size_t i = 0; i < grade->MaskCount(); ++i) {
    if (grade->MaskAt(i).id == mask_id) {
      index = static_cast<std::uint32_t>(i);
      break;
    }
  }
  if (!index.has_value()) {
    if (error) *error = "Mask is missing: " + std::string{mask_id.Value()};
    return false;
  }
  auto batch = MakeRemoveMaskBatch(node_id, mask_id, MaskModelToJson(grade->MaskAt(*index)), *index);
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
}

auto EditorHistoryMutation::ReplaceMaskSource(const alcedo::EditorHistoryGuardHandle& guard,
                                              const alcedo::NodeId& node_id,
                                              const alcedo::MaskId& mask_id,
                                              nlohmann::json after_source, std::string* error)
    -> bool {
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
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->pipeline_guard->document_->Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  const auto* mask = grade->FindMask(mask_id);
  if (mask == nullptr) {
    if (error) *error = "Mask is missing: " + std::string{mask_id.Value()};
    return false;
  }
  auto before = MaskModelToJson(*mask).at("source");
  auto batch  = MakeReplaceMaskSourceBatch(node_id, mask_id, std::move(before),
                                          std::move(after_source));
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
}

auto EditorHistoryMutation::ReplaceMaskAsset(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::NodeId& node_id,
                                             const alcedo::MaskId& mask_id,
                                             nlohmann::json after_source,
                                             alcedo::MaskStore& mask_store, std::string* error)
    -> bool {
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
  auto render_lock   = LockLivePipeline(*state->pipeline_guard->pipeline_);
  state->mask_store  = &mask_store;
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->pipeline_guard->document_->Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  const auto* mask = grade->FindMask(mask_id);
  if (mask == nullptr) {
    if (error) *error = "Mask is missing: " + std::string{mask_id.Value()};
    return false;
  }
  auto before = MaskModelToJson(*mask).at("source");
  auto batch  = MakeReplaceMaskAssetBatch(node_id, mask_id, std::move(before),
                                         std::move(after_source));
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
}

auto EditorHistoryMutation::SetMaskField(const alcedo::EditorHistoryGuardHandle& guard,
                                         const alcedo::NodeId& node_id, const alcedo::MaskId& mask_id,
                                         std::string field_key, nlohmann::json after_value,
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
  auto render_lock = LockLivePipeline(*state->pipeline_guard->pipeline_);
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->pipeline_guard->document_->Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  const auto* mask = grade->FindMask(mask_id);
  if (mask == nullptr) {
    if (error) *error = "Mask is missing: " + std::string{mask_id.Value()};
    return false;
  }
  nlohmann::json before;
  if (field_key == "enabled") {
    before = mask->enabled;
  } else if (field_key == "invert") {
    before = mask->invert;
  } else if (field_key == "opacity") {
    before = mask->opacity;
  } else {
    before = mask->display_name;
  }
  auto batch = MakeSetMaskFieldBatch(node_id, mask_id, std::move(field_key), std::move(before),
                                     std::move(after_value));
  return PublishAppliedTypedBatch(*state, state_, batch, false, state->mask_store, error);
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
    if (!ApplyPreparedHeadMoveOnLivePipeline(*state, state_, prepared, state->mask_store, error))
      return false;
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

  if (!RefreshCommittedSnapshotFromLive(*state, error, false)) {
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
