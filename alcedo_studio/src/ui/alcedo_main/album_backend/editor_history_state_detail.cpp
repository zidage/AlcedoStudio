//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

#include <ctime>

#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"

namespace alcedo::ui {

void EditorHistoryState::SetServices(Services services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

void EditorHistoryState::SetPipelinePort(
    std::shared_ptr<EditorSessionPipelinePort> pipeline_port) {
  std::scoped_lock lock(mutex_);
  pipeline_port_ = std::move(pipeline_port);
}

auto EditorHistoryState::EnsureWorkingState(sl_element_id_t element_id, std::string* error)
    -> std::shared_ptr<HistoryWorkingState> {
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  std::function<std::filesystem::path(sl_element_id_t)> journal_path;
  {
    std::scoped_lock lock(mutex_);
    const auto existing = working_states_.find(element_id);
    if (existing != working_states_.end()) return existing->second;
    pipeline_port = pipeline_port_.lock();
    journal_path = services_.mini_git_journal_path;
  }
  if (!pipeline_port) {
    if (error) *error = "Editor pipeline port is unavailable";
    return nullptr;
  }
  auto guard = pipeline_port->EnsureLoaded(element_id, error);
  if (!guard || !guard->commit_graph_) {
    if (error && error->empty()) *error = "Editor Mini-Git history graph is unavailable";
    return nullptr;
  }

  std::filesystem::path path;
  try {
    if (journal_path) path = journal_path(element_id);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return nullptr;
  } catch (...) {
    if (error) *error = "Failed to resolve Mini-Git journal path";
    return nullptr;
  }
  auto journal = std::make_shared<alcedo::MiniGitJournal>(std::move(path));
  if (!journal->Load(error)) return nullptr;

  auto state = std::make_shared<HistoryWorkingState>();
  state->pipeline_guard = guard;
  state->journal = journal;
  const auto& image_state = guard->commit_graph_->GetImageEditState();
  if (image_state.serialized_pipeline_state.has_value() &&
      image_state.serialized_pipeline_state->is_object() &&
      image_state.serialized_pipeline_state->contains("pipeline_params")) {
    if (!MakeAdjustmentSnapshotFromPipelineParams(
            image_state.serialized_pipeline_state->at("pipeline_params"),
            &state->committed_snapshot, error)) {
      return nullptr;
    }
  } else {
    state->committed_snapshot = MakeEmptyCompleteAdjustmentSnapshot();
  }
  if (!RootSnapshotFromMaterialized(
          state->committed_snapshot, *guard->commit_graph_,
          image_state.materialized_head_commit_hash, &state->root_snapshot, error)) {
    return nullptr;
  }
  auto replay_graph = *guard->commit_graph_;
  const auto journal_records = journal->records();
  for (const auto& record : journal_records) {
    if (!ApplyRecoveredRecordToSnapshot(&state->committed_snapshot, &replay_graph, record, error)) {
      return nullptr;
    }
  }
  *guard->commit_graph_ = std::move(replay_graph);
  guard->working_head_commit_hash_ = guard->commit_graph_->GetActiveVersionRef().head_commit_hash;
  guard->transaction_chain_hash_ =
      guard->commit_graph_->ChainHashForHead(guard->working_head_commit_hash_);
  guard->dirty_ = !journal_records.empty();
  state->recovered_head = !journal_records.empty();
  state->history =
      std::make_unique<alcedo::MiniGitWorkingHistory>(guard->commit_graph_, journal);

  std::scoped_lock lock(mutex_);
  const auto [it, inserted] = working_states_.emplace(element_id, state);
  return inserted ? state : it->second;
}

void EditorHistoryState::ReleaseState(sl_element_id_t element_id) {
  std::scoped_lock lock(mutex_);
  working_states_.erase(element_id);
}

auto EditorHistoryState::PipelinePort() const -> std::shared_ptr<EditorSessionPipelinePort> {
  std::scoped_lock lock(mutex_);
  return pipeline_port_.lock();
}

auto EditorHistoryState::PipelineService() const
    -> std::shared_ptr<alcedo::PipelineMgmtService> {
  auto port = PipelinePort();
  return port ? port->PipelineService() : nullptr;
}

auto EditorHistoryState::HasUnmaterializedChanges(sl_element_id_t element_id, std::string* error)
    -> bool {
  auto state = EnsureWorkingState(element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  return state->history->working_head() !=
         state->pipeline_guard->commit_graph_->GetImageEditState().materialized_head_commit_hash;
}

auto EditorHistoryState::JournalPathResolver() const
    -> std::function<std::filesystem::path(sl_element_id_t)> {
  std::scoped_lock lock(mutex_);
  return services_.mini_git_journal_path;
}

}  // namespace alcedo::ui
