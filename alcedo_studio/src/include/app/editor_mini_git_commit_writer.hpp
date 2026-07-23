//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "edit/history/commit_graph.hpp"
#include "json.hpp"
#include "sleeve/storage_service.hpp"
#include "type/type.hpp"

namespace alcedo {

/// One DuckDB transaction that inserts commit objects and updates the checked-out
/// Version head, transaction-chain hash, serialized pipeline state, and recovery
/// metadata. The caller must supply a validated CommitGraphMaterialization; this
/// writer validates nothing beyond what DuckDB enforces.
///
/// Owner/lifetime: constructed on the save worker thread with a StorageService
/// reference. Connection and transaction state live only for the duration of one
/// Write call. Does not perform any pipeline replay or mutation.
///
/// Thread context: call from the save worker while the global save lock is held.
class EditorMiniGitCommitWriter final {
 public:
  /// Outcome of one Write call.
  struct WriteResult {
    bool        accepted = false;
    std::string error;
  };

  /// @param storage  Non-null StorageService used to obtain a DuckDB connection.
  explicit EditorMiniGitCommitWriter(std::shared_ptr<StorageService> storage);

  /// Atomically write commits, Version refs, ImageEditState, and serialized
  /// pipeline state from a validated materialization capture in one DuckDB
  /// transaction. On failure the transaction is rolled back and no durable
  /// state changes.
  ///
  /// @param materialization  Validated capture from the live CommitGraph.
  /// @param error            Output error string when accepted is false (may be null).
  /// @return                 WriteResult with accepted=true on success.
  auto Write(const CommitGraphMaterialization& materialization, std::string* error = nullptr)
      -> WriteResult;

 private:
  std::shared_ptr<StorageService> storage_;
};

}  // namespace alcedo
