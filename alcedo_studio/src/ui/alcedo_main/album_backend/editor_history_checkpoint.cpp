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

  alcedo::EditorMiniGitSaveCapture capture;
  capture.element_id = guard.element_id;
  capture.version_id = state->pipeline_guard->commit_graph_->GetActiveVersionId();
  capture.root_id = state->pipeline_guard->root_id_;
  capture.working_head = state->history->working_head();
  capture.transaction_chain_hash = state->history->transaction_chain_hash();
  capture.journal_records = journal_snapshot.records;
  capture.journal_path = state->journal->path();
  capture.first_journal_sequence = journal_snapshot.first_sequence;
  capture.last_journal_sequence = journal_snapshot.last_sequence;

  const auto pipeline_params = MakePipelineParamsFromSnapshot(state->committed_snapshot, error);
  if (!pipeline_params.has_value()) return nullptr;
  const auto serialized = alcedo::MakeEditorSerializedPipelineState(
      state->pipeline_guard->root_id_, capture.working_head, capture.transaction_chain_hash,
      *pipeline_params);
  try {
    capture.materialization =
        state->pipeline_guard->commit_graph_->CaptureMaterializationWithSerializedPipelineState(
            serialized);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return nullptr;
  }
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
