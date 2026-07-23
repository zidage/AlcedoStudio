//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "sleeve/storage_service.hpp"
#include "type/type.hpp"

namespace alcedo {

class EditorMiniGitCommitWriter;

/// Load an existing mini-Git journal, detect already-materialized prefixes
/// (crash after DuckDB commit before journal truncation), fold remaining records,
/// write them via EditorMiniGitCommitWriter, and truncate the journal file.
///
/// Owner/lifetime: constructed on the save worker or recovery thread with
/// StorageService. Journal file handles are owned by MiniGitJournal, loaded
/// anew on each Recovery call; no persistent file descriptors are retained.
///
/// Thread context: call from the save worker while the global save lock is held,
/// or from the editor open/recovery path. No internal mutex.
class EditorMiniGitJournalRecovery final {
 public:
  /// Outcome of one Recovery call.
  struct RecoveryResult {
    /// True when the journal was successfully recovered (even if empty).
    bool        accepted     = false;
    /// True when DuckDB was actually written (false for empty or already-materialized).
    bool        materialized = false;
    std::string error;
  };

  /// @param storage  Non-null StorageService used to obtain a DuckDB connection.
  explicit EditorMiniGitJournalRecovery(std::shared_ptr<StorageService> storage);

  /// Recover a journal for the given element: load records, skip already-
  /// materialized prefixes, fold remaining records through a CommitWriter
  /// transaction, then truncate the journal file. Never inserts a commit twice.
  ///
  /// @param element_id    Image element to recover.
  /// @param journal_path  Path to the durable mini-Git journal.
  /// @param error         Output error string when accepted is false (may be null).
  /// @return              RecoveryResult with accepted=true on success.
  auto        Recover(sl_element_id_t element_id, const std::filesystem::path& journal_path,
                      std::string* error = nullptr) -> RecoveryResult;

  /// Truncate a journal file to zero bytes (discarding all records). Safe to
  /// call on a non-existent or already-empty path; returns success in those cases.
  ///
  /// @param path   Path to the mini-Git journal file.
  /// @param error  Output error string on failure (may be null).
  /// @return       True when the file was truncated or does not exist.
  static auto TruncateJournalFile(const std::filesystem::path& path, std::string* error = nullptr)
      -> bool;

 private:
  std::shared_ptr<StorageService> storage_;
};

}  // namespace alcedo
