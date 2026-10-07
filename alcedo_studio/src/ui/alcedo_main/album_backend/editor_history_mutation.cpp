//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_mutation.hpp"

#include <ctime>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "app/editor_adjustment_context.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_panel_projection.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_parameter_write.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/mask/mask_model.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {
namespace {

[[nodiscard]] auto PanelFieldMatchesProjectionNode(const PipelineDocument& document,
                                                    const NodeId& projection_node,
                                                    const EditorParameterTarget& target) -> bool {
  if (projection_node.Empty()) {
    return true;
  }
  if (target.owner_kind == EditorParameterOwnerKind::Document) {
    const auto* develop = document.Develop();
    return develop != nullptr && develop->Id() == projection_node;
  }
  if (target.owner_kind == EditorParameterOwnerKind::DrtPost) {
    const auto* drt = document.Drt();
    if (drt != nullptr && projection_node == drt->Id()) {
      return true;
    }
    return dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(projection_node)) !=
           nullptr;
  }
  return target.node_id == projection_node;
}

auto ProjectPanelFieldsForState(HistoryWorkingState& state, std::string* error) -> bool {
  return ProjectPanelFieldsForDocument(state.document->Document(), &state.panel_projection_node_id,
                                       &state.panel_projection, error);
}

/// Re-read every panel field from the working document after the document changed as a whole
/// (head move, typed batch, Version checkout).
auto RefreshPanelProjectionFromDocument(HistoryWorkingState& state, std::string* error) -> bool {
  try {
    return ProjectPanelFieldsForState(state, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

}  // namespace

auto ProjectPanelFieldsForDocument(const alcedo::PipelineDocument& document,
                                   alcedo::NodeId*                 projection_node_id,
                                   alcedo::EditorPanelProjection* out, std::string* error) -> bool {
  if (!projection_node_id->Empty()) {
    if (document.Graph().FindNode(*projection_node_id) != nullptr) {
      return alcedo::ProjectSelectedNodePanelFields(document, *projection_node_id, 0, out, error);
    }
    // The document no longer holds the panel's node; fall back to current-panel
    // routing until the UI selects another node.
    *projection_node_id = {};
  }
  return alcedo::ProjectCurrentPanelFields(document, 0, out, error);
}

namespace {

auto NodeDisplayName(const PipelineDocument& document, const NodeId& node_id) -> std::string {
  const auto* node = document.Graph().FindNode(node_id);
  if (node == nullptr) {
    return {};
  }
  return std::string{node->DisplayName()};
}

/// Update only the affected panel projection; it never becomes the source of an edit.
/// The projection reads the live document, which already holds the edited value.
void ProjectDocumentEdit(HistoryWorkingState&                          state,
                         const HistoryWorkingState::DocumentFieldEdit& edit) {
  if (!PanelFieldMatchesProjectionNode(state.document->Document(), state.panel_projection_node_id,
                                       edit.target)) {
    return;
  }
  alcedo::EditorPanelFieldPresentation field;
  std::string                          ignore;
  if (alcedo::ReadEditorPanelField(state.document->Document(), edit.target, &field, &ignore)) {
    alcedo::UpsertEditorPanelField(&state.panel_projection, std::move(field));
  }
}

/// Restore local before-values in reverse order.
/// Report a restoration error and stop; never continue using a substitute pipeline.
auto RestoreDocumentFields(HistoryWorkingState&                                       state,
                           const std::vector<HistoryWorkingState::DocumentFieldEdit>& fields,
                           std::string* error) -> bool {
  for (auto it = fields.rbegin(); it != fields.rend(); ++it) {
    std::string restore_error;
    if (!ApplyEditorParameterPatch(state.document->Document(), it->target, it->before_model_json,
                                   &restore_error)) {
      if (error) *error = "Document parameter restoration failed: " + restore_error;
      return false;
    }
  }
  return true;
}

/// Write the before values of the open input sequence back to the working document and close the
/// sequence. No commit records a preview value, so nothing may run on top of it.
auto RestorePendingDocumentSequence(HistoryWorkingState& state, std::string* error) -> bool {
  if (state.pending_document_sequence.empty()) {
    return true;
  }
  std::vector<HistoryWorkingState::DocumentFieldEdit> fields;
  fields.reserve(state.pending_document_sequence.size());
  for (const auto& [_, edit] : state.pending_document_sequence) {
    fields.push_back(edit);
  }
  if (!RestoreDocumentFields(state, fields, error)) {
    return false;
  }
  for (const auto& edit : fields) {
    ProjectDocumentEdit(state, edit);
  }
  state.pending_document_sequence.clear();
  return true;
}

/// Apply one traversed commit to the working document only. The caller restores earlier commits
/// of the same head move when a later one fails.
auto ApplyCommitToLiveDocument(HistoryWorkingState& state, const EditCommit& commit, bool backward,
                               std::string* error) -> bool {
  if (IsPipelineEditBatchJson(commit.GetPayloadJSON())) {
    try {
      const auto batch = PipelineEditBatch::FromJSON(commit.GetPayloadJSON());
      const auto direction =
          backward ? PipelineEditApplyDirection::Inverse : PipelineEditApplyDirection::Forward;
      if (!ApplyPipelineEditBatch(state.document->Document(), batch, direction, error, {})) {
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
    if (!ApplyEditorParameterPatch(state.document->Document(), found->second.target, json, error)) {
      return false;
    }
  }
  return true;
}

auto InverseApplyCommitToLiveDocument(HistoryWorkingState& state, const EditCommit& commit,
                                      bool original_backward, std::string* error) -> bool {
  return ApplyCommitToLiveDocument(state, commit, !original_backward, error);
}

/// WAL-first same-session head move: publish the head move, apply the traversed commits to the
/// working document, and restore both when a later step fails. The session refuses a head move
/// while an input sequence is open; if one still reaches this point, its preview values are
/// restored first, so the moved document holds only committed values.
auto ApplyPreparedHeadMoveOnLivePipeline(HistoryWorkingState&           state,
                                         EditorHistoryState&            history_state,
                                         const MiniGitPreparedHeadMove& prepared,
                                         std::string*                   error) -> bool {
  if (!RestorePendingDocumentSequence(state, error)) {
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
    if (!ApplyCommitToLiveDocument(state, commit, prepared.backward, error)) {
      std::string restore_error;
      for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
        if (!InverseApplyCommitToLiveDocument(state, *it, prepared.backward,
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
      ProjectDocumentEdit(state, found->second);
    }
  }
  if (!RefreshPanelProjectionFromDocument(state, error)) {
    std::string restore_error;
    for (auto it = applied.rbegin(); it != applied.rend(); ++it) {
      (void)InverseApplyCommitToLiveDocument(state, *it, prepared.backward,
                                             &restore_error);
    }
    std::string abandon_error;
    (void)state.history->AbandonPublishedHeadMove(prepared, prior_selection, &abandon_error);
    return false;
  }
  history_state.RecordPublishedRenderReason(RenderReasonForHeadMove(prepared.traversed_commits));
  state.recovered_head = false;
  return true;
}

auto PublishAppliedTypedBatch(HistoryWorkingState& state, EditorHistoryState& history_state,
                              const PipelineEditBatch& batch, bool document_already_at_after,
                              std::string* error) -> bool {
  // The pre-apply snapshot is the exact failure rollback: inverse re-apply is
  // semantically equal but does not preserve node/edge container positions.
  std::optional<PipelineDocument> pre_apply_document;
  const auto                      prior_panel_node = state.panel_projection_node_id;
  if (!document_already_at_after) {
    // Check every actual removal against the live before-state, before any unlock or mutation.
    for (const auto& change : batch.changes) {
      std::vector<GraphValidationError> errors;
      if (const auto* removed_grade = std::get_if<RemoveColorGradeChange>(&change)) {
        errors = state.document->Document().ValidateUserDeletion(removed_grade->node_id);
      } else if (const auto* removed_mask = std::get_if<RemoveMaskChange>(&change)) {
        errors = state.document->Document().ValidateUserDeletion(removed_mask->node_id,
                                                                 removed_mask->mask_id);
      } else if (const auto* topology = std::get_if<NodeGraphTopologyChange>(&change)) {
        for (const auto& removed_node : topology->removed_nodes) {
          auto rejected = state.document->Document().ValidateUserDeletion(
              NodeId{removed_node.node.at("id").get<std::string>()});
          errors.insert(errors.end(), rejected.begin(), rejected.end());
        }
      }
      if (!errors.empty()) {
        if (error) *error = errors.front().message;
        return false;
      }
    }
    pre_apply_document = ClonePipelineDocument(state.document->Document());
    if (!ApplyPipelineEditBatch(state.document->Document(), batch,
                                PipelineEditApplyDirection::Forward, error, {})) {
      return false;
    }
  }
  const auto restore_document = [&]() {
    if (!pre_apply_document.has_value()) {
      return;
    }
    state.document->Document()       = std::move(*pre_apply_document);
    state.panel_projection_node_id   = prior_panel_node;
  };
  const auto prior_selection = state.history->WorkingSelection();
  const auto prepared        = state.history->PrepareAppendEdit(batch);
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    restore_document();
    return false;
  }
  const auto append = state.history->PublishPreparedEdit(prepared);
  if (!append.committed) {
    if (error) *error = append.error;
    restore_document();
    return false;
  }
  if (!append.commit.has_value()) {
    if (error) *error = "Published typed edit is missing the commit object";
    std::string abandon_error;
    (void)state.history->AbandonPublishedEdit(prepared, prior_selection, &abandon_error);
    restore_document();
    return false;
  }
  if (!RefreshPanelProjectionFromDocument(state, error)) {
    std::string abandon_error;
    (void)state.history->AbandonPublishedEdit(prepared, prior_selection, &abandon_error);
    restore_document();
    return false;
  }
  history_state.RecordPublishedRenderReason(RenderReasonForBatch(batch));
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
  if (!alcedo::ResolveEditorAdjustmentField(patch.field_key).has_value()) {
    if (error) *error = "Unknown editor adjustment field: " + patch.field_key;
    return false;
  }
  if (!patch.write.has_value()) {
    if (error) *error = "Typed field write is required";
    return false;
  }

  const auto sequence    = state->pending_document_sequence.find(patch.field_key);
  HistoryWorkingState::DocumentFieldEdit edit;
  if (sequence == state->pending_document_sequence.end()) {
    if (patch.target.owner_kind == alcedo::EditorParameterOwnerKind::Unspecified) {
      if (!state->panel_projection_node_id.Empty()) {
        auto filled = alcedo::CompleteSelectedNodeParameterTarget(
            state->document->Document(), state->panel_projection_node_id, patch.field_key, error);
        if (!filled.has_value()) return false;
        edit.target = std::move(*filled);
      } else {
        auto filled = alcedo::CompleteCurrentPanelParameterTarget(state->document->Document(),
                                                                  patch.field_key, error);
        if (!filled.has_value()) return false;
        edit.target = std::move(*filled);
      }
    } else {
      const auto target_error =
          alcedo::DescribeEditorParameterTargetError(patch.target, patch.field_key);
      if (!target_error.empty()) {
        if (error) *error = target_error;
        return false;
      }
      edit.target = patch.target;
    }
    if (!ReadEditorParameterJson(state->document->Document(), edit.target, &edit.before_model_json,
                                 error))
      return false;
  } else {
    edit = sequence->second;
  }
  // The document is the only parameter store. A rejected write leaves it unchanged.
  if (!ApplyEditorParameterWrite(state->document->Document(), edit.target, *patch.write, error)) {
    return false;
  }
  {
    alcedo::EditorPanelFieldPresentation field;
    std::string                          ignore;
    if (PanelFieldMatchesProjectionNode(state->document->Document(),
                                        state->panel_projection_node_id, edit.target) &&
        alcedo::ReadEditorPanelField(state->document->Document(), edit.target, &field, &ignore)) {
      alcedo::UpsertEditorPanelField(&state->panel_projection, std::move(field));
    }
  }
  // Only the first successful patch locks a target. Rejected input does not capture state.
  if (sequence == state->pending_document_sequence.end()) {
    state->pending_document_sequence.emplace(patch.field_key, std::move(edit));
  }
  return true;
}

auto EditorHistoryMutation::RestoreUnsettledPreview(const alcedo::EditorHistoryGuardHandle& guard,
                                                    bool* live_changed, std::string* error)
    -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (live_changed != nullptr) {
    *live_changed = !state->pending_document_sequence.empty();
  }
  return RestorePendingDocumentSequence(*state, error);
}

auto EditorHistoryMutation::CommitAdjustment(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::EditorAdjustmentPatch& patch,
                                             std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
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
  if (!patch.write.has_value()) {
    if (error) *error = "Typed field write is required";
    return false;
  }

  const auto locked_target = sequence->second.target;
  HistoryWorkingState::DocumentFieldEdit recorded = sequence->second;
  if (!ReadEditorParameterJson(state->document->Document(), locked_target,
                               &recorded.after_model_json, error))
    return false;

  if (recorded.before_model_json == recorded.after_model_json) {
    ProjectDocumentEdit(*state, recorded);
    state->pending_document_sequence.erase(sequence);
    return true;
  }
  const auto restore_before = [&] { return RestoreDocumentFields(*state, {recorded}, error); };
  PipelineEditBatch batch;
  try {
    batch = MakeSetParameterBatch(
        locked_target, recorded.before_model_json, recorded.after_model_json, true, true,
        NodeDisplayName(state->document->Document(), locked_target.node_id));
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    (void)restore_before();
    return false;
  }
  if (!PublishAppliedTypedBatch(*state, state_, batch, true, error)) {
    (void)restore_before();
    return false;
  }
  ProjectDocumentEdit(*state, recorded);
  state->pending_document_sequence.erase(patch.field_key);
  return true;
}

auto EditorHistoryMutation::Undo(const alcedo::EditorHistoryGuardHandle& guard,
                                 std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto prepared = state->history->PrepareUndo();
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    return false;
  }
  if (prepared.is_noop) return true;
  return ApplyPreparedHeadMoveOnLivePipeline(*state, state_, prepared, error);
}

auto EditorHistoryMutation::Redo(const alcedo::EditorHistoryGuardHandle& guard,
                                 std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto prepared = state->history->PrepareRedo();
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    return false;
  }
  if (prepared.is_noop) return true;
  return ApplyPreparedHeadMoveOnLivePipeline(*state, state_, prepared, error);
}

auto EditorHistoryMutation::MoveHeadToCommit(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::commit_hash_t& commit_id,
                                             std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto prepared = state->history->PrepareMoveHeadToCommit(commit_id);
  if (!prepared.ready) {
    if (error) *error = prepared.error;
    return false;
  }
  if (prepared.is_noop) return true;
  return ApplyPreparedHeadMoveOnLivePipeline(*state, state_, prepared, error);
}

auto EditorHistoryMutation::CommitPipelineEditBatch(const alcedo::EditorHistoryGuardHandle& guard,
                                                    alcedo::PipelineEditBatch batch,
                                                    std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  return PublishAppliedTypedBatch(*state, state_, batch, false, error);
}

auto EditorHistoryMutation::EditNodeGraph(const alcedo::EditorHistoryGuardHandle& guard,
                                          alcedo::NodeGraphTopologyChange change,
                                          std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  try {
    return PublishAppliedTypedBatch(*state, state_, MakeEditNodeGraphBatch(std::move(change)), false,
                                    error);
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
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->document->Document().Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  auto batch = MakeRenameColorGradeBatch(node_id, std::string{grade->DisplayName()},
                                         std::move(display_name));
  return PublishAppliedTypedBatch(*state, state_, batch, false, error);
}

/// Commit deletion metadata under the render lock; equal values clear stale render intent only.
auto EditorHistoryMutation::SetColorGradeDeletionProtected(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::NodeId& node_id,
    bool deletion_protected, std::string* error, bool* changed) -> bool {
  if (changed) *changed = false;
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->document->Document().Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  if (grade->DeletionProtected() == deletion_protected) {
    state_.RecordPublishedRenderReason(std::nullopt);
    return true;
  }
  auto batch = PipelineEditBatch::Make(
      PipelineEditOperationKind::SetNodeDeletionProtection,
      {SetNodeDeletionProtectionChange{node_id, grade->DeletionProtected(), deletion_protected}},
      PresentationKeyForOperation(PipelineEditOperationKind::SetNodeDeletionProtection));
  if (!PublishAppliedTypedBatch(*state, state_, batch, false, error)) return false;
  if (changed) *changed = true;
  return true;
}

auto EditorHistoryMutation::InsertColorGradeAtTop(const alcedo::EditorHistoryGuardHandle& guard,
                                                  const alcedo::NodeId& new_id,
                                                  const alcedo::NodeId& expected_predecessor_id,
                                                  std::string*          error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  try {
    auto change = alcedo::CaptureAddColorGradeAtTopChange(state->document->Document(), new_id,
                                                          expected_predecessor_id);
    return PublishAppliedTypedBatch(
        *state, state_, alcedo::MakeAddColorGradeBatch(std::move(change)), false, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto EditorHistoryMutation::RemoveColorGradeAndBridge(const alcedo::EditorHistoryGuardHandle& guard,
                                                      const alcedo::NodeId& node_id,
                                                      std::string*          error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  try {
    auto change = alcedo::CaptureRemoveColorGradeChange(state->document->Document(), node_id);
    return PublishAppliedTypedBatch(
        *state, state_, alcedo::MakeRemoveColorGradeBatch(std::move(change)), false, error);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto EditorHistoryMutation::SetColorGradeEnabled(const alcedo::EditorHistoryGuardHandle& guard,
                                                 const alcedo::NodeId& node_id, bool enabled,
                                                 std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->document->Document().Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  auto batch = MakeSetNodeEnabledBatch(node_id, PipelineEditNodeKind::ColorGrade, grade->Enabled(),
                                       enabled);
  return PublishAppliedTypedBatch(*state, state_, batch, false, error);
}

auto EditorHistoryMutation::SetColorGradeMix(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::NodeId& node_id, float mix,
                                             std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->document->Document().Graph().FindNode(node_id));
  if (grade == nullptr) {
    if (error) *error = "Color Grade node is missing: " + std::string{node_id.Value()};
    return false;
  }
  auto batch = MakeSetNodeMixBatch(node_id, grade->Mix(), mix);
  return PublishAppliedTypedBatch(*state, state_, batch, false, error);
}

auto EditorHistoryMutation::AddMask(const alcedo::EditorHistoryGuardHandle& guard,
                                    const alcedo::NodeId& node_id, alcedo::MaskModel mask,
                                    std::uint32_t display_index, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto mask_id = mask.id;
  auto       json    = MaskModelToJson(mask);
  auto batch = MakeAddMaskBatch(node_id, mask_id, std::move(json), display_index);
  return PublishAppliedTypedBatch(*state, state_, batch, false, error);
}

auto EditorHistoryMutation::RemoveMask(const alcedo::EditorHistoryGuardHandle& guard,
                                       const alcedo::NodeId& node_id, const alcedo::MaskId& mask_id,
                                       std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto* grade =
      dynamic_cast<ColorGradeNodeModel*>(state->document->Document().Graph().FindNode(node_id));
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
  return PublishAppliedTypedBatch(*state, state_, batch, false, error);
}

auto EditorHistoryMutation::ReplaceMaskSource(const alcedo::EditorHistoryGuardHandle& guard,
                                              const alcedo::NodeId& node_id,
                                              const alcedo::MaskId& mask_id,
                                              nlohmann::json after_source, std::string* error)
    -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->document->Document().Graph().FindNode(node_id));
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
  return PublishAppliedTypedBatch(*state, state_, batch, false, error);
}

auto EditorHistoryMutation::SetMaskField(const alcedo::EditorHistoryGuardHandle& guard,
                                         const alcedo::NodeId& node_id, const alcedo::MaskId& mask_id,
                                         std::string field_key, nlohmann::json after_value,
                                         std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(
      state->document->Document().Graph().FindNode(node_id));
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
  } else if (field_key == "deletion_protected") {
    before = mask->deletion_protected;
  } else {
    before = mask->display_name;
  }
  if (field_key == "deletion_protected" && before == after_value) {
    state_.RecordPublishedRenderReason(std::nullopt);
    return true;
  }
  auto batch = MakeSetMaskFieldBatch(node_id, mask_id, std::move(field_key), std::move(before),
                                     std::move(after_value));
  return PublishAppliedTypedBatch(*state, state_, batch, false, error);
}

auto EditorHistoryMutation::DiscardUnmaterializedChanges(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }

  if (!RestorePendingDocumentSequence(*state, error)) return false;
  const auto materialized_head = state->graph->GetImageEditState().materialized_head_commit_hash;
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
    if (!ApplyPreparedHeadMoveOnLivePipeline(*state, state_, prepared, error))
      return false;
  }

  if (state->journal && !state->journal->TruncateMaterialized(error)) return false;
  state->history->PublishWorkingSelection({});
  state->pending_document_sequence.clear();
  state->recovered_head = false;
  return true;
}

auto EditorHistoryMutation::CheckoutVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                            const alcedo::Hash128& version_id,
                                            std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }

  auto& graph = *state->graph;
  if (graph.GetActiveVersionId() == version_id) {
    return true;
  }

  alcedo::head_commit_hash_t target_head;
  try {
    target_head = graph.GetVersionRef(version_id).head_commit_hash;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  // Build phase: the target document and its panel projection stay private until every
  // fallible step has succeeded, so a failure restores only the history.
  auto document = EditorHistoryState::BuildDocumentForHead(*state, target_head, error);
  if (!document) return false;
  alcedo::NodeId                projection_node_id = state->panel_projection_node_id;
  alcedo::EditorPanelProjection projection;
  try {
    if (!ProjectPanelFieldsForDocument(*document, &projection_node_id, &projection, error)) {
      return false;
    }
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  const auto graph_before      = graph;
  const auto prior_select      = state->history->WorkingSelection();
  const bool prior_recovered   = state->recovered_head;
  auto       restore_or_report = [&](std::string original) {
    try {
      graph = graph_before;
      state->history->PublishWorkingSelection(prior_select);
      state->recovered_head = prior_recovered;
      if (error) *error = std::move(original);
    } catch (const std::exception& ex) {
      if (error) {
        *error = std::string("fatal editor session: ") + original +
                 "; prior Version restoration failed: " + ex.what();
      }
    }
  };

  try {
    graph.SetActiveVersionId(version_id);
    if (!state->history->SelectVersion(version_id, error)) {
      restore_or_report(error ? *error : std::string{"Version selection failed"});
      return false;
    }
    if (auto pipeline_service = state_.PipelineMapper()) {
      std::string persistence_error;
      if (!pipeline_service->PersistEditorHistory(graph, graph_before.GetImageEditState(),
                                                  *document, &persistence_error)) {
        restore_or_report(persistence_error);
        return false;
      }
    }
  } catch (const std::exception& ex) {
    restore_or_report(ex.what());
    return false;
  }

  // Swap phase: the checked-out Version is active and persisted; bind its document.
  state->document->Replace(std::move(document));
  state->panel_projection_node_id = std::move(projection_node_id);
  state->panel_projection         = std::move(projection);
  state_.RecordPublishedRenderReason(alcedo::EditorRenderReason::VersionDocumentChanged);
  state->recovered_head = false;
  return true;
}

auto EditorHistoryMutation::SetPanelProjectionNode(const alcedo::EditorHistoryGuardHandle& guard,
                                                   const alcedo::NodeId& node_id,
                                                   std::uint64_t session_generation,
                                                   std::string* error) -> bool {
  auto state = state_.PeekWorkingState(guard.element_id);
  if (!state) {
    if (error) *error = "Editor history working state is unavailable";
    return false;
  }
  if (node_id.Empty()) {
    state->panel_projection_node_id = {};
    state->panel_projection         = {};
    return true;
  }
  try {
    alcedo::EditorPanelProjection next;
    const auto&                   document = state->document->Document();
    if (!alcedo::ProjectSelectedNodePanelFields(document, node_id, session_generation, &next,
                                                error)) {
      return false;
    }
    state->panel_projection_node_id = node_id;
    state->panel_projection         = std::move(next);
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto EditorHistoryMutation::WithWorkingDocument(
    const alcedo::EditorHistoryGuardHandle&           guard,
    const alcedo::IEditorHistoryPort::MaskDocumentOp& op, std::string* error) -> bool {
  if (!op) {
    if (error) *error = "Locked Mask document operation is empty";
    return false;
  }
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  alcedo::IEditorHistoryPort::MaskSettle settle =
      [this, state](const alcedo::PipelineEditBatch& batch, std::string* settle_error) {
        return PublishAppliedTypedBatch(*state, state_, batch, true,
                                        settle_error);
      };
  bool       input_open = state->mask_input_open;
  const bool applied = op(state->document->Document(), *state->history, settle, &input_open, error);
  state->mask_input_open = input_open;
  return applied;
}

}  // namespace alcedo::ui
