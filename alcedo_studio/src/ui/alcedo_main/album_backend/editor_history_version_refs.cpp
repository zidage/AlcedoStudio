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

struct NamedRefPriorState {
  alcedo::CommitGraph graph;  // includes logical head on active Version
  alcedo::MiniGitWorkingSelection selection;
  bool dirty = false;
  bool serialized = false;
  bool recovered = false;
  /// The document bound before the operation. Replay swaps in a new document and never
  /// changes this one, so a restore binds it back without a copy.
  std::shared_ptr<alcedo::PipelineDocument> document;
};

auto CaptureNamedRefPrior(HistoryWorkingState& state) -> NamedRefPriorState {
  NamedRefPriorState prior;
  prior.graph = *state.pipeline_guard->commit_graph_;
  prior.selection = state.history->WorkingSelection();
  prior.dirty = state.pipeline_guard->dirty_;
  prior.serialized = state.pipeline_guard->serialized_state_needs_writeback_;
  prior.recovered = state.recovered_head;
  prior.document   = state.pipeline_guard->document_;
  return prior;
}

void RestoreNamedRefPrior(HistoryWorkingState& state, const NamedRefPriorState& prior) {
  *state.pipeline_guard->commit_graph_ = prior.graph;
  state.history->PublishWorkingSelection(prior.selection);
  state.pipeline_guard->dirty_ = prior.dirty;
  state.pipeline_guard->serialized_state_needs_writeback_ = prior.serialized;
  state.recovered_head = prior.recovered;
  if (!prior.document || prior.document == state.pipeline_guard->document_ ||
      !state.pipeline_guard->pipeline_) {
    return;
  }
  auto render_lock = LockLivePipeline(*state.pipeline_guard->pipeline_);
  (void)alcedo::BindLivePipelineDocument(*state.pipeline_guard, prior.document);
}

void PublishNamedRefSuccess(HistoryWorkingState& state) {
  state.pipeline_guard->dirty_ = false;
  state.pipeline_guard->serialized_state_needs_writeback_ = false;
  state.recovered_head = false;
}

}  // namespace

EditorHistoryVersionRefs::EditorHistoryVersionRefs(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryVersionRefs::CreateRootVersionAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, std::string display_name,
    alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto prior = CaptureNamedRefPrior(*state);
  const auto expected_materialized = prior.graph.GetImageEditState();
  alcedo::version_ref_id_t new_id{};
  try {
    new_id = graph.CreateVersionRefAtRoot(UniqueVersionName(graph, std::move(display_name)));
    graph.SetActiveVersionId(new_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  if (!state->history->SelectVersion(new_id, error)) {
    RestoreNamedRefPrior(*state, prior);
    return false;
  }
  if (!state_.ReplayWorkingDocumentFromImmutableRoot(*state, std::nullopt, error)) {
    try {
      RestoreNamedRefPrior(*state, prior);
    } catch (const std::exception& ex) {
      if (error) {
        *error = std::string("fatal editor session: ") + (error->empty() ? std::string{} : *error) +
                 "; prior Version restoration failed: " + ex.what();
      }
    }
    return false;
  }
  if (auto pipeline_service = state_.PipelineMapper()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard, expected_materialized,
                                                     &persistence_error)) {
      try {
        RestoreNamedRefPrior(*state, prior);
      } catch (const std::exception& ex) {
        if (error) {
          *error = std::string("fatal editor session: ") + persistence_error +
                   "; prior Version restoration failed: " + ex.what();
        }
        return false;
      }
      if (error) *error = persistence_error;
      return false;
    }
  }
  if (version_id) *version_id = new_id;
  PublishNamedRefSuccess(*state);
  state_.RecordPublishedRenderReason(alcedo::EditorRenderReason::VersionDocumentChanged);
  return true;
}

auto EditorHistoryVersionRefs::BranchFromCommitAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::commit_hash_t& commit_id,
    std::string display_name, alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  if (!graph.FindCommit(commit_id)) {
    if (error) *error = "Branch target commit does not exist in the editor history";
    return false;
  }
  const auto prior = CaptureNamedRefPrior(*state);
  const auto expected_materialized = prior.graph.GetImageEditState();
  alcedo::version_ref_id_t new_id{};
  try {
    new_id = graph.CreateVersionRefAtHead(UniqueVersionName(graph, std::move(display_name)),
                                          commit_id);
    graph.SetActiveVersionId(new_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  if (!state->history->SelectVersion(new_id, error)) {
    RestoreNamedRefPrior(*state, prior);
    return false;
  }
  if (!state_.ReplayWorkingDocumentFromImmutableRoot(*state, commit_id, error)) {
    try {
      RestoreNamedRefPrior(*state, prior);
    } catch (const std::exception& ex) {
      if (error) {
        *error = std::string("fatal editor session: ") + (error->empty() ? std::string{} : *error) +
                 "; prior Version restoration failed: " + ex.what();
      }
    }
    return false;
  }
  if (auto pipeline_service = state_.PipelineMapper()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard, expected_materialized,
                                                     &persistence_error)) {
      try {
        RestoreNamedRefPrior(*state, prior);
      } catch (const std::exception& ex) {
        if (error) {
          *error = std::string("fatal editor session: ") + persistence_error +
                   "; prior Version restoration failed: " + ex.what();
        }
        return false;
      }
      if (error) *error = persistence_error;
      return false;
    }
  }
  if (version_id) *version_id = new_id;
  PublishNamedRefSuccess(*state);
  state_.RecordPublishedRenderReason(alcedo::EditorRenderReason::VersionDocumentChanged);
  return true;
}

auto EditorHistoryVersionRefs::RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id,
                                             std::string display_name, std::string* error)
    -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  try {
    auto& ref = graph.GetVersionRef(version_id);
    ref.display_name = UniqueVersionName(graph, std::move(display_name), &version_id);
    ref.updated_at = std::time(nullptr);
    state->pipeline_guard->dirty_ = true;
    state->pipeline_guard->serialized_state_needs_writeback_ = true;
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
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  if (!state->pipeline_guard->commit_graph_->RemoveVersionRef(version_id)) {
    if (error) *error = "The active Version or the final remaining Version cannot be removed";
    return false;
  }
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->recovered_head = false;
  return true;
}

}  // namespace alcedo::ui
