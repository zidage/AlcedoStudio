//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_state_detail.hpp"

#include <QtGlobal>
#include <ctime>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app/pipeline_root_state.hpp"
#include "app/pipeline_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
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
  std::shared_ptr<HistoryWorkingState> state;
  {
    std::scoped_lock lock(mutex_);
    const auto       existing = working_states_.find(element_id);
    if (existing != working_states_.end()) state = existing->second;
  }
  if (!state) {
    if (error) {
      *error = "Editor history is not acquired for image " + std::to_string(element_id);
    }
    return nullptr;
  }
  return state;
}

auto EditorHistoryState::AcquireWorkingState(sl_element_id_t element_id, std::string* error)
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
  auto lease = pipeline_port->AcquireLease(element_id, error);
  if (!lease.has_value()) {
    if (error && error->empty()) *error = "Editor history lease is unavailable";
    return nullptr;
  }
  // Every return below that does not publish the state returns the lease.
  struct LeaseReturn {
    EditorSessionPipelinePort& port;
    sl_element_id_t            element_id;
    bool                       kept = false;
    ~LeaseReturn() {
      if (!kept) port.ReleaseLease(element_id);
    }
  } lease_return{*pipeline_port, element_id};

  auto state      = std::make_shared<HistoryWorkingState>();
  state->graph    = lease->graph_;
  state->root     = lease->root_;
  state->document = lease->document_;

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
  state->journal             = journal;

  // Attach the WAL against the unique history instance: no shadow CommitGraph copy.
  const auto journal_records = journal->records();
  if (!journal_records.empty()) {
    const auto alignment =
        alcedo::MiniGitWorkingHistory::AlignJournalWithStoredHead(*state->graph, journal_records);
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
    } else {
      // Contiguous missing suffix: build the recovered history and its document first; nothing
      // is swapped in until both exist.
      const auto expected_materialized = state->graph->GetImageEditState();
      auto       recovered_graph       = *state->graph;
      std::vector<alcedo::MiniGitJournalRecord> missing(
          journal_records.begin() + static_cast<std::ptrdiff_t>(alignment.missing_from_index),
          journal_records.end());
      std::string replay_error;
      if (!alcedo::MiniGitWorkingHistory::Replay(recovered_graph, missing, &replay_error)) {
        if (error) *error = replay_error;
        std::string isolate_error;
        (void)alcedo::MiniGitJournal::IsolateJournalFile(journal->path(), &isolate_error);
        return nullptr;
      }
      const auto recovered_head = recovered_graph.GetActiveVersionRef().head_commit_hash;
      auto       recovered_document =
          alcedo::BuildDocumentFromRoot(recovered_graph, *state->root, recovered_head, error);
      if (!recovered_document) {
        std::string isolate_error;
        (void)alcedo::MiniGitJournal::IsolateJournalFile(journal->path(), &isolate_error);
        return nullptr;
      }

      // Swap: the recovered history and document become the working state.
      *state->graph = std::move(recovered_graph);
      state->document->Replace(recovered_document);
      state->recovered_head = true;

      // Persist the recovered history with the checkpoint of its document in one transaction,
      // then clear the WAL. A failed write keeps the recovered state in memory and the WAL on
      // disk for the next save.
      if (auto pipeline_service = PipelineMapper()) {
        std::string persist_error;
        if (!pipeline_service->PersistEditorHistory(*state->graph, expected_materialized,
                                                    *recovered_document, &persist_error)) {
          if (error) *error = persist_error;
        } else {
          if (!journal->TruncateMaterialized(error)) return nullptr;
          state->recovered_head = false;
        }
      }
    }
  }

  state->history = std::make_unique<alcedo::MiniGitWorkingHistory>(state->graph, journal);

  std::scoped_lock lock(mutex_);
  const auto [it, inserted] = working_states_.emplace(element_id, state);
  lease_return.kept         = inserted;
  return inserted ? state : it->second;
}

auto EditorHistoryState::PeekWorkingState(sl_element_id_t element_id) const
    -> std::shared_ptr<HistoryWorkingState> {
  std::scoped_lock lock(mutex_);
  const auto       existing = working_states_.find(element_id);
  return existing == working_states_.end() ? nullptr : existing->second;
}

void EditorHistoryState::ReleaseState(sl_element_id_t element_id, bool discard_unmaterialized) {
  std::shared_ptr<HistoryWorkingState>       state;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  {
    std::scoped_lock lock(mutex_);
    const auto       it = working_states_.find(element_id);
    if (it == working_states_.end()) return;
    state = std::move(it->second);
    working_states_.erase(it);
    pipeline_port = pipeline_port_.lock();
  }
  // Clear the journal before the lease returns: from then on the next acquire may read it.
  if (discard_unmaterialized && state->journal) {
    std::string error;
    if (!state->journal->TruncateMaterialized(&error)) {
      qWarning("Editor history of image %llu: discarded changes stay in the journal: %s",
               static_cast<unsigned long long>(element_id), error.c_str());
    }
  }
  if (pipeline_port) pipeline_port->ReleaseLease(element_id);
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
  if (!state->history) {
    if (error) *error = "Editor history graph is unavailable";
    return false;
  }
  return state->history->working_head() !=
         state->graph->GetImageEditState().materialized_head_commit_hash;
}

auto EditorHistoryState::JournalPathResolver() const
    -> std::function<std::filesystem::path(sl_element_id_t)> {
  std::scoped_lock lock(mutex_);
  return services_.mini_git_journal_path;
}

void EditorHistoryState::PublishWorkingSnapshots(sl_element_id_t element_id) {
  const auto state = PeekWorkingState(element_id);
  if (!state || !state->history) {
    return;
  }
  std::shared_ptr<const alcedo::PipelineGraphSnapshot> preview;
  try {
    preview = state->document->PublishPreview();
  } catch (const std::exception& ex) {
    qWarning("Editor history: preview snapshot of image %llu was not published: %s",
             static_cast<unsigned long long>(element_id), ex.what());
    return;
  }
  if (state->HasUncommittedLiveValues()) {
    return;
  }
  const auto service = PipelineMapper();
  if (!service) {
    return;
  }
  const auto head    = state->history->working_head();
  const auto chain   = state->history->transaction_chain_hash();
  const auto lineage = preview->Lineage();
  if (state->last_published_commit.has_value() &&
      state->last_published_commit->lineage == lineage &&
      state->last_published_commit->head == head && state->last_published_commit->chain == chain) {
    return;
  }
  try {
    // The preview froze a document without uncommitted values, so it is the committed document.
    service->PublishCommitted(alcedo::PipelineGraphSnapshot::Committed(
        preview->SharedDocument(), element_id, lineage, head, chain));
    state->last_published_commit = HistoryWorkingState::PublishedCommit{lineage, head, chain};
  } catch (const std::exception& ex) {
    qWarning("Editor history: committed snapshot of image %llu was not published: %s",
             static_cast<unsigned long long>(element_id), ex.what());
  }
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

auto EditorHistoryState::BuildDocumentForHead(const HistoryWorkingState&        state,
                                              const alcedo::head_commit_hash_t& head,
                                              std::string*                      error)
    -> std::shared_ptr<alcedo::PipelineDocument> {
  if (!state.graph || !state.root) {
    if (error) *error = "Editor history root is unavailable for Version replay";
    return nullptr;
  }
  return alcedo::BuildDocumentFromRoot(*state.graph, *state.root, head, error);
}

}  // namespace alcedo::ui
