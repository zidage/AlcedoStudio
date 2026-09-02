//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

#include <ctime>
#include <mutex>
#include <utility>
#include <vector>

#include "app/editor_adjustment_pipeline.hpp"
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
  const bool have_serialized_params =
      image_state.serialized_pipeline_state.has_value() &&
      image_state.serialized_pipeline_state->is_object() &&
      image_state.serialized_pipeline_state->contains("pipeline_params");
  // A matching checkpoint can be scraped as JSON. After a history rebuild
  // (library Paste, stale/missing checkpoint) LoadEditorPipeline has already
  // applied the Version tip to the live executor — that table is the panel
  // authority. Using an empty/stale snapshot here leaves LUT on the image
  // while LUTPanel highlights None.
  if (have_serialized_params && !guard->serialized_state_needs_writeback_) {
    if (!MakeAdjustmentSnapshotFromPipelineParams(
            image_state.serialized_pipeline_state->at("pipeline_params"),
            &state->committed_snapshot, error)) {
      return nullptr;
    }
  } else if (guard->serialized_state_needs_writeback_ && guard->pipeline_) {
    try {
      std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
      if (!MakeAdjustmentSnapshotFromLivePipeline(*guard->pipeline_, &state->committed_snapshot,
                                                  error)) {
        return nullptr;
      }
    } catch (const std::exception& ex) {
      if (error) *error = ex.what();
      return nullptr;
    }
  } else if (have_serialized_params) {
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

  // Attach WAL against the unique history instance — no shadow CommitGraph copy,
  // no ApplyRecoveredRecordToSnapshot reducer.
  const auto journal_records = journal->records();
  if (journal_records.empty()) {
    guard->dirty_ = false;
    state->recovered_head = false;
  } else {
    const auto alignment = alcedo::MiniGitWorkingHistory::AlignJournalWithStoredHead(
        *guard->commit_graph_, journal_records);
    if (!alignment.accepted || alignment.broken) {
      std::string isolate_error;
      (void)alcedo::MiniGitJournal::IsolateJournalFile(journal->path(), &isolate_error);
      if (error) {
        *error = alignment.error.empty()
                     ? "Mini-Git journal cannot be recovered against stored history"
                     : alignment.error;
      }
      return nullptr;
    }

    if (alignment.fully_covered) {
      // Crash after durable save, before WAL clear: discard leftover log only.
      if (!journal->TruncateMaterialized(error)) return nullptr;
      guard->dirty_ = false;
      state->recovered_head = false;
    } else {
      // Contiguous missing suffix: apply into unique graph + live pipeline.
      const auto prior_graph = *guard->commit_graph_;
      const auto prior_snap  = state->committed_snapshot;
      const auto expected_materialized = prior_graph.GetImageEditState();

      std::vector<alcedo::MiniGitJournalRecord> missing(
          journal_records.begin() +
              static_cast<std::ptrdiff_t>(alignment.missing_from_index),
          journal_records.end());

      std::string replay_error;
      if (!alcedo::MiniGitWorkingHistory::Replay(*guard->commit_graph_, missing, &replay_error)) {
        *guard->commit_graph_ = prior_graph;
        if (error) *error = replay_error;
        std::string isolate_error;
        (void)alcedo::MiniGitJournal::IsolateJournalFile(journal->path(), &isolate_error);
        return nullptr;
      }

      alcedo::EditorRenderAdjustmentSnapshot derived;
      if (!SnapshotAtHead(state->root_snapshot, *guard->commit_graph_,
                          guard->commit_graph_->GetActiveVersionRef().head_commit_hash, &derived,
                          error)) {
        *guard->commit_graph_ = prior_graph;
        return nullptr;
      }

      if (guard->pipeline_) {
        std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
        if (!alcedo::ApplyEditorAdjustmentSnapshot(*guard->pipeline_, derived, error)) {
          *guard->commit_graph_ = prior_graph;
          return nullptr;
        }
      }

      state->committed_snapshot = std::move(derived);
      guard->dirty_ = true;
      guard->serialized_state_needs_writeback_ = true;
      state->recovered_head = true;

      // Normal save APIs for recovery result: history persist + pipeline checkpoint.
      if (auto pipeline_service = PipelineMapper()) {
        std::string persist_error;
        if (!pipeline_service->PersistEditorHistoryState(guard, expected_materialized,
                                                         &persist_error)) {
          // Keep recovered memory/live state; leave WAL for retry.
          if (error) *error = persist_error;
        } else {
          if (guard->pipeline_) {
            try {
              pipeline_service->SavePipeline(guard);
            } catch (const std::exception& ex) {
              if (error) *error = ex.what();
              // Leave WAL intact when pipeline checkpoint fails.
              goto attach_history;
            } catch (...) {
              if (error) *error = "Recovered pipeline checkpoint save failed";
              goto attach_history;
            }
          }
          if (!journal->TruncateMaterialized(error)) {
            *guard->commit_graph_ = prior_graph;
            state->committed_snapshot = prior_snap;
            return nullptr;
          }
          guard->dirty_ = false;
          guard->serialized_state_needs_writeback_ = false;
          state->recovered_head = false;
          try {
            guard->commit_graph_->MaterializeActiveHeadInMemory();
          } catch (const std::exception& ex) {
            if (error) *error = ex.what();
            return nullptr;
          }
        }
      }
    }
  }

attach_history:
  state->history =
      std::make_unique<alcedo::MiniGitWorkingHistory>(guard->commit_graph_, journal);

  // After WAL attach, if checkpoint identity still disagrees with logical head,
  // install the derived adjustment snapshot on the unique live executor.
  const auto& post_wal_state = guard->commit_graph_->GetImageEditState();
  if (!alcedo::CheckpointMatchesLogicalHead(post_wal_state, guard->working_head_commit_hash(),
                                            guard->transaction_chain_hash())) {
    if (guard->pipeline_) {
      std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
      if (!alcedo::ApplyEditorAdjustmentSnapshot(*guard->pipeline_, state->committed_snapshot,
                                                 error)) {
        return nullptr;
      }
    }
    guard->serialized_state_needs_writeback_ = true;
  }

  std::scoped_lock lock(mutex_);
  const auto [it, inserted] = working_states_.emplace(element_id, state);
  return inserted ? state : it->second;
}

auto EditorHistoryState::PeekWorkingState(sl_element_id_t element_id) const
    -> std::shared_ptr<HistoryWorkingState> {
  std::scoped_lock lock(mutex_);
  const auto       existing = working_states_.find(element_id);
  return existing == working_states_.end() ? nullptr : existing->second;
}

void EditorHistoryState::ReleaseState(sl_element_id_t element_id) {
  std::scoped_lock lock(mutex_);
  working_states_.erase(element_id);
}

auto EditorHistoryState::PipelinePort() const -> std::shared_ptr<EditorSessionPipelinePort> {
  std::scoped_lock lock(mutex_);
  return pipeline_port_.lock();
}

auto EditorHistoryState::PipelineMapper() const
    -> std::shared_ptr<alcedo::PipelineMgmtService> {
  auto port = PipelinePort();
  return port ? port->PipelineMapper() : nullptr;
}

auto EditorHistoryState::HasUnmaterializedChanges(sl_element_id_t element_id, std::string* error)
    -> bool {
  auto state = PeekWorkingState(element_id);
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

void EditorHistoryState::RecordPublishedRenderReason(
    std::optional<alcedo::EditorRenderReason> reason) {
  std::scoped_lock lock(mutex_);
  last_published_render_reason_ = reason;
}

auto EditorHistoryState::LastPublishedRenderReason() const
    -> std::optional<alcedo::EditorRenderReason> {
  std::scoped_lock lock(mutex_);
  return last_published_render_reason_;
}

}  // namespace alcedo::ui
