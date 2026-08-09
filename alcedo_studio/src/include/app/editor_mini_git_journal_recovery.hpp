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
#include "sleeve/storage.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Validate a mini-Git WAL against durable history without creating a shadow
/// graph and without folding WAL records into DuckDB.
///
/// - empty WAL: accepted, no work
/// - fully covered by DB HEAD: clear the entire WAL
/// - contiguous missing suffix: accepted and left for EnsureWorkingState to
///   apply on the unique history instance + live pipeline via operator APIs
/// - broken / unaligned: isolate the journal file and return an error
///
/// Owner/lifetime: constructed on the recovery thread with Storage.
/// Journal file handles are owned by MiniGitJournal, loaded anew on each
/// Recovery call.
///
/// Thread context: call from the save worker while the global save lock is held,
/// or from the editor open/recovery path. No internal mutex.
class EditorMiniGitJournalRecovery final {
 public:
  /// Outcome of one Recovery call.
  struct RecoveryResult {
    /// True when the journal is empty, fully covered (and cleared), or a valid
    /// contiguous extension left for live attach. False when isolated/broken.
    bool        accepted     = false;
    /// True only when this path itself wrote DuckDB (always false under the
    /// simplified model — normal save owns DB writes after live attach).
    bool        materialized = false;
    std::string error;
  };

  /// @param storage  Non-null Storage used to obtain a DuckDB connection.
  explicit EditorMiniGitJournalRecovery(std::shared_ptr<Storage> storage);

  /// Validate WAL for the given element against durable history. See class docs.
  auto        Recover(sl_element_id_t element_id, const std::filesystem::path& journal_path,
                      std::string* error = nullptr) -> RecoveryResult;

  /// Truncate a journal file to zero bytes (discarding all records). Safe to
  /// call on a non-existent or already-empty path; returns success in those cases.
  static auto TruncateJournalFile(const std::filesystem::path& path, std::string* error = nullptr)
      -> bool;

 private:
  std::shared_ptr<Storage> storage_;
};

}  // namespace alcedo
