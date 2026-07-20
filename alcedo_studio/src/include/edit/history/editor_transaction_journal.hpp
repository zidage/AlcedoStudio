//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/edit_transaction.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Phase 5F redo-only editor transaction journal.
///
/// Storage is append-only. Timeline rewrites are logical tombstones encoded as a
/// single RewriteTimeline record; physical omission of discarded redo tails is
/// deferred to verified compaction (Phase 5H).
///
/// This module owns the durable record layout, rewrite validation rules, and an
/// independent in-memory timeline simulator used by recovery/fuzz tests. It does
/// not own WorkingVersion and does not create user-visible Versions.

inline constexpr std::uint16_t kEditorJournalFormatVersion = 1;
/// Little-endian ASCII 'ALJ1'.
inline constexpr std::uint32_t kEditorJournalRecordMagic = 0x314A4C41u;

enum class EditorJournalRecordType : std::uint16_t {
  EditAppend           = 1,
  CursorMove           = 2,
  RewriteTimeline      = 3,
  MaterializedHead     = 4,
  RecoveryMarker       = 5,
  CompactionCheckpoint = 6,
  JournalBatchCommit   = 7,
};

struct EditorJournalIdentity {
  sl_element_id_t element_id         = 0;
  Hash128         version_id{};
  std::uint64_t   session_generation = 0;
  std::uint64_t   journal_generation = 0;
};

struct EditorJournalEditAppendPayload {
  EditTransaction transaction{TransactionType::_EDIT, OperatorType::UNKNOWN,
                              PipelineStageName::Stage_Count, nlohmann::json(nullptr)};
};

struct EditorJournalCursorMovePayload {
  std::uint64_t from_cursor = 0;
  std::uint64_t to_cursor   = 0;
};

struct EditorJournalRewriteTimelinePayload {
  Hash128         expected_timeline_hash{};
  Hash128         discarded_tail_hash{};
  std::uint64_t   retained_cursor = 0;
  EditTransaction replacement{TransactionType::_EDIT, OperatorType::UNKNOWN,
                              PipelineStageName::Stage_Count, nlohmann::json(nullptr)};
};

struct EditorJournalMaterializedHeadPayload {
  Hash128        timeline_hash{};
  std::uint64_t  applied_cursor = 0;
  nlohmann::json head_pipeline_params = nlohmann::json::object();
};

struct EditorJournalMarkerPayload {
  std::uint64_t last_valid_sequence = 0;
  std::string   note;
};

/// Durability boundary for one append-only journal batch. The commit record is
/// itself framed and checksummed, but its covered records are eligible for
/// recovery only after the writer successfully flushes the file.
struct EditorJournalBatchCommitPayload {
  std::uint64_t previous_batch_commit_sequence = 0;
  std::uint64_t first_covered_sequence         = 0;
  std::uint64_t last_covered_sequence          = 0;
  std::uint64_t last_operation_sequence        = 0;
  Hash128       record_chain_hash{};
};

struct EditorJournalDecodedRecord {
  std::uint32_t             record_length      = 0;
  std::uint16_t             format_version     = 0;
  EditorJournalRecordType   record_type        = EditorJournalRecordType::EditAppend;
  std::uint64_t             sequence           = 0;
  EditorJournalIdentity     identity{};
  Hash128                   payload_checksum{};
  Hash128                   record_checksum{};
  std::vector<std::uint8_t> payload_bytes;

  std::optional<EditorJournalEditAppendPayload>       edit_append;
  std::optional<EditorJournalCursorMovePayload>       cursor_move;
  std::optional<EditorJournalRewriteTimelinePayload>  rewrite_timeline;
  std::optional<EditorJournalMaterializedHeadPayload> materialized_head;
  std::optional<EditorJournalMarkerPayload>           marker;
  std::optional<EditorJournalBatchCommitPayload>      batch_commit;
};

enum class EditorJournalApplyStatus {
  Applied,
  RejectedHashMismatch,
  RejectedInvalidPayload,
  RejectedOutOfOrder,
  IgnoredAlreadyMaterialized,
};

struct EditorJournalApplyResult {
  EditorJournalApplyStatus status = EditorJournalApplyStatus::RejectedInvalidPayload;
  std::string              message;
};

/// Stable hash of the logical working timeline: ordered transaction hashes plus cursor.
[[nodiscard]] auto ComputeEditorTimelineHash(const std::vector<EditTransaction>& transactions,
                                             std::size_t                         cursor) -> Hash128;

/// Hash of transactions in [begin, end).
[[nodiscard]] auto ComputeEditorTransactionSpanHash(
    const std::vector<EditTransaction>& transactions, std::size_t begin, std::size_t end)
    -> Hash128;

[[nodiscard]] auto EncodeEditorJournalEditAppendPayload(
    const EditorJournalEditAppendPayload& payload) -> std::vector<std::uint8_t>;
[[nodiscard]] auto EncodeEditorJournalCursorMovePayload(
    const EditorJournalCursorMovePayload& payload) -> std::vector<std::uint8_t>;
[[nodiscard]] auto EncodeEditorJournalRewriteTimelinePayload(
    const EditorJournalRewriteTimelinePayload& payload) -> std::vector<std::uint8_t>;
[[nodiscard]] auto EncodeEditorJournalMaterializedHeadPayload(
    const EditorJournalMaterializedHeadPayload& payload) -> std::vector<std::uint8_t>;
[[nodiscard]] auto EncodeEditorJournalMarkerPayload(const EditorJournalMarkerPayload& payload)
    -> std::vector<std::uint8_t>;
[[nodiscard]] auto EncodeEditorJournalBatchCommitPayload(
    const EditorJournalBatchCommitPayload& payload) -> std::vector<std::uint8_t>;

[[nodiscard]] auto DecodeEditorJournalEditAppendPayload(const std::vector<std::uint8_t>& bytes,
                                                        EditorJournalEditAppendPayload* out,
                                                        std::string* error) -> bool;
[[nodiscard]] auto DecodeEditorJournalCursorMovePayload(const std::vector<std::uint8_t>& bytes,
                                                        EditorJournalCursorMovePayload* out,
                                                        std::string* error) -> bool;
[[nodiscard]] auto DecodeEditorJournalRewriteTimelinePayload(
    const std::vector<std::uint8_t>& bytes, EditorJournalRewriteTimelinePayload* out,
    std::string* error) -> bool;
[[nodiscard]] auto DecodeEditorJournalMaterializedHeadPayload(
    const std::vector<std::uint8_t>& bytes, EditorJournalMaterializedHeadPayload* out,
    std::string* error) -> bool;
[[nodiscard]] auto DecodeEditorJournalMarkerPayload(const std::vector<std::uint8_t>& bytes,
                                                    EditorJournalMarkerPayload* out,
                                                    std::string* error) -> bool;
[[nodiscard]] auto DecodeEditorJournalBatchCommitPayload(const std::vector<std::uint8_t>& bytes,
                                                         EditorJournalBatchCommitPayload* out,
    std::string* error) -> bool;

/// Frames one complete journal record. Returns empty on encode failure.
[[nodiscard]] auto EncodeEditorJournalRecord(EditorJournalRecordType type, std::uint64_t sequence,
                                             const EditorJournalIdentity&     identity,
                                             const std::vector<std::uint8_t>& payload_bytes)
    -> std::vector<std::uint8_t>;

/// Decode complete, checksum-valid records from an append-only byte stream.
/// Stops at the first incomplete or corrupt tail without consuming it.
struct EditorJournalDecodeRecordChainResult {
  std::vector<EditorJournalDecodedRecord> records;
  std::size_t                             valid_chain_byte_count = 0;
  bool                                    stopped_on_incomplete_tail = false;
  bool                                    stopped_on_corrupt_record  = false;
  std::string                             message;
};

[[nodiscard]] auto DecodeEditorJournalRecordChain(const std::uint8_t* data, std::size_t size)
    -> EditorJournalDecodeRecordChainResult;

/// Cumulative checksum of the non-commit records through `last_sequence`.
/// The sequence is mixed into the chain so reordering records cannot preserve
/// the same value.
[[nodiscard]] auto ComputeEditorJournalRecordChainHash(
    const std::vector<EditorJournalDecodedRecord>& records, std::uint64_t last_sequence) -> Hash128;

[[nodiscard]] auto IsEditorJournalEditHistoryRecord(EditorJournalRecordType type) -> bool;

/// Append-only in-memory journal log. File durability is owned by
/// EditorJournalWriter; this class remains useful as the deterministic record
/// and replay model in tests.
class EditorTransactionJournal final {
 public:
  [[nodiscard]] auto empty() const -> bool { return bytes_.empty(); }
  [[nodiscard]] auto size() const -> std::size_t { return bytes_.size(); }
  [[nodiscard]] auto bytes() const -> const std::vector<std::uint8_t>& { return bytes_; }
  [[nodiscard]] auto next_sequence() const -> std::uint64_t { return next_sequence_; }

  void Clear();

  /// Append raw framed bytes (used by tests to inject truncated tails).
  void AppendRaw(const std::uint8_t* data, std::size_t size);

  /// Replace the in-memory log with bytes read from an existing journal file.
  /// Unlike AppendRaw, this also advances the next sequence after the valid
  /// decoded prefix and is therefore intended for journal recovery/bootstrap.
  ///
  /// Recovery tolerates a partial or damaged tail: when the byte stream has a
  /// valid record prefix followed by a corrupt/incomplete record, the damaged
  /// tail is truncated to `valid_chain_byte_count` and the function returns
  /// true so the caller can recover from the last complete batch. `error`
  /// carries the decode reason and `truncated_corrupt_tail` is set so the caller
  /// can emit a diagnostic bundle and truncate the on-disk file to match. A
  /// fully corrupt stream (no valid prefix) returns false.
  auto               LoadBytes(const std::vector<std::uint8_t>& data, std::string* error = nullptr,
                               bool* truncated_corrupt_tail = nullptr) -> bool;

  /// Remove an uncommitted in-memory tail without changing the valid prefix.
  auto Truncate(std::size_t byte_count, std::string* error = nullptr) -> bool;

  auto AppendEdit(const EditorJournalIdentity& identity, const EditTransaction& transaction)
      -> std::uint64_t;
  auto AppendCursorMove(const EditorJournalIdentity& identity, std::uint64_t from_cursor,
                        std::uint64_t to_cursor) -> std::uint64_t;
  auto AppendRewriteTimeline(const EditorJournalIdentity& identity,
                             const Hash128& expected_timeline_hash,
                             const Hash128& discarded_tail_hash, std::uint64_t retained_cursor,
                             const EditTransaction& replacement) -> std::uint64_t;
  auto AppendMaterializedHead(const EditorJournalIdentity& identity, const Hash128& timeline_hash,
                              std::uint64_t         applied_cursor,
                              const nlohmann::json& head_pipeline_params) -> std::uint64_t;
  auto AppendRecoveryMarker(const EditorJournalIdentity& identity,
                            std::uint64_t last_valid_sequence, std::string note) -> std::uint64_t;
  auto AppendCompactionCheckpoint(const EditorJournalIdentity& identity,
                                  std::uint64_t last_valid_sequence, std::string note)
      -> std::uint64_t;
  auto AppendJournalBatchCommit(const EditorJournalIdentity& identity,
                                              const EditorJournalBatchCommitPayload& payload) -> std::uint64_t;

  [[nodiscard]] auto DecodeRecordChain() const -> EditorJournalDecodeRecordChainResult;

 private:
  auto AppendFramed(EditorJournalRecordType type, const EditorJournalIdentity& identity,
                    const std::vector<std::uint8_t>& payload) -> std::uint64_t;

  std::vector<std::uint8_t> bytes_;
  std::uint64_t             next_sequence_ = 1;
};

/// Independent timeline state machine for journal replay. Does not use WorkingVersion.
class JournalTimelineSimulator final {
 public:
  JournalTimelineSimulator() = default;
  explicit JournalTimelineSimulator(EditorJournalIdentity identity);

  void Reset(EditorJournalIdentity identity = {});

  [[nodiscard]] auto identity() const -> const EditorJournalIdentity& { return identity_; }
  [[nodiscard]] auto cursor() const -> std::size_t { return cursor_; }
  [[nodiscard]] auto transactions() const -> const std::vector<EditTransaction>& {
    return transactions_;
  }
  [[nodiscard]] auto tx_id_high_water() const -> tx_id_t { return tx_id_high_water_; }
  [[nodiscard]] auto last_sequence() const -> std::uint64_t { return last_sequence_; }
  [[nodiscard]] auto materialized_sequence() const -> std::uint64_t {
    return materialized_sequence_;
  }
  [[nodiscard]] auto head_pipeline_params() const -> const std::optional<nlohmann::json>& {
    return head_pipeline_params_;
  }
  [[nodiscard]] auto TimelineHash() const -> Hash128 {
    return ComputeEditorTimelineHash(transactions_, cursor_);
  }

  [[nodiscard]] auto ApplyDecodedRecord(const EditorJournalDecodedRecord& record)
      -> EditorJournalApplyResult;
  [[nodiscard]] auto ReplayRecordChain(const EditorTransactionJournal& journal)
      -> EditorJournalApplyResult;

  /// Replay only records covered by a valid JournalBatchCommit. If a journal
  /// contains no batch commits, this retains Phase 5F's in-memory format
  /// behavior for backwards-compatible unit fixtures; on-disk recovery must
  /// use a journal with a commit record.
  [[nodiscard]] auto ReplayCommittedRecordChain(const EditorTransactionJournal& journal)
      -> EditorJournalApplyResult;

  /// Seed the simulator from a DuckDB-materialized Version projection. Used by
  /// recovery to REDO only journal-committed edit-history operations after
  /// `materialized_operation_sequence`.
  void SeedMaterializedState(EditorJournalIdentity identity,
                             std::vector<EditTransaction> transactions, std::size_t cursor,
                             std::uint64_t                materialized_operation_sequence,
                             std::optional<nlohmann::json> head_pipeline_params = std::nullopt);

  /// REDO journal-committed edit-history operations strictly after the seeded
  /// materialized operation sequence. Does not Reset(); callers must Seed first
  /// (or accept an empty base). Control records at or before the materialization
  /// point are ignored; later batch commits remain validated by the commit chain.
  [[nodiscard]] auto ReplayCommittedAfterMaterialized(const EditorTransactionJournal& journal)
      -> EditorJournalApplyResult;

  /// Like ReplayCommittedRecordChain, but ignores edit-history operations whose
  /// record sequence is greater than `max_operation_sequence` (and their later
  /// batch commits). Used when an in-memory journal still holds an unflushed
  /// batch that must not be materialized.
  [[nodiscard]] auto ReplayCommittedThroughOperationSequence(
      const EditorTransactionJournal& journal, std::uint64_t max_operation_sequence)
      -> EditorJournalApplyResult;

  /// Allocate the next transaction id. Never reuses ids at or below the high-water mark,
  /// including ids that belonged to a discarded redo tail.
  [[nodiscard]] auto AllocateTransactionId() -> tx_id_t;

  void NoteTransactionId(tx_id_t id);

 private:
  EditorJournalIdentity              identity_{};
  std::vector<EditTransaction>       transactions_;
  std::size_t                        cursor_                 = 0;
  tx_id_t                            tx_id_high_water_       = 0;
  std::uint64_t                      last_sequence_          = 0;
  std::uint64_t                      materialized_sequence_  = 0;
  std::optional<nlohmann::json>      head_pipeline_params_   = std::nullopt;
};

/// Drive WorkingVersion mutations while recording redo-only journal records.
/// Used by tests and later by autosave to keep WorkingVersion and the journal aligned.
class WorkingVersionJournalRecorder final {
 public:
  WorkingVersionJournalRecorder(EditorTransactionJournal* journal, EditorJournalIdentity identity);

  void SetIdentity(EditorJournalIdentity identity) { identity_ = identity; }

  /// Append an already-built transaction. If the working cursor is behind the tail, records
  /// RewriteTimeline; otherwise records EditAppend. The transaction must not yet have an id
  /// assigned (WorkingVersion assigns it).
  void RecordAfterAppend(const std::vector<EditTransaction>& before_transactions,
                         std::size_t before_cursor, const EditTransaction& appended,
                         const std::vector<EditTransaction>& after_transactions,
                         std::size_t after_cursor);

  void RecordCursorMove(std::size_t from_cursor, std::size_t to_cursor);

  void RecordMaterializedHead(const std::vector<EditTransaction>& transactions, std::size_t cursor,
                              const nlohmann::json& head_pipeline_params);

 private:
  EditorTransactionJournal* journal_ = nullptr;
  EditorJournalIdentity     identity_{};
};

}  // namespace alcedo
