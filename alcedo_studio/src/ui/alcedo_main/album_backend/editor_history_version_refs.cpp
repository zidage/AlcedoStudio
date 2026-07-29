//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_version_refs.hpp"

#include <ctime>
#include <string>
#include <unordered_map>
#include <utility>

#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {
namespace {

struct NamedRefPriorState {
  alcedo::CommitGraph graph;
  alcedo::MiniGitWorkingSelection selection;
  alcedo::head_commit_hash_t head = std::nullopt;
  alcedo::transaction_chain_hash_t chain{};
  bool dirty = false;
  bool serialized = false;
  alcedo::EditorRenderAdjustmentSnapshot snapshot;
  alcedo::EditorRenderAdjustmentSnapshot root_snapshot;
  std::unordered_map<std::string, alcedo::EditorAdjustmentOperatorState> pending;
  bool recovered = false;
};

auto CaptureNamedRefPrior(HistoryWorkingState& state) -> NamedRefPriorState {
  NamedRefPriorState prior;
  prior.graph = *state.pipeline_guard->commit_graph_;
  prior.selection = state.history->WorkingSelection();
  prior.head = state.pipeline_guard->working_head_commit_hash_;
  prior.chain = state.pipeline_guard->transaction_chain_hash_;
  prior.dirty = state.pipeline_guard->dirty_;
  prior.serialized = state.pipeline_guard->serialized_state_needs_writeback_;
  prior.snapshot = state.committed_snapshot;
  prior.root_snapshot = state.root_snapshot;
  prior.pending = state.pending_before;
  prior.recovered = state.recovered_head;
  return prior;
}

void RestoreNamedRefPrior(HistoryWorkingState& state, const NamedRefPriorState& prior) {
  *state.pipeline_guard->commit_graph_ = prior.graph;
  state.history->PublishWorkingSelection(prior.selection);
  state.pipeline_guard->working_head_commit_hash_ = prior.head;
  state.pipeline_guard->transaction_chain_hash_ = prior.chain;
  state.pipeline_guard->dirty_ = prior.dirty;
  state.pipeline_guard->serialized_state_needs_writeback_ = prior.serialized;
  state.committed_snapshot = prior.snapshot;
  state.root_snapshot = prior.root_snapshot;
  state.pending_before = prior.pending;
  state.recovered_head = prior.recovered;
}

void PublishNamedRefSuccess(HistoryWorkingState& state,
                            alcedo::EditorRenderAdjustmentSnapshot next_snapshot) {
  state.pipeline_guard->working_head_commit_hash_ = state.history->working_head();
  state.pipeline_guard->transaction_chain_hash_ = state.history->transaction_chain_hash();
  state.pipeline_guard->dirty_ = false;
  state.pipeline_guard->serialized_state_needs_writeback_ = false;
  state.pending_before.clear();
  state.recovered_head = false;
  state.committed_snapshot = std::move(next_snapshot);
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
  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph, std::nullopt, &next_snapshot, error)) {
    RestoreNamedRefPrior(*state, prior);
    return false;
  }
  if (auto pipeline_service = state_.PipelineService()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard, expected_materialized,
                                                     &persistence_error)) {
      RestoreNamedRefPrior(*state, prior);
      if (error) *error = persistence_error;
      return false;
    }
  }
  if (version_id) *version_id = new_id;
  PublishNamedRefSuccess(*state, std::move(next_snapshot));
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
  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!SnapshotAtHead(state->root_snapshot, graph, commit_id, &next_snapshot, error)) {
    RestoreNamedRefPrior(*state, prior);
    return false;
  }
  if (auto pipeline_service = state_.PipelineService()) {
    std::string persistence_error;
    if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard, expected_materialized,
                                                     &persistence_error)) {
      RestoreNamedRefPrior(*state, prior);
      if (error) *error = persistence_error;
      return false;
    }
  }
  if (version_id) *version_id = new_id;
  PublishNamedRefSuccess(*state, std::move(next_snapshot));
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
    state->pipeline_guard->working_head_commit_hash_ = graph.GetActiveVersionRef().head_commit_hash;
    state->pipeline_guard->transaction_chain_hash_ =
        graph.ChainHashForHead(state->pipeline_guard->working_head_commit_hash_);
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
  state->pipeline_guard->working_head_commit_hash_ =
      state->pipeline_guard->commit_graph_->GetActiveVersionRef().head_commit_hash;
  state->pipeline_guard->transaction_chain_hash_ =
      state->pipeline_guard->commit_graph_->ChainHashForHead(
          state->pipeline_guard->working_head_commit_hash_);
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->recovered_head = false;
  return true;
}

}  // namespace alcedo::ui
