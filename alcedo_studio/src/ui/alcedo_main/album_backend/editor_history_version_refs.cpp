//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_version_refs.hpp"

#include <ctime>
#include <mutex>
#include <utility>

#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {

EditorHistoryVersionRefs::EditorHistoryVersionRefs(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryVersionRefs::CreateRootVersionAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, std::string display_name,
    alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port = state_.PipelinePort();
  if (!pipeline_port) {
    if (error) *error = "Editor pipeline port is unavailable for root Version creation";
    return false;
  }
  auto pipeline_service = pipeline_port->PipelineService();
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for root Version creation";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  const auto prior_head = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending = state->pending_before;
  const bool prior_recovered = state->recovered_head;

  alcedo::version_ref_id_t new_id{};
  try {
    new_id = graph.CreateVersionRefAtRoot(UniqueVersionName(graph, std::move(display_name)));
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  std::string rebuild_error;
  if (!pipeline_port->CheckoutVersion(guard.element_id, new_id, &rebuild_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
    if (error) *error = rebuild_error;
    return false;
  }

  std::string select_error;
  if (!state->history->SelectVersion(new_id, &select_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
    if (error) *error = select_error;
    return false;
  }

  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &next_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
    return false;
  }

  std::string persistence_error;
  if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                   graph_before.GetImageEditState(),
                                                   &persistence_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
    if (error) *error = persistence_error;
    return false;
  }

  if (version_id) *version_id = new_id;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->pending_before.clear();
  state->recovered_head = false;
  state->committed_snapshot = std::move(next_snapshot);
  return true;
}

auto EditorHistoryVersionRefs::BranchFromCommitAndCheckout(
    const alcedo::EditorHistoryGuardHandle& guard, const alcedo::commit_hash_t& commit_id,
    std::string display_name, alcedo::version_ref_id_t* version_id, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  auto pipeline_port = state_.PipelinePort();
  if (!pipeline_port) {
    if (error) *error = "Editor pipeline port is unavailable for branch creation";
    return false;
  }
  auto pipeline_service = pipeline_port->PipelineService();
  if (!pipeline_service) {
    if (error) *error = "Pipeline service is unavailable for branch creation";
    return false;
  }
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto graph_before = graph;
  const auto prior_head = state->pipeline_guard->working_head_commit_hash_;
  const auto prior_chain = state->pipeline_guard->transaction_chain_hash_;
  const bool prior_dirty = state->pipeline_guard->dirty_;
  const bool prior_serialized = state->pipeline_guard->serialized_state_needs_writeback_;
  const auto prior_snapshot = state->committed_snapshot;
  const auto prior_pending = state->pending_before;
  const bool prior_recovered = state->recovered_head;

  if (!graph.FindCommit(commit_id)) {
    if (error) *error = "Branch target commit does not exist in the editor history";
    return false;
  }

  alcedo::version_ref_id_t new_id{};
  try {
    new_id = graph.CreateVersionRefAtHead(UniqueVersionName(graph, std::move(display_name)),
                                          commit_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  std::string rebuild_error;
  if (!pipeline_port->CheckoutVersion(guard.element_id, new_id, &rebuild_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
    if (error) *error = rebuild_error;
    return false;
  }

  std::string select_error;
  if (!state->history->SelectVersion(new_id, &select_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
    if (error) *error = select_error;
    return false;
  }

  alcedo::EditorRenderAdjustmentSnapshot next_snapshot;
  if (!ReadPipelineSnapshot(*state->pipeline_guard, &next_snapshot, error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
    return false;
  }

  std::string persistence_error;
  if (!pipeline_service->PersistEditorHistoryState(state->pipeline_guard,
                                                   graph_before.GetImageEditState(),
                                                   &persistence_error)) {
    RestoreGraphAndPipeline(graph, graph_before, *pipeline_service, state->pipeline_guard,
                            prior_head, prior_chain, prior_dirty, prior_serialized);
    state->committed_snapshot = prior_snapshot;
    state->pending_before = prior_pending;
    state->recovered_head = prior_recovered;
    if (error) *error = persistence_error;
    return false;
  }

  if (version_id) *version_id = new_id;
  state->pipeline_guard->working_head_commit_hash_ = state->history->working_head();
  state->pipeline_guard->transaction_chain_hash_ = state->history->transaction_chain_hash();
  state->pipeline_guard->dirty_ = true;
  state->pipeline_guard->serialized_state_needs_writeback_ = true;
  state->pending_before.clear();
  state->recovered_head = false;
  state->committed_snapshot = std::move(next_snapshot);
  return true;
}

auto EditorHistoryVersionRefs::RenameVersion(const alcedo::EditorHistoryGuardHandle& guard,
                                             const alcedo::Hash128& version_id,
                                             std::string display_name, std::string* error)
    -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  auto& graph = *state->pipeline_guard->commit_graph_;
  try {
    auto& ref = graph.GetVersionRef(version_id);
    const auto name = UniqueVersionName(graph, std::move(display_name), &version_id);
    ref.display_name = name;
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
  std::scoped_lock state_lock(state->mutex);
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  if (!state->pipeline_guard->commit_graph_->RemoveVersionRef(version_id)) {
    if (error) {
      *error = "The active Version or the final remaining Version cannot be removed";
    }
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
