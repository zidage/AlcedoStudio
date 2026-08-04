//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_checkpoint.hpp"

#include <filesystem>
#include <utility>

#include "app/editor_mini_git_materializer.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "ui/alcedo_main/album_backend/editor_history_shared_helpers.hpp"
#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

namespace alcedo::ui {
EditorHistoryCheckpoint::EditorHistoryCheckpoint(EditorHistoryState& state) : state_(state) {}

auto EditorHistoryCheckpoint::CaptureSaveCheckpoint(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error)
    -> std::shared_ptr<const alcedo::EditorMiniGitSaveCapture> {
  auto journal_path = state_.JournalPathResolver();
  if (!journal_path) {
    if (error) *error = "Mini-Git journal path is unavailable";
    return nullptr;
  }

  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return nullptr;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_ || !state->history ||
      !state->journal) {
    if (error) *error = "Mini-Git save capture requires an immutable history state";
    return nullptr;
  }

  const auto journal_snapshot = state->journal->Snapshot();

  // Single live identity: CommitGraph active Version head is the only logical head.
  // Build one materialization from the graph, then project capture fields from it.
  auto& graph = *state->pipeline_guard->commit_graph_;
  const auto logical_head  = graph.GetActiveVersionRef().head_commit_hash;
  const auto logical_chain = graph.ChainHashForHead(logical_head);

  const auto pipeline_params = MakePipelineParamsFromSnapshot(state->committed_snapshot, error);
  if (!pipeline_params.has_value()) return nullptr;
  const auto serialized = alcedo::MakeEditorSerializedPipelineState(
      state->pipeline_guard->root_id_, logical_head, logical_chain, *pipeline_params);

  alcedo::EditorMiniGitSaveCapture capture;
  capture.journal_records        = journal_snapshot.records;
  capture.journal_path           = state->journal->path();
  capture.first_journal_sequence = journal_snapshot.first_sequence;
  capture.last_journal_sequence  = journal_snapshot.last_sequence;
  try {
    capture.materialization =
        graph.CaptureMaterializationWithSerializedPipelineState(serialized);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return nullptr;
  }
  // Top-level identity fields are projections of materialization only.
  capture.element_id             = capture.materialization.image_state.element_id;
  capture.version_id             = capture.materialization.image_state.active_version_id;
  capture.root_id                = capture.materialization.image_state.root_id;
  capture.working_head           = capture.materialization.image_state.materialized_head_commit_hash;
  capture.transaction_chain_hash =
      capture.materialization.image_state.materialized_transaction_chain_hash;
  return std::make_shared<const alcedo::EditorMiniGitSaveCapture>(std::move(capture));
}

auto EditorHistoryCheckpoint::DiscardMaterializedJournalThrough(
    const alcedo::EditorHistoryGuardHandle& guard, std::uint64_t last_sequence,
    std::string* error) -> bool {
  if (last_sequence == 0) {
    if (error) *error = "DiscardMaterializedJournalThrough requires a non-zero sequence";
    return false;
  }
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->journal) {
    if (error) *error = "Mini-Git journal is unavailable for prefix discard";
    return false;
  }
  return state->journal->TruncateThroughSequence(last_sequence, error);
}

auto EditorHistoryCheckpoint::SyncMaterializedStateAfterCheckpoint(
    const alcedo::EditorHistoryGuardHandle& guard, std::string* error) -> bool {
  auto state = state_.EnsureWorkingState(guard.element_id, error);
  if (!state) return false;
  if (!state->pipeline_guard || !state->pipeline_guard->commit_graph_) {
    if (error) *error = "Mini-Git commit graph is unavailable for materialized-state sync";
    return false;
  }
  try {
    state->pipeline_guard->commit_graph_->MaterializeActiveHeadInMemory();
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  return true;
}

}  // namespace alcedo::ui
