//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <vector>

#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"

namespace alcedo {

/// Pure algorithm: validate and replay MiniGitJournalRecord entries against a
/// CommitGraph loaded from DuckDB, producing the folded head and transaction-chain
/// hash without performing any database or file I/O.
///
/// Owner/lifetime: constructed as a value type on the save worker thread with a
/// copy of the materialized graph. All inputs and outputs are immutable values.
/// Thread context: call from the save worker; no internal mutex.
class EditorMiniGitJournalFold final {
 public:
  /// Outcome of one fold operation. Either the fold succeeded (accepted=true)
  /// and the graph was updated in-place, or it failed with a diagnostic error.
  struct FoldResult {
    bool        accepted = false;
    std::string error;
  };

  /// Fold journal records onto a graph that was loaded from the materialized
  /// DuckDB state. Already-materialized prefixes (crash after DB commit before
  /// truncate) are skipped when the stored head has already advanced past them.
  /// The caller-provided graph is mutated to reflect the folded state.
  ///
  /// @param graph        Mutable graph loaded from the durable materialized state.
  /// @param records      Journal records captured at save-checkpoint start.
  /// @param error        Output error string when accepted is false (may be null).
  /// @return             FoldResult with accepted=true on success.
  static auto Fold(CommitGraph& graph, const std::vector<MiniGitJournalRecord>& records,
                   std::string* error) -> FoldResult;
};

}  // namespace alcedo
