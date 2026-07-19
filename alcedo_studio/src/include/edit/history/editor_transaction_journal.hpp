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

/// Frames one complete journal record. Returns empty on encode failure.
[[nodiscard]] auto EncodeEditorJournalRecord(EditorJournalRecordType type, std::uint64_t sequence,
                                             const EditorJournalIdentity&     identity,
                                             const std::vector<std::uint8_t>& payload_bytes)
    -> std::vector<std::uint8_t>;

/// Decode complete, checksum-valid records from an append-only byte stream.
/// Stops at the first incomplete or corrupt tail without consuming it.
struct EditorJournalDecodePrefixResult {
  std::vector<EditorJournalDecodedRecord> records;
  std::size_t                             valid_byte_count = 0;
  bool                                    stopped_on_incomplete_tail = false;
  bool                                    stopped_on_corrupt_record  = false;
  std::string                             message;
};

[[nodiscard]] auto DecodeEditorJournalValidPrefix(const std::uint8_t* data, std::size_t size)
    -> EditorJournalDecodePrefixResult;

/// Append-only in-memory journal log (file durability is Phase 5G/5H).
class EditorTransactionJournal final {
 public:
  [[nodiscard]] auto empty() const -> bool { return bytes_.empty(); }
  [[nodiscard]] auto size() const -> std::size_t { return bytes_.size(); }
  [[nodiscard]] auto bytes() const -> const std::vector<std::uint8_t>& { return bytes_; }
  [[nodiscard]] auto next_sequence() const -> std::uint64_t { return next_sequence_; }

  void Clear();

  /// Append raw framed bytes (used by tests to inject truncated tails).
  void AppendRaw(const std::uint8_t* data, std::size_t size);

  auto AppendEdit(const EditorJournalIdentity& identity, const EditTransaction& transaction)
      -> std::uint64_t;
  auto AppendCursorMove(const EditorJournalIdentity& identity, std::uint64_t from_cursor,
                        std::uint64_t to_cursor) -> std::uint64_t;
  auto AppendRewriteTimeline(const EditorJournalIdentity& identity,
                             const Hash128& expected_timeline_hash,
                             const Hash128& discarded_tail_hash, std::uint64_t retained_cursor,
                             const EditTransaction& replacement) -> std::uint64_t;
  auto AppendMaterializedHead(const EditorJournalIdentity& identity, const Hash128& timeline_hash,
                              std::uint64_t applied_cursor, const nlohmann::json& head_pipeline_params)
      -> std::uint64_t;
  auto AppendRecoveryMarker(const EditorJournalIdentity& identity, std::uint64_t last_valid_sequence,
                            std::string note) -> std::uint64_t;
  auto AppendCompactionCheckpoint(const EditorJournalIdentity& identity,
                                  std::uint64_t last_valid_sequence, std::string note)
      -> std::uint64_t;

  [[nodiscard]] auto DecodeValidPrefix() const -> EditorJournalDecodePrefixResult;

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
  [[nodiscard]] auto ReplayValidPrefix(const EditorTransactionJournal& journal)
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
