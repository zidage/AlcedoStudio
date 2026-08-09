//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_journal_recovery.hpp"

#include <fstream>
#include <utility>

#include "edit/history/mini_git_working_history.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"

namespace alcedo {
namespace {

void SetError(std::string* error, std::string message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

// ── Construction ────────────────────────────────────────────────────────────

EditorMiniGitJournalRecovery::EditorMiniGitJournalRecovery(std::shared_ptr<Storage> storage)
    : storage_(std::move(storage)) {
  if (!storage_) {
    throw std::invalid_argument("EditorMiniGitJournalRecovery requires Storage");
  }
}

// ── Recovery ────────────────────────────────────────────────────────────────

auto EditorMiniGitJournalRecovery::Recover(sl_element_id_t              element_id,
                                           const std::filesystem::path& journal_path,
                                           std::string*                 error) -> RecoveryResult {
  // Startup recovery without a live pipeline: validate WAL continuity against
  // durable history, clear fully-covered leftover WAL, leave contiguous missing
  // suffixes for EnsureWorkingState (unique history + live operator apply), and
  // isolate broken journals. Never fold WAL into DuckDB and never create a
  // shadow CommitGraph.
  RecoveryResult result;

  MiniGitJournal journal(journal_path);
  if (!journal.Load(error)) {
    result.error = error != nullptr ? *error : "journal load failed";
    // Corrupt on-disk log: isolate so a later open does not re-read it silently.
    std::string isolate_error;
    (void)MiniGitJournal::IsolateJournalFile(journal_path, &isolate_error);
    return result;
  }

  if (journal.records().empty()) {
    result.accepted     = true;
    result.materialized = false;
    return result;
  }

  try {
    auto               db_guard = storage_->GetDatabase().GetConnectionGuard();
    auto               db_lock  = db_guard.Lock();
    CommitGraphStore graph_service(db_guard.conn_);
    auto               stored_graph = graph_service.LoadGraph(element_id);
    if (!stored_graph.has_value()) {
      SetError(error, "mini-Git recovery requires a durable commit graph");
      result.error = error != nullptr ? *error : "missing graph";
      std::string isolate_error;
      (void)MiniGitJournal::IsolateJournalFile(journal_path, &isolate_error);
      return result;
    }

    const auto alignment =
        MiniGitWorkingHistory::AlignJournalWithStoredHead(*stored_graph, journal.records());
    if (!alignment.accepted || alignment.broken) {
      SetError(error, alignment.error.empty()
                          ? "mini-Git journal cannot be recovered against stored history"
                          : alignment.error);
      result.error = error != nullptr ? *error : "journal recovery rejected";
      std::string isolate_error;
      (void)MiniGitJournal::IsolateJournalFile(journal_path, &isolate_error);
      return result;
    }

    if (alignment.fully_covered) {
      // Crash after DB success, before WAL clear: discard leftover log only.
      std::string truncate_error;
      if (!TruncateJournalFile(journal_path, &truncate_error)) {
        SetError(error, truncate_error.empty() ? "failed to clear fully-covered WAL"
                                               : truncate_error);
        result.error = error != nullptr ? *error : "WAL clear failed";
        return result;
      }
      result.accepted     = true;
      result.materialized = false;
      return result;
    }

    // Contiguous missing suffix: leave WAL intact. EnsureWorkingState applies
    // missing records to the unique history instance and live pipeline, then
    // runs the normal save path and clears WAL.
    result.accepted     = true;
    result.materialized = false;
    return result;
  } catch (const std::exception& e) {
    SetError(error, e.what());
    result.error = e.what();
    return result;
  } catch (...) {
    SetError(error, "mini-Git recovery failed");
    result.error = error != nullptr ? *error : "recovery failed";
    return result;
  }
}

// ── Static helpers ──────────────────────────────────────────────────────────

auto EditorMiniGitJournalRecovery::TruncateJournalFile(const std::filesystem::path& path,
                                                       std::string*                 error) -> bool {
  try {
    if (path.empty() || !std::filesystem::exists(path)) {
      return true;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open()) {
      SetError(error, "mini-Git journal file could not be truncated");
      return false;
    }
    output.flush();
    if (!output.good()) {
      SetError(error, "mini-Git journal file truncate failed");
      return false;
    }
    return true;
  } catch (const std::exception& e) {
    SetError(error, e.what());
  } catch (...) {
    SetError(error, "mini-Git journal truncate failed");
  }
  return false;
}

}  // namespace alcedo
