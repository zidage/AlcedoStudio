//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_mini_git_journal_fold.hpp"

#include <utility>

namespace alcedo {

auto EditorMiniGitJournalFold::Fold(CommitGraph&                             graph,
                                    const std::vector<MiniGitJournalRecord>& records,
                                    std::string*                             error) -> FoldResult {
  std::size_t applied_from = 0;
  if (!MiniGitWorkingHistory::ReplaySkippingMaterializedPrefix(graph, records, &applied_from,
                                                               error)) {
    FoldResult result;
    result.accepted = false;
    result.error    = error != nullptr ? *error : "journal fold failed";
    return result;
  }
  FoldResult result;
  result.accepted = true;
  return result;
}

}  // namespace alcedo
