//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_checkpoint.hpp"

#include <exception>
#include <filesystem>
#include <utility>

#include "app/editor_mini_git_materializer.hpp"
#include "app/pipeline_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "json.hpp"
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
  if (!state->history || !state->journal) {
    if (error) *error = "Mini-Git save capture requires an immutable history state";
    return nullptr;
  }
  // Only committed state is persisted. Every persisting seal settles open input first, so an
  // uncommitted value here is a caller defect, not a state to save.
  if (state->HasUncommittedLiveValues()) {
    if (error) *error = "Mini-Git save capture cannot run while an editor preview is unsettled";
    return nullptr;
  }

  // Single live identity: CommitGraph active Version head is the only logical head.
  // Build one materialization from the graph, then project capture fields from it. The capture
  // runs on the owner thread, the only writer of the working document, so it reads it directly.
  auto&      graph         = *state->graph;
  const auto logical_head  = graph.GetActiveVersionRef().head_commit_hash;
  const auto logical_chain = graph.ChainHashForHead(logical_head);
  const auto serialized    = alcedo::MakeEditorSerializedPipelineState(
      graph.GetRootId(), logical_head, logical_chain, state->document->Document());
  const auto journal_snapshot = state->journal->Snapshot();

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
  try {
    state->graph->MaterializeActiveHeadInMemory();
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }
  return true;
}

}  // namespace alcedo::ui
