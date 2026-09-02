//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/commit_types.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "json.hpp"

namespace alcedo {

namespace edit_history_test {
struct CommitClockAccess;
struct EditCommitAccess;
}  // namespace edit_history_test

/**
 * @brief Immutable content-addressed commit object.
 *
 * All commits are first-parent commits whose payload is a canonical PipelineEditBatch.
 * Root-child commits have nullopt first_parent_hash.
 */
class EditCommit {
 public:
  EditCommit() = default;

  /**
   * @brief Create a first-parent commit whose payload is a typed batch.
   *
   * @param root_id Image root bound into the commit hash.
   * @param first_parent Prior head, or nullopt for a root-child commit.
   * @param payload Validated typed batch. Non-batch payloads are rejected.
   * @return Finalized immutable commit. Does not insert into a graph.
   */
  static auto MakePipelineEdit(root_id_t root_id, head_commit_hash_t first_parent,
                               PipelineEditBatch payload) -> EditCommit;

  auto        GetCommitHash() const -> commit_hash_t { return commit_hash_; }
  auto        GetRootId() const -> root_id_t { return root_id_; }
  auto        GetFirstParentHash() const -> head_commit_hash_t { return first_parent_hash_; }
  auto        GetCreatedAtNs() const -> std::uint64_t { return created_at_ns_; }
  auto        GetPayloadJSON() const -> const nlohmann::json& { return edit_payload_; }

  /// Host-endianness-independent hash input bytes for this commit object.
  auto CanonicalHashInput() const -> std::vector<std::uint8_t>;
  auto ComputeCommitHash() const -> commit_hash_t;
  void FinalizeHash();

  /// Validate parent cardinality and payload shape without rehashing.
  void ValidateStructure() const;

  auto ToJSON() const -> nlohmann::json;
  static auto FromJSON(const nlohmann::json& j) -> EditCommit;

 private:
  friend struct edit_history_test::EditCommitAccess;

  static auto MakePipelineEditAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                          std::uint64_t created_at_ns, PipelineEditBatch payload)
      -> EditCommit;

  commit_hash_t      commit_hash_{};
  root_id_t          root_id_{};
  head_commit_hash_t first_parent_hash_ = std::nullopt;
  std::uint64_t      created_at_ns_     = 0;
  nlohmann::json     edit_payload_      = nlohmann::json::object();
};

/**
 * @brief Process-wide, thread-safe, strictly increasing commit timestamp clock.
 *
 * All instances share one sequence. Timestamp exhaustion (would wrap past UINT64_MAX) throws.
 * There is no public reset: tests use edit_history_test::CommitClockAccess only.
 */
class CommitClock {
 public:
  CommitClock() = default;

  /// Return max(now_ns, previous + 1) on the process-wide clock and advance it.
  auto        Next(std::uint64_t now_ns) -> std::uint64_t { return NextGlobal(now_ns); }

  /// Process-wide previous stamp after the last successful NextGlobal call.
  static auto PreviousGlobal() -> std::uint64_t;

  /// Process-wide next stamp. Thread-safe.
  static auto NextGlobal(std::uint64_t now_ns) -> std::uint64_t;

 private:
  friend struct edit_history_test::CommitClockAccess;
  static void ResetGlobalForTesting(std::uint64_t previous_ns);
};

/// Canonical little-endian hash input for the root chain: H(chain_format_version, root_id).
auto RootChainHashInput(const root_id_t& root_id) -> std::vector<std::uint8_t>;

/// Root chain label before any commits (params at root with no first-parent commits applied).
auto ComputeRootChainHash(const root_id_t& root_id) -> transaction_chain_hash_t;

/// Canonical little-endian fold input: H(previous_chain_hash, commit_hash).
auto TransactionChainFoldInput(const transaction_chain_hash_t& previous,
                               const commit_hash_t& commit_hash) -> std::vector<std::uint8_t>;

/// Fold **one commit** into the chain hash. Call once when history head advances to that
/// commit — never once per SetOperator.
auto FoldTransactionChainHash(const transaction_chain_hash_t& previous,
                              const commit_hash_t& commit_hash) -> transaction_chain_hash_t;

/// Fold an ordered first-parent commit list starting from the root chain hash.
auto FoldFirstParentChain(const root_id_t&                  root_id,
                          const std::vector<commit_hash_t>& first_parent_commits)
    -> transaction_chain_hash_t;

}  // namespace alcedo
