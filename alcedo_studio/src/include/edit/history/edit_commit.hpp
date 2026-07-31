//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/commit_types.hpp"
#include "edit/operators/op_base.hpp"
#include "json.hpp"

namespace alcedo {

namespace edit_history_test {
struct CommitClockAccess;
struct EditCommitAccess;
}  // namespace edit_history_test

/**
 * @brief Canonical ordinary-edit payload used for hashing and forward replay.
 *
 * Field identity is operator + stage + optional field name. Before/after values and enabled
 * flags are stored so reconstruction never re-derives the user action.
 */
struct OrdinaryEditPayload {
  OperatorType      operator_type = OperatorType::UNKNOWN;
  PipelineStageName stage_name    = PipelineStageName::Basic_Adjustment;
  std::string       field_name;
  nlohmann::json    before_value   = nlohmann::json(nullptr);
  nlohmann::json    after_value    = nlohmann::json(nullptr);
  bool              before_enabled = false;
  bool              after_enabled  = true;

  auto              CanonicalJSON() const -> nlohmann::json;
  auto              ToJSON() const -> nlohmann::json { return CanonicalJSON(); }
  static auto       FromJSON(const nlohmann::json& j) -> OrdinaryEditPayload;
};

/**
 * @brief One UI-resolved field in a merge payload.
 *
 * `before_*` stores the live first-parent operator state so undo can reverse without
 * replaying the second parent or re-deriving values from the transfer package.
 */
struct MergeFieldDelta {
  OperatorType      operator_type = OperatorType::UNKNOWN;
  PipelineStageName stage_name    = PipelineStageName::Basic_Adjustment;
  std::string       field_name;
  nlohmann::json    before_value     = nlohmann::json(nullptr);
  bool              before_enabled   = false;
  nlohmann::json    resolved_value   = nlohmann::json(nullptr);
  bool              resolved_enabled = true;

  /// Field identity used for merge-payload canonicalization (parent order is separate).
  auto              IdentityKey() const -> std::string;
  auto              CanonicalJSON() const -> nlohmann::json;
  static auto       FromJSON(const nlohmann::json& j) -> MergeFieldDelta;
};

/**
 * @brief Complete merge payload: the full field delta transforming first-parent pipeline state
 * into the merge result. Reconstruction applies this payload; it does not re-run conflict UI.
 *
 * Field deltas are stored and hashed in identity-sorted order. Duplicate field identities are
 * rejected. Ordered parent hashes remain significant for the commit hash and are not part of this
 * payload.
 */
struct MergeEditPayload {
  std::vector<MergeFieldDelta> fields;

  /// Sort by field identity and reject duplicates. Called by MakeMerge and FromJSON.
  void                         CanonicalizeAndValidate();

  auto                         CanonicalJSON() const -> nlohmann::json;
  auto                         ToJSON() const -> nlohmann::json { return CanonicalJSON(); }
  static auto                  FromJSON(const nlohmann::json& j) -> MergeEditPayload;
};

/**
 * @brief Immutable content-addressed commit object.
 *
 * Ordinary edits have at most one parent (first_parent_hash; null means root). Merge commits have
 * ordered parents: first = checked-out branch, second = incoming branch. An Edit commit must not
 * carry a second parent; a Merge commit must carry exactly one second parent.
 */
class EditCommit {
 public:
  EditCommit() = default;

  /// Production factory. The creation timestamp comes from the process-wide monotonic clock.
  static auto MakeEdit(root_id_t root_id, head_commit_hash_t first_parent,
                       OrdinaryEditPayload payload) -> EditCommit;

  /// Production factory. The creation timestamp comes from the process-wide monotonic clock.
  static auto MakeMerge(root_id_t root_id, head_commit_hash_t first_parent,
                        commit_hash_t second_parent, MergeEditPayload payload) -> EditCommit;

  auto        GetCommitHash() const -> commit_hash_t { return commit_hash_; }
  auto        GetRootId() const -> root_id_t { return root_id_; }
  auto        GetFirstParentHash() const -> head_commit_hash_t { return first_parent_hash_; }
  auto GetSecondParentHash() const -> std::optional<commit_hash_t> { return second_parent_hash_; }
  auto GetCreatedAtNs() const -> std::uint64_t { return created_at_ns_; }
  auto GetKind() const -> EditCommitKind { return kind_; }
  auto GetPayloadJSON() const -> const nlohmann::json& { return edit_payload_; }

  /// Host-endianness-independent hash input bytes for this commit object.
  auto CanonicalHashInput() const -> std::vector<std::uint8_t>;
  auto ComputeCommitHash() const -> commit_hash_t;
  void FinalizeHash();

  /// Validate kind, parent cardinality, and payload shape without rehashing.
  void ValidateStructure() const;

  auto ToJSON() const -> nlohmann::json;
  static auto FromJSON(const nlohmann::json& j) -> EditCommit;

 private:
  friend struct edit_history_test::EditCommitAccess;

  static auto MakeEditAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                  std::uint64_t created_at_ns, OrdinaryEditPayload payload)
      -> EditCommit;
  static auto        MakeMergeAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                          commit_hash_t second_parent, std::uint64_t created_at_ns,
                                          MergeEditPayload payload) -> EditCommit;

  commit_hash_t      commit_hash_{};
  root_id_t          root_id_{};
  head_commit_hash_t first_parent_hash_            = std::nullopt;
  std::optional<commit_hash_t> second_parent_hash_ = std::nullopt;
  std::uint64_t                created_at_ns_      = 0;
  EditCommitKind               kind_               = EditCommitKind::kEdit;
  nlohmann::json               edit_payload_       = nlohmann::json::object();
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

/// Root chain hash before any commits.
auto ComputeRootChainHash(const root_id_t& root_id) -> transaction_chain_hash_t;

/// Canonical little-endian fold input: H(previous_chain_hash, commit_hash).
auto TransactionChainFoldInput(const transaction_chain_hash_t& previous,
                               const commit_hash_t& commit_hash) -> std::vector<std::uint8_t>;

/// Fold one commit into the chain.
auto FoldTransactionChainHash(const transaction_chain_hash_t& previous,
                              const commit_hash_t& commit_hash) -> transaction_chain_hash_t;

/// Fold an ordered first-parent commit list starting from the root chain hash.
auto FoldFirstParentChain(const root_id_t&                  root_id,
                          const std::vector<commit_hash_t>& first_parent_commits)
    -> transaction_chain_hash_t;

}  // namespace alcedo
