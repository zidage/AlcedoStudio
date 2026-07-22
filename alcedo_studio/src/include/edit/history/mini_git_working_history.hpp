//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/commit_graph.hpp"

namespace alcedo {

/// One append-only recovery-journal operation for the mini-Git history model.
///
/// Edit records carry the complete immutable commit and prove both ends of the
/// transaction-chain fold. Head moves are reflog-style operations: they only
/// select an already-known commit and never mutate a commit object.
enum class MiniGitJournalRecordKind : std::uint8_t {
  kEditCommit,
  kHeadMove,
};

struct MiniGitJournalRecord {
  MiniGitJournalRecordKind  kind                 = MiniGitJournalRecordKind::kEditCommit;
  head_commit_hash_t        expected_source_head = std::nullopt;
  transaction_chain_hash_t  expected_source_chain_hash{};
  head_commit_hash_t        target_head = std::nullopt;
  transaction_chain_hash_t  target_chain_hash{};
  std::optional<EditCommit> edit_commit = std::nullopt;
};

/// Narrow append seam for the durable journal writer. The working-history
/// state advances only after this call succeeds, which keeps a failed append
/// from being presented as a committed edit.
class IMiniGitJournalAppender {
 public:
  virtual ~IMiniGitJournalAppender()                                                  = default;

  virtual auto Append(const MiniGitJournalRecord& record, std::string* error) -> bool = 0;
};

/// Deterministic append-only journal used by the editor before Phase 6C-5
/// materialization owns file truncation. It also provides the recovery input
/// used by focused history tests.
class MiniGitJournal final : public IMiniGitJournalAppender {
 public:
  MiniGitJournal() = default;
  explicit MiniGitJournal(std::filesystem::path path) : path_(std::move(path)) {}

  auto Append(const MiniGitJournalRecord& record, std::string* error) -> bool override;

  /// Read and checksum-validate a complete durable journal prefix. A missing
  /// journal is an empty prefix; a malformed record is corruption.
  auto Load(std::string* error) -> bool;

  [[nodiscard]] auto records() const -> const std::vector<MiniGitJournalRecord>& {
    return records_;
  }

 private:
  std::filesystem::path             path_;
  std::vector<MiniGitJournalRecord> records_;
};

struct MiniGitEditAppendResult {
  bool                      committed = false;
  std::optional<EditCommit> commit;
  std::string               error;
};

struct MiniGitHeadMoveResult {
  bool                      moved = false;
  std::optional<EditCommit> selected_commit;
  std::string               error;
};

/// In-memory working HEAD and redo selection for one checked-out Version ref.
///
/// The graph owns immutable commits and mutable Version heads. This class adds
/// only unmaterialized editor state: the redo stack and journal append order.
/// It deliberately never updates ImageEditState.materialized_*; Phase 6C-5
/// captures those values in one DuckDB materialization transaction.
class MiniGitWorkingHistory final {
 public:
  MiniGitWorkingHistory(std::shared_ptr<CommitGraph>             graph,
                        std::shared_ptr<IMiniGitJournalAppender> journal);

  [[nodiscard]] auto graph() const -> const std::shared_ptr<CommitGraph>& { return graph_; }
  [[nodiscard]] auto working_head() const -> head_commit_hash_t;
  [[nodiscard]] auto transaction_chain_hash() const -> transaction_chain_hash_t;
  [[nodiscard]] auto redo_count() const -> std::size_t { return redo_stack_.size(); }

  auto               AppendEdit(OrdinaryEditPayload payload) -> MiniGitEditAppendResult;
  auto               Undo() -> MiniGitHeadMoveResult;
  auto               Redo() -> MiniGitHeadMoveResult;

  /// Select another named Version after the caller has completed the save
  /// checkpoint. This does not reconstruct a pipeline; checkout owns that in
  /// Phase 6C-6.
  auto               SelectVersion(const version_ref_id_t& version_id, std::string* error) -> bool;

  /// Replay a durable journal prefix against a graph loaded from DuckDB. The
  /// final graph head is the recovered working head; callers may then rebuild
  /// their live pipeline from that head. Redo is intentionally empty after a
  /// restart because it is an in-memory convenience, not persisted state.
  static auto        Replay(CommitGraph& graph, const std::vector<MiniGitJournalRecord>& records,
                            std::string* error) -> bool;

 private:
  auto AppendHeadMove(head_commit_hash_t target_head, transaction_chain_hash_t target_chain,
                      std::optional<EditCommit>* selected_commit) -> MiniGitHeadMoveResult;

  std::shared_ptr<CommitGraph>             graph_;
  std::shared_ptr<IMiniGitJournalAppender> journal_;
  std::vector<commit_hash_t>               redo_stack_;
};

}  // namespace alcedo
