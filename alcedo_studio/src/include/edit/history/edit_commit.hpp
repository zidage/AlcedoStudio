//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
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
 *
 * Every factory establishes the identity of the commit: the commit hash is the hash of
 * @ref CanonicalHashInput. The payload is kept as its canonical text, the exact bytes of the hash
 * input. A commit read from storage (@ref FromStoredRow) is checked against its hash on that text
 * and parses and validates the payload on the first @ref GetPayloadJSON, so a reader of the graph
 * pays only for the payloads that it uses. Copies share the payload.
 */
class EditCommit {
 public:
  EditCommit();

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

  /**
   * @brief The payload, a validated PipelineEditBatch JSON object.
   * @throws std::runtime_error when a payload read from storage is not a valid batch. The
   *         first call parses and validates; later calls return the same value or error.
   */
  auto        GetPayloadJSON() const -> const nlohmann::json&;

  /// Canonical text of the payload: the bytes that the commit hash covers and storage keeps.
  auto        GetPayloadText() const -> const std::string& { return payload_->text; }

  /// Host-endianness-independent hash input bytes for this commit object.
  auto CanonicalHashInput() const -> std::vector<std::uint8_t>;
  auto ComputeCommitHash() const -> commit_hash_t;
  void FinalizeHash();

  /// Validate parent cardinality and payload shape without rehashing.
  void ValidateStructure() const;

  auto ToJSON() const -> nlohmann::json;
  static auto FromJSON(const nlohmann::json& j) -> EditCommit;

  /**
   * @brief Rebuild a commit from the columns of its stored row.
   *
   * Checks @p commit_hash against the hash of the stored fields and @p payload_text, without
   * parsing the payload. The payload is parsed and validated on the first GetPayloadJSON.
   * @throws std::runtime_error when the hash does not match the stored fields.
   */
  static auto FromStoredRow(commit_hash_t commit_hash, root_id_t root_id,
                            head_commit_hash_t first_parent, std::uint64_t created_at_ns,
                            std::string payload_text) -> EditCommit;

 private:
  friend struct edit_history_test::EditCommitAccess;

  /// Payload text and its parsed value; shared by the copies of one commit.
  struct Payload {
    std::string                text;
    mutable std::once_flag     parsed_once;
    mutable nlohmann::json     json;
    mutable std::exception_ptr parse_error;
  };

  static auto MakePipelineEditAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                          std::uint64_t created_at_ns, PipelineEditBatch payload)
      -> EditCommit;

  /// A payload whose @p json the caller has validated (or the empty object of a default commit).
  static auto        ValidatedPayload(nlohmann::json json) -> std::shared_ptr<const Payload>;

  commit_hash_t      commit_hash_{};
  root_id_t          root_id_{};
  head_commit_hash_t first_parent_hash_ = std::nullopt;
  std::uint64_t      created_at_ns_     = 0;
  std::shared_ptr<const Payload> payload_;
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
