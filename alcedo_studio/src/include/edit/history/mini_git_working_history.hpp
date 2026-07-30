//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
  /// Monotonic journal sequence assigned by MiniGitJournal::Append. Sequences
  /// start at 1; 0 means unset (not yet appended). Capture and truncation use
  /// inclusive [first, last] sequence ranges over these values.
  std::uint64_t             sequence             = 0;
  MiniGitJournalRecordKind  kind                 = MiniGitJournalRecordKind::kEditCommit;
  head_commit_hash_t        expected_source_head = std::nullopt;
  transaction_chain_hash_t  expected_source_chain_hash{};
  head_commit_hash_t        target_head = std::nullopt;
  transaction_chain_hash_t  target_chain_hash{};
  std::optional<EditCommit> edit_commit = std::nullopt;
};

/// Immutable snapshot of journal records taken under the journal mutex.
///
/// first_sequence / last_sequence form an inclusive range when records is
/// non-empty; both are nullopt when the journal has no records. Callers copy
/// this value into a save worker and must not hold live journal locks across
/// DuckDB or other long work.
struct MiniGitJournalSnapshot {
  std::vector<MiniGitJournalRecord> records;
  std::optional<std::uint64_t>      first_sequence;
  std::optional<std::uint64_t>      last_sequence;
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
///
/// One mutex serializes Append, Snapshot, Load, and range truncation so a save
/// capture and a concurrent append cannot interleave mid-snapshot, and so
/// truncating a captured prefix cannot delete a later sequence.
class MiniGitJournal final : public IMiniGitJournalAppender {
 public:
  MiniGitJournal() = default;
  explicit MiniGitJournal(std::filesystem::path path) : path_(std::move(path)) {}

  auto Append(const MiniGitJournalRecord& record, std::string* error) -> bool override;

  /// Read and checksum-validate a complete durable journal prefix. A missing
  /// journal is an empty prefix; a malformed record is corruption.
  auto Load(std::string* error) -> bool;

  /// Copy all in-memory records and their inclusive sequence range under the
  /// journal mutex. An empty journal returns empty records and nullopt range.
  [[nodiscard]] auto Snapshot() const -> MiniGitJournalSnapshot;

  /// After DuckDB materialization succeeds, discard every durable record with
  /// sequence <= last_sequence, rewriting the journal file to keep any later
  /// records. Safe when the journal is empty or last_sequence is below every
  /// stored sequence. Does not rewrite commit objects. Sequence numbers are
  /// never reused.
  auto TruncateThroughSequence(std::uint64_t last_sequence, std::string* error) -> bool;

  /// Discard the entire journal (memory + file). Used when recovery proves the
  /// whole file is already reflected by DuckDB. Prefer TruncateThroughSequence
  /// for normal save checkpoints that must preserve post-capture edits.
  auto TruncateMaterialized(std::string* error) -> bool;

  /// Replace the in-memory record list after a successful fold that skipped an
  /// already-materialized prefix (DB-commit / truncate crash window).
  void SetRecords(std::vector<MiniGitJournalRecord> records);

  /// Copy of current records (locks the journal). Prefer Snapshot when the
  /// sequence range is also required.
  [[nodiscard]] auto records() const -> std::vector<MiniGitJournalRecord>;

  [[nodiscard]] auto path() const -> const std::filesystem::path& { return path_; }

  /// Next sequence that Append will assign. Diagnostics only.
  [[nodiscard]] auto next_sequence() const -> std::uint64_t;

 private:
  auto RewriteFileUnlocked(std::string* error) -> bool;
  auto AppendUnlocked(const MiniGitJournalRecord& record, std::string* error) -> bool;

  mutable std::mutex                mutex_;
  std::filesystem::path             path_;
  std::vector<MiniGitJournalRecord> records_;
  std::uint64_t                     next_sequence_ = 1;
};

struct MiniGitEditAppendResult {
  bool                      committed = false;
  std::optional<EditCommit> commit;
  std::string               error;
};

struct MiniGitHeadMoveResult {
  bool                      moved = false;
  std::optional<EditCommit> selected_commit;
  /// Commits between the source head and the target head, in the order the
  /// caller must apply them to a pipeline/snapshot. Empty for single-step
  /// Undo/Redo (which only need `selected_commit`). Populated by
  /// MoveHeadToCommit so one user jump publishes one final snapshot.
  std::vector<EditCommit>   traversed_commits;
  /// True when the move is backward (target is an ancestor of the source head).
  /// The caller applies before-values of `traversed_commits`; false means
  /// forward (redo suffix) and the caller applies after-values.
  bool                      backward = false;
  std::string               error;
};

/// Copyable in-memory redo selection for one checked-out Version.
///
/// The working head lives on the commit graph; only the unmaterialized redo
/// suffix is owned by MiniGitWorkingHistory. Callers capture this value before
/// a fallible named-ref transition and publish it again only on success, or
/// restore it after a rejected prepare path.
struct MiniGitWorkingSelection {
  /// Furthest-future-first; back() is the immediate child of the working head.
  std::vector<commit_hash_t> redo_suffix;
};

/// Prepared local head move that has not yet touched the journal, graph, or redo.
///
/// Callers must apply every payload in `traversed_commits` (ordinary and merge)
/// to a candidate pipeline/snapshot first. Only after that validation succeeds
/// may they call PublishPreparedHeadMove, which appends one journal record and
/// performs a no-fail swap of head + redo.
struct MiniGitPreparedHeadMove {
  bool                       ready = false;
  bool                       is_noop = false;
  bool                       backward = false;
  std::string                error;
  head_commit_hash_t         target_head = std::nullopt;
  transaction_chain_hash_t   target_chain{};
  MiniGitJournalRecord       journal_record{};
  /// Apply order: newest-first when backward, oldest-first when forward.
  std::vector<EditCommit>    traversed_commits;
  MiniGitWorkingSelection    next_selection{};
  std::optional<EditCommit>  selected_commit;
};

/// Prepared settled edit that has not yet touched the journal, graph, or redo.
struct MiniGitPreparedEdit {
  bool                     ready = false;
  std::string              error;
  EditCommit               commit;
  MiniGitJournalRecord     journal_record{};
  transaction_chain_hash_t target_chain{};
};

/// In-memory working HEAD and redo selection for one checked-out Version ref.
///
/// The graph owns immutable commits and mutable Version heads. This class adds
/// only unmaterialized editor state: the redo stack and journal append order.
/// It deliberately never updates ImageEditState.materialized_*; Phase 6C-5
/// captures those values in one DuckDB materialization transaction.
///
/// Local mutations follow prepare → (caller validates pipeline) → publish so a
/// failed operator application never leaves the journal, head, or redo advanced.
class MiniGitWorkingHistory final {
 public:
  MiniGitWorkingHistory(std::shared_ptr<CommitGraph>             graph,
                        std::shared_ptr<IMiniGitJournalAppender> journal);

  [[nodiscard]] auto graph() const -> const std::shared_ptr<CommitGraph>& { return graph_; }
  [[nodiscard]] auto working_head() const -> head_commit_hash_t;
  [[nodiscard]] auto transaction_chain_hash() const -> transaction_chain_hash_t;
  [[nodiscard]] auto redo_count() const -> std::size_t { return redo_stack_.size(); }
  /// In-memory redo suffix in furthest-future-first order; back() is the
  /// immediate child of the working head. Used by the history projection to
  /// publish future rows and by MoveHeadToCommit to validate forward targets.
  [[nodiscard]] auto RedoSuffix() const -> std::vector<commit_hash_t> { return redo_stack_; }

  /// Copyable working selection (redo only). Pair with the graph head for a
  /// complete local working-state capture.
  [[nodiscard]] auto WorkingSelection() const -> MiniGitWorkingSelection;

  /// No-fail redo swap used when publishing a prepared transition or restoring
  /// a captured selection after a rejected named-ref path.
  void               PublishWorkingSelection(MiniGitWorkingSelection selection);

  /// Plan a settled edit without mutating journal/graph/redo.
  [[nodiscard]] auto PrepareAppendEdit(OrdinaryEditPayload payload) const -> MiniGitPreparedEdit;
  /// Plan a merge commit without mutating journal/graph/redo.
  [[nodiscard]] auto PrepareAppendMerge(commit_hash_t second_parent,
                                        MergeEditPayload payload) const -> MiniGitPreparedEdit;
  /// Append one journal record, insert the commit, advance the head, and clear redo.
  auto               PublishPreparedEdit(const MiniGitPreparedEdit& prepared)
      -> MiniGitEditAppendResult;

  auto               AppendEdit(OrdinaryEditPayload payload) -> MiniGitEditAppendResult;
  /// Create a merge commit whose first parent is the current working head and whose second
  /// parent is the incoming branch head. The merge commit stores the resolved field delta
  /// and folds into the first-parent chain hash. Journal append and head advance follows
  /// the same pattern as AppendEdit.
  auto               AppendMerge(commit_hash_t second_parent, MergeEditPayload payload)
      -> MiniGitEditAppendResult;

  /// Plan Undo without mutating journal/graph/redo. `traversed_commits` holds the
  /// abandoned head commit for before-value application.
  [[nodiscard]] auto PrepareUndo() const -> MiniGitPreparedHeadMove;
  /// Plan Redo without mutating journal/graph/redo.
  [[nodiscard]] auto PrepareRedo() const -> MiniGitPreparedHeadMove;
  /// Plan an explicit multi-step head move without mutating journal/graph/redo.
  [[nodiscard]] auto PrepareMoveHeadToCommit(const commit_hash_t& target) const
      -> MiniGitPreparedHeadMove;
  /// Append one head-move journal record and swap head + redo. Must only be called
  /// after the caller has applied every payload in `prepared.traversed_commits`.
  auto               PublishPreparedHeadMove(const MiniGitPreparedHeadMove& prepared)
      -> MiniGitHeadMoveResult;

  auto               Undo() -> MiniGitHeadMoveResult;
  auto               Redo() -> MiniGitHeadMoveResult;
  /// Move the working head to an explicit commit in one operation. The target
  /// must be an ancestor of the working head (backward) or a member of the
  /// in-memory redo suffix (forward); otherwise the move is rejected without
  /// changing the graph, journal, or redo suffix. Backward moves push the
  /// traversed commits onto the redo suffix in deterministic order; forward
  /// moves consume the redo suffix up to and including the target. Exactly one
  /// head-move journal record is appended. `traversed_commits` is populated in
  /// apply order with `backward` selecting before vs. after values.
  ///
  /// Prefer PrepareMoveHeadToCommit + PublishPreparedHeadMove when the caller
  /// must validate pipeline application before the durable journal append.
  auto               MoveHeadToCommit(const commit_hash_t& target, std::string* error)
      -> MiniGitHeadMoveResult;

  /// Select another named Version after the caller has completed the save
  /// checkpoint. Clears the in-memory redo suffix as part of the published
  /// selection for the new Version. Callers that still need the prior redo on
  /// a failure path must capture WorkingSelection() before this call.
  /// This does not reconstruct a pipeline; checkout owns that in Phase 6C-6.
  auto               SelectVersion(const version_ref_id_t& version_id, std::string* error) -> bool;

  /// Replay a durable journal prefix against a graph loaded from DuckDB. The
  /// final graph head is the recovered working head; callers may then rebuild
  /// their live pipeline from that head. Redo is intentionally empty after a
  /// restart because it is an in-memory convenience, not persisted state.
  static auto        Replay(CommitGraph& graph, const std::vector<MiniGitJournalRecord>& records,
                            std::string* error) -> bool;

  /// Like Replay, but skips a leading prefix already reflected by the stored
  /// materialized head (crash after DuckDB commit before journal truncation).
  /// Returns the index of the first applied record (records.size() when all
  /// were already materialized).
  static auto ReplaySkippingMaterializedPrefix(CommitGraph&                             graph,
                                               const std::vector<MiniGitJournalRecord>& records,
                                               std::size_t* applied_from_index,
                                               std::string* error) -> bool;

 private:
  std::shared_ptr<CommitGraph>             graph_;
  std::shared_ptr<IMiniGitJournalAppender> journal_;
  std::vector<commit_hash_t>               redo_stack_;
};

}  // namespace alcedo
