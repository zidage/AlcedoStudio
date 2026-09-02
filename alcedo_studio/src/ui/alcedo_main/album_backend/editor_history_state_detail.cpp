//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

#include <ctime>
#include <mutex>
#include <utility>
#include <vector>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/pipeline_history_applier.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "json.hpp"
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
  // LoadEditorPipeline already installed the live document from a matching
  // checkpoint or from first-parent replay. Panel state is read from that
  // executor, never from a stored CPU-parameter checkpoint blob.
  if (!guard->pipeline_) {
    if (error) *error = "Mini-Git working state requires a live pipeline executor";
    return nullptr;
  }
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
      alcedo::PipelineDocument prior_document;
      nlohmann::json           prior_params;
      if (guard->pipeline_ && guard->document_) {
        std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
        prior_document = alcedo::ClonePipelineDocument(*guard->document_);
        prior_params   = guard->pipeline_->ExportPipelineParams();
      }

      auto restore_recovery = [&]() {
        *guard->commit_graph_ = prior_graph;
        state->committed_snapshot = prior_snap;
        if (!guard->pipeline_ || !guard->document_) {
          return;
        }
        std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
        alcedo::BindLivePipelineDocument(*guard, alcedo::ClonePipelineDocument(prior_document));
        guard->pipeline_->ImportPipelineParams(prior_params);
        guard->pipeline_->SetExecutionStages();
      };

      std::vector<alcedo::MiniGitJournalRecord> missing(
          journal_records.begin() +
              static_cast<std::ptrdiff_t>(alignment.missing_from_index),
          journal_records.end());

      std::string replay_error;
      if (!alcedo::MiniGitWorkingHistory::Replay(*guard->commit_graph_, missing, &replay_error)) {
        restore_recovery();
        if (error) *error = replay_error;
        std::string isolate_error;
        (void)alcedo::MiniGitJournal::IsolateJournalFile(journal->path(), &isolate_error);
        return nullptr;
      }

      const auto recovered_head = guard->commit_graph_->GetActiveVersionRef().head_commit_hash;
      if (auto pipeline_service = PipelineMapper()) {
        if (!pipeline_service->RebuildActiveEditorPipeline(guard, error, state->mask_store)) {
          restore_recovery();
          std::string isolate_error;
          (void)alcedo::MiniGitJournal::IsolateJournalFile(journal->path(), &isolate_error);
          return nullptr;
        }
      } else if (!ReplayWorkingDocumentFromImmutableRoot(*state, recovered_head, error)) {
        restore_recovery();
        std::string isolate_error;
        (void)alcedo::MiniGitJournal::IsolateJournalFile(journal->path(), &isolate_error);
        return nullptr;
      }

      if (guard->pipeline_) {
        try {
          std::unique_lock<std::mutex> render_lock(guard->pipeline_->GetRenderLock());
          if (!MakeAdjustmentSnapshotFromLivePipeline(*guard->pipeline_, &state->committed_snapshot,
                                                      error)) {
            restore_recovery();
            return nullptr;
          }
        } catch (const std::exception& ex) {
          restore_recovery();
          if (error) *error = ex.what();
          return nullptr;
        }
      }

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
  // history remains authoritative and a new checkpoint writeback is required.
  const auto& post_wal_state = guard->commit_graph_->GetImageEditState();
  if (!alcedo::CheckpointMatchesLogicalHead(post_wal_state, guard->working_head_commit_hash(),
                                            guard->transaction_chain_hash())) {
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

auto EditorHistoryState::ReplayWorkingDocumentFromImmutableRoot(
    HistoryWorkingState& state, const alcedo::head_commit_hash_t& head, std::string* error)
    -> bool {
  if (!state.pipeline_guard || !state.pipeline_guard->pipeline_ ||
      !state.pipeline_guard->document_ || !state.pipeline_guard->commit_graph_) {
    if (error) *error = "Live pipeline document is unavailable for Version replay";
    return false;
  }
  if (!state.pipeline_guard->root_document_) {
    if (error) *error = "Immutable root document is missing for Version replay";
    return false;
  }

  std::vector<alcedo::EditCommit> commits;
  try {
    commits = alcedo::FirstParentCommitsForHead(*state.pipeline_guard->commit_graph_, head);
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  alcedo::PipelineDocument prior_document;
  nlohmann::json           prior_params;
  try {
    auto render_lock = LockLivePipeline(*state.pipeline_guard->pipeline_);
    prior_document   = alcedo::ClonePipelineDocument(*state.pipeline_guard->document_);
    prior_params     = state.pipeline_guard->pipeline_->ExportPipelineParams();
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
    return false;
  }

  alcedo::PipelineHistoryApplyContext context;
  context.mask_store = state.mask_store;
  auto replayed      = alcedo::ReplayPipelineDocumentFromRoot(
      *state.pipeline_guard->root_document_, commits, error, context);
  if (!replayed.has_value()) {
    return false;
  }
  if (!alcedo::VerifyPersistentMaskAssets(*replayed, state.mask_store, error)) {
    return false;
  }

  auto restore_live = [&]() {
    alcedo::BindLivePipelineDocument(*state.pipeline_guard,
                                     alcedo::ClonePipelineDocument(prior_document));
    state.pipeline_guard->pipeline_->ImportPipelineParams(prior_params);
    state.pipeline_guard->pipeline_->SetExecutionStages();
  };

  try {
    auto render_lock = LockLivePipeline(*state.pipeline_guard->pipeline_);
    alcedo::BindLivePipelineDocument(*state.pipeline_guard, std::move(*replayed));
    if (!alcedo::ApplyVersionHeadToLivePipeline(*state.pipeline_guard->pipeline_,
                                                *state.pipeline_guard->commit_graph_, head,
                                                error)) {
      restore_live();
      return false;
    }
    state.pipeline_guard->pipeline_->SetExecutionStages();
    return true;
  } catch (const std::exception& ex) {
    try {
      auto render_lock = LockLivePipeline(*state.pipeline_guard->pipeline_);
      restore_live();
    } catch (...) {
    }
    if (error) *error = ex.what();
    return false;
  }
}

}  // namespace alcedo::ui
