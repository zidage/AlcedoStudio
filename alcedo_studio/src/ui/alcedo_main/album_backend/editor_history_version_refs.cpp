//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_version_refs.hpp"

#include <ctime>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {
namespace {

/// History fields a Version operation changes before its persistence step.
struct NamedRefPriorState {
  alcedo::CommitGraph             graph;  // includes logical head on active Version
  alcedo::MiniGitWorkingSelection selection;
  bool                            recovered = false;
};

auto CaptureNamedRefPrior(HistoryWorkingState& state) -> NamedRefPriorState {
  return NamedRefPriorState{*state.graph, state.history->WorkingSelection(), state.recovered_head};
}

void RestoreNamedRefPrior(HistoryWorkingState& state, const NamedRefPriorState& prior) {
  *state.graph = prior.graph;
  state.history->PublishWorkingSelection(prior.selection);
  state.recovered_head = prior.recovered;
}

/**
 * @brief Make the new Version @p new_id active, persist it with the checkpoint of its document,
 *        and swap its document in.
 *
 * Build-then-swap: the document of @p head and its panel projection are built first; the graph
 * change is restored when selection or persistence fails; the working document is replaced only
 * after every fallible step succeeded.
 * @pre @p new_id was just created in the graph by the caller.
 */
auto CheckoutCreatedVersion(HistoryWorkingState& state, EditorHistoryState& history_state,
                            const NamedRefPriorState& prior, const alcedo::version_ref_id_t& new_id,
                            const alcedo::head_commit_hash_t& head, std::string* error) -> bool {
  auto restore_or_report = [&](std::string original) {
    try {
      RestoreNamedRefPrior(state, prior);
      if (error) *error = std::move(original);
    } catch (const std::exception& ex) {
      if (error) {
        *error = std::string("fatal editor session: ") + original +
                 "; prior Version restoration failed: " + ex.what();
      }
    }
  };
  auto document = EditorHistoryState::BuildDocumentForHead(state, head, error);
  if (!document) {
    restore_or_report(error ? *error : std::string{"Version replay failed"});
    return false;
  }
  alcedo::NodeId                projection_node_id = state.panel_projection_node_id;
  alcedo::EditorPanelProjection projection;
  try {
    if (!ProjectPanelFieldsForDocument(*document, &projection_node_id, &projection, error)) {
      restore_or_report(error ? *error : std::string{"Panel projection failed"});
      return false;
    }
    state.graph->SetActiveVersionId(new_id);
    if (!state.history->SelectVersion(new_id, error)) {
      restore_or_report(error ? *error : std::string{"Version selection failed"});
      return false;
    }
    if (auto pipeline_service = history_state.PipelineMapper()) {
      std::string persistence_error;
      if (!pipeline_service->PersistEditorHistory(*state.graph, prior.graph.GetImageEditState(),
                                                  *document, &persistence_error)) {
        restore_or_report(persistence_error);
        return false;
      }
    }
  } catch (const std::exception& ex) {
    restore_or_report(ex.what());
    return false;
  }
  state.document->Replace(std::move(document));
  state.panel_projection_node_id = std::move(projection_node_id);
  state.panel_projection         = std::move(projection);
  state.recovered_head           = false;
  history_state.RecordPublishedRenderReason(alcedo::EditorRenderReason::VersionDocumentChanged);
  return true;
}

}  // namespace

EditorHistoryVersionRefs::EditorHistoryVersionRefs(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryVersionRefs::CreateRootVersionAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, std::string display_name,
    alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto&                    graph = *state->graph;
  const auto               prior = CaptureNamedRefPrior(*state);
  alcedo::version_ref_id_t new_id{};
  try {
    new_id = graph.CreateVersionRefAtRoot(UniqueVersionName(graph, std::move(display_name)));
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  if (!CheckoutCreatedVersion(*state, state_, prior, new_id, std::nullopt, error)) return false;
  if (version_id) *version_id = new_id;
  return true;
}

auto EditorHistoryVersionRefs::BranchFromCommitAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::commit_hash_t& commit_id,
    std::string display_name, alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->graph;
  if (!graph.FindCommit(commit_id)) {
    if (error) *error = "Branch target commit does not exist in the editor history";
    return false;
  }
  const auto               prior = CaptureNamedRefPrior(*state);
  alcedo::version_ref_id_t new_id{};
  try {
    new_id =
        graph.CreateVersionRefAtHead(UniqueVersionName(graph, std::move(display_name)), commit_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  if (!CheckoutCreatedVersion(*state, state_, prior, new_id, commit_id, error)) return false;
  if (version_id) *version_id = new_id;
  return true;
}

auto EditorHistoryVersionRefs::RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id,
                                             std::string display_name, std::string* error)
    -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto& graph = *state->graph;
  try {
    auto& ref = graph.GetVersionRef(version_id);
    ref.display_name = UniqueVersionName(graph, std::move(display_name), &version_id);
    ref.updated_at        = std::time(nullptr);
    state->recovered_head = false;
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
}

auto EditorHistoryVersionRefs::RemoveVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id,
                                             std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->graph->RemoveVersionRef(version_id)) {
    if (error) *error = "The active Version or the final remaining Version cannot be removed";
    return false;
  }
  state->recovered_head = false;
  return true;
}

}  // namespace alcedo::ui
