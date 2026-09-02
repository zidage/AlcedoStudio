//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

// Test-only access to CommitClock reset. Production translation units must not include this header.

#include <cstdint>
#include <utility>

#include "edit/history/edit_commit.hpp"

namespace alcedo::edit_history_test {

struct CommitClockAccess {
  static void ResetGlobal(std::uint64_t previous_ns = 0) {
    CommitClock::ResetGlobalForTesting(previous_ns);
  }
};

struct EditCommitAccess {
  static auto MakeEditAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                  std::uint64_t created_at_ns, OrdinaryEditPayload payload)
      -> EditCommit {
    return EditCommit::MakeEditAtTimestamp(root_id, std::move(first_parent), created_at_ns,
                                           std::move(payload));
  }

  static auto MakePipelineEditAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                          std::uint64_t created_at_ns, PipelineEditBatch payload)
      -> EditCommit {
    return EditCommit::MakePipelineEditAtTimestamp(root_id, std::move(first_parent), created_at_ns,
                                                   std::move(payload));
  }

  static auto MakeMergeAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                   commit_hash_t second_parent, std::uint64_t created_at_ns,
                                   MergeEditPayload payload) -> EditCommit {
    return EditCommit::MakeMergeAtTimestamp(root_id, std::move(first_parent), second_parent,
                                            created_at_ns, std::move(payload));
  }
};

}  // namespace alcedo::edit_history_test
