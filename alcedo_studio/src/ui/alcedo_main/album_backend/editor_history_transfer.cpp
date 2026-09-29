//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_transfer.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <utility>

#include "app/document_transfer.hpp"
#include "app/document_transfer_planner.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/pipeline_document_history.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/version_ref.hpp"
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
  alcedo::CommitGraph                       graph;  // includes logical head on active Version
  alcedo::MiniGitWorkingSelection           selection;
  bool                                      recovered = false;
  std::optional<alcedo::EditorRenderReason> published_reason;
};

auto CaptureLivePastePrior(HistoryWorkingState& state, EditorHistoryState& history_state)
    -> LivePastePriorState {
  return LivePastePriorState{*state.graph, state.history->WorkingSelection(), state.recovered_head,
                             history_state.LastPublishedRenderReason()};
}

void RestoreLivePastePrior(HistoryWorkingState& state, EditorHistoryState& history_state,
                           const LivePastePriorState& prior) {
  *state.graph = prior.graph;
  state.history->PublishWorkingSelection(prior.selection);
  state.recovered_head = prior.recovered;
  history_state.RecordPublishedRenderReason(prior.published_reason);
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
  if (!state->history || !state->journal) {
    return SetError(error, "Editor live paste requires a complete history state");
  }

  auto&                                graph            = *state->graph;
  const auto prior_version_id = graph.GetActiveVersionId();
  const auto prior = CaptureLivePastePrior(*state, state_);

  alcedo::DocumentTransferPasteOptions options;

  alcedo::PreparedDocumentPaste prepared;
  try {
    prepared = alcedo::DocumentTransferPlanner::Plan(package, state->root->document, options);
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }

  // Build phase: the new Version starts at the root, so its document is the replay of the root.
  // It is the checkpoint of the persisted Version state and, with the paste batch applied, the
  // pasted document. It stays private until the WAL append below succeeds.
  auto pasted_document = EditorHistoryState::BuildDocumentForHead(*state, std::nullopt, error);
  if (!pasted_document) return false;

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
    if (!pipeline_service->PersistEditorHistory(graph, expected_before_version, *pasted_document,
                                                &persistence_error)) {
      RestoreLivePastePrior(*state, state_, prior);
      return SetError(error, persistence_error.empty() ? "Paste Version persistence failed"
                                                       : persistence_error);
    }
    version_persisted       = true;
    persisted_version_state = graph.GetImageEditState();
  }

  // Every rollback runs before the WAL append publishes, so no journal truncate is needed. The
  // working document was never replaced, so it is the document of the restored Version.
  auto rollback_after_version = [&]() -> bool {
    RestoreLivePastePrior(*state, state_, prior);
    if (version_persisted) {
      if (auto pipeline_service = state_.PipelineMapper()) {
        std::string persistence_error;
        if (!pipeline_service->PersistEditorHistory(
                graph, persisted_version_state, state->document->Document(), &persistence_error)) {
          return SetError(error, persistence_error.empty()
                                     ? "Paste Version persistence rollback failed"
                                     : persistence_error);
        }
      }
    }
    return true;
  };

  if (!alcedo::ApplyPipelineEditBatch(*pasted_document, prepared.batch,
                                      alcedo::PipelineEditApplyDirection::Forward, error)) {
    (void)rollback_after_version();
    return false;
  }

  // Project the panels from the pasted document before publishing, as Version
  // checkout does. Without it every panel (LUT included) keeps the prior
  // Version values after the paste.
  alcedo::NodeId                pasted_projection_node_id = state->panel_projection_node_id;
  alcedo::EditorPanelProjection pasted_projection;
  try {
    if (!ProjectPanelFieldsForDocument(*pasted_document, &pasted_projection_node_id,
                                       &pasted_projection, error)) {
      (void)rollback_after_version();
      return false;
    }
  } catch (const std::exception& ex) {
    (void)rollback_after_version();
    return SetError(error, ex.what());
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

  // Swap phase: the new Version head is published, so bind its document and its
  // panel projection.
  state->document->Replace(std::move(pasted_document));
  state->panel_projection_node_id = std::move(pasted_projection_node_id);
  state->panel_projection         = std::move(pasted_projection);

  state_.RecordPublishedRenderReason(alcedo::RenderReasonForBatch(prepared.batch));
  state->recovered_head = false;

  result->pasted = true;
  result->new_version_id = new_version_id;
  result->prior_version_id = prior_version_id;
  result->new_head = state->history->working_head().value_or(alcedo::commit_hash_t{});
  return true;
}

}  // namespace alcedo::ui
