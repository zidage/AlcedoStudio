//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/editor_transaction_journal.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace alcedo {
namespace {

constexpr std::size_t kFixedHeaderBytes = 4 +  // magic
    4 +  // record_length
    2 +  // format_version
    2 +  // record_type
    8 +  // sequence
    8 +  // element_id
    8 +  // version_id_low
    8 +  // version_id_high
    8 +  // session_generation
    8 +  // journal_generation
    4;   // payload_size
constexpr std::size_t kChecksumBytes = 16 + 16;

auto MerkleRoot(std::vector<Hash128> hashes) -> Hash128 {
  if (hashes.empty()) {
    return Hash128{};
  }
  while (hashes.size() > 1) {
    std::vector<Hash128> next_level;
    next_level.reserve((hashes.size() + 1) / 2);
    for (std::size_t i = 0; i < hashes.size(); i += 2) {
      if (i + 1 < hashes.size()) {
        next_level.push_back(Hash128::Blend(hashes[i], hashes[i + 1]));
      } else {
        next_level.push_back(hashes[i]);
      }
    }
    hashes = std::move(next_level);
  }
  return hashes[0];
}

void AppendU16(std::vector<std::uint8_t>* out, std::uint16_t value) {
  out->push_back(static_cast<std::uint8_t>(value & 0xFF));
  out->push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void AppendU32(std::vector<std::uint8_t>* out, std::uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

void AppendU64(std::vector<std::uint8_t>* out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out->push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFF));
  }
}

void AppendHash(std::vector<std::uint8_t>* out, const Hash128& hash) {
  AppendU64(out, hash.low64());
  AppendU64(out, hash.high64());
}

void AppendBytes(std::vector<std::uint8_t>* out, const std::uint8_t* data, std::size_t size) {
  out->insert(out->end(), data, data + size);
}

auto ReadU16(const std::uint8_t* data) -> std::uint16_t {
  return static_cast<std::uint16_t>(data[0]) | (static_cast<std::uint16_t>(data[1]) << 8);
}

auto ReadU32(const std::uint8_t* data) -> std::uint32_t {
  return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
         (static_cast<std::uint32_t>(data[2]) << 16) | (static_cast<std::uint32_t>(data[3]) << 24);
}

auto ReadU64(const std::uint8_t* data) -> std::uint64_t {
  std::uint64_t value = 0;
  for (int i = 0; i < 8; ++i) {
    value |= (static_cast<std::uint64_t>(data[i]) << (8 * i));
  }
  return value;
}

auto ReadHash(const std::uint8_t* data) -> Hash128 {
  return Hash128(ReadU64(data), ReadU64(data + 8));
}

auto JsonToBytes(const nlohmann::json& j) -> std::vector<std::uint8_t> {
  const std::string text = j.dump();
  return std::vector<std::uint8_t>(text.begin(), text.end());
}

auto BytesToJson(const std::vector<std::uint8_t>& bytes, nlohmann::json* out, std::string* error)
    -> bool {
  try {
    *out = nlohmann::json::parse(bytes.begin(), bytes.end());
    return true;
  } catch (const std::exception& e) {
    if (error) {
      *error = e.what();
    }
    return false;
  }
}

auto EncodeTransactionJson(const EditTransaction& tx) -> std::vector<std::uint8_t> {
  return JsonToBytes(tx.ToJSON());
}

auto DecodeTransactionJson(const std::vector<std::uint8_t>& bytes, EditTransaction* out,
                           std::string* error) -> bool {
  nlohmann::json j;
  if (!BytesToJson(bytes, &j, error)) {
    return false;
  }
  try {
    *out = EditTransaction(j);
    return true;
  } catch (const std::exception& e) {
    if (error) {
      *error = e.what();
    }
    return false;
  }
}

auto EncodeStringPayload(const std::string& text) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  AppendU32(&out, static_cast<std::uint32_t>(text.size()));
  AppendBytes(&out, reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
  return out;
}

auto DecodeStringPayload(const std::uint8_t* data, std::size_t size, std::size_t* offset,
                         std::string* out, std::string* error) -> bool {
  if (*offset + 4 > size) {
    if (error) {
      *error = "truncated string length";
    }
    return false;
  }
  const auto len = ReadU32(data + *offset);
  *offset += 4;
  if (*offset + len > size) {
    if (error) {
      *error = "truncated string bytes";
    }
    return false;
  }
  out->assign(reinterpret_cast<const char*>(data + *offset), len);
  *offset += len;
  return true;
}

auto FillTypedPayload(EditorJournalDecodedRecord* record, std::string* error) -> bool {
  switch (record->record_type) {
    case EditorJournalRecordType::EditAppend: {
      EditorJournalEditAppendPayload payload;
      if (!DecodeEditorJournalEditAppendPayload(record->payload_bytes, &payload, error)) {
        return false;
      }
      record->edit_append = std::move(payload);
      return true;
    }
    case EditorJournalRecordType::CursorMove: {
      EditorJournalCursorMovePayload payload;
      if (!DecodeEditorJournalCursorMovePayload(record->payload_bytes, &payload, error)) {
        return false;
      }
      record->cursor_move = std::move(payload);
      return true;
    }
    case EditorJournalRecordType::RewriteTimeline: {
      EditorJournalRewriteTimelinePayload payload;
      if (!DecodeEditorJournalRewriteTimelinePayload(record->payload_bytes, &payload, error)) {
        return false;
      }
      record->rewrite_timeline = std::move(payload);
      return true;
    }
    case EditorJournalRecordType::MaterializedHead: {
      EditorJournalMaterializedHeadPayload payload;
      if (!DecodeEditorJournalMaterializedHeadPayload(record->payload_bytes, &payload, error)) {
        return false;
      }
      record->materialized_head = std::move(payload);
      return true;
    }
    case EditorJournalRecordType::RecoveryMarker:
    case EditorJournalRecordType::CompactionCheckpoint: {
      EditorJournalMarkerPayload payload;
      if (!DecodeEditorJournalMarkerPayload(record->payload_bytes, &payload, error)) {
        return false;
      }
      record->marker = std::move(payload);
      return true;
    }
    case EditorJournalRecordType::JournalBatchCommit: {
      EditorJournalBatchCommitPayload payload;
      if (!DecodeEditorJournalBatchCommitPayload(record->payload_bytes, &payload, error)) {
        return false;
      }
      record->batch_commit = std::move(payload);
      return true;
    }
  }
  if (error) {
    *error = "unknown journal record type";
  }
  return false;
}

}  // namespace

auto ComputeEditorTimelineHash(const std::vector<EditTransaction>& transactions, std::size_t cursor)
    -> Hash128 {
  if (transactions.empty()) {
    const std::uint64_t c = static_cast<std::uint64_t>(cursor);
    return Hash128::Compute(&c, sizeof(c));
  }
  std::vector<Hash128> leaves;
  leaves.reserve(transactions.size() + 1);
  for (const auto& tx : transactions) {
    leaves.push_back(tx.GetTransactionHash());
  }
  const std::uint64_t c = static_cast<std::uint64_t>(cursor);
  leaves.push_back(Hash128::Compute(&c, sizeof(c)));
  return MerkleRoot(std::move(leaves));
}

auto ComputeEditorTransactionSpanHash(const std::vector<EditTransaction>& transactions,
                                      std::size_t begin, std::size_t end) -> Hash128 {
  if (begin >= end || begin >= transactions.size()) {
    return Hash128{};
  }
  end = std::min(end, transactions.size());
  std::vector<Hash128> leaves;
  leaves.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    leaves.push_back(transactions[i].GetTransactionHash());
  }
  return MerkleRoot(std::move(leaves));
}

auto EncodeEditorJournalEditAppendPayload(const EditorJournalEditAppendPayload& payload)
    -> std::vector<std::uint8_t> {
  return EncodeTransactionJson(payload.transaction);
}

auto EncodeEditorJournalCursorMovePayload(const EditorJournalCursorMovePayload& payload)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  AppendU64(&out, payload.from_cursor);
  AppendU64(&out, payload.to_cursor);
  return out;
}

auto EncodeEditorJournalRewriteTimelinePayload(const EditorJournalRewriteTimelinePayload& payload)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  AppendHash(&out, payload.expected_timeline_hash);
  AppendHash(&out, payload.discarded_tail_hash);
  AppendU64(&out, payload.retained_cursor);
  const auto tx_bytes = EncodeTransactionJson(payload.replacement);
  AppendU32(&out, static_cast<std::uint32_t>(tx_bytes.size()));
  AppendBytes(&out, tx_bytes.data(), tx_bytes.size());
  return out;
}

auto EncodeEditorJournalMaterializedHeadPayload(const EditorJournalMaterializedHeadPayload& payload)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  AppendHash(&out, payload.timeline_hash);
  AppendU64(&out, payload.applied_cursor);
  const auto params = JsonToBytes(payload.head_pipeline_params);
  AppendU32(&out, static_cast<std::uint32_t>(params.size()));
  AppendBytes(&out, params.data(), params.size());
  return out;
}

auto EncodeEditorJournalMarkerPayload(const EditorJournalMarkerPayload& payload)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  AppendU64(&out, payload.last_valid_sequence);
  const auto note = EncodeStringPayload(payload.note);
  AppendBytes(&out, note.data(), note.size());
  return out;
}

auto EncodeEditorJournalBatchCommitPayload(const EditorJournalBatchCommitPayload& payload)
    -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> out;
  out.reserve(8 * 4 + 16);
  AppendU64(&out, payload.previous_batch_commit_sequence);
  AppendU64(&out, payload.first_covered_sequence);
  AppendU64(&out, payload.last_covered_sequence);
  AppendU64(&out, payload.last_operation_sequence);
  AppendHash(&out, payload.record_chain_hash);
  return out;
}

auto DecodeEditorJournalEditAppendPayload(const std::vector<std::uint8_t>& bytes,
                                          EditorJournalEditAppendPayload* out, std::string* error)
    -> bool {
  if (out == nullptr) {
    return false;
  }
  return DecodeTransactionJson(bytes, &out->transaction, error);
}

auto DecodeEditorJournalCursorMovePayload(const std::vector<std::uint8_t>& bytes,
                                          EditorJournalCursorMovePayload* out, std::string* error)
    -> bool {
  if (out == nullptr || bytes.size() < 16) {
    if (error) {
      *error = "cursor-move payload too short";
    }
    return false;
  }
  out->from_cursor = ReadU64(bytes.data());
  out->to_cursor   = ReadU64(bytes.data() + 8);
  return true;
}

auto DecodeEditorJournalRewriteTimelinePayload(const std::vector<std::uint8_t>& bytes,
                                               EditorJournalRewriteTimelinePayload* out,
                                               std::string* error) -> bool {
  if (out == nullptr || bytes.size() < 16 + 16 + 8 + 4) {
    if (error) {
      *error = "rewrite-timeline payload too short";
    }
    return false;
  }
  std::size_t offset          = 0;
  out->expected_timeline_hash = ReadHash(bytes.data() + offset);
  offset += 16;
  out->discarded_tail_hash = ReadHash(bytes.data() + offset);
  offset += 16;
  out->retained_cursor = ReadU64(bytes.data() + offset);
  offset += 8;
  const auto tx_size = ReadU32(bytes.data() + offset);
  offset += 4;
  if (offset + tx_size != bytes.size()) {
    if (error) {
      *error = "rewrite-timeline replacement size mismatch";
    }
    return false;
  }
  std::vector<std::uint8_t> tx_bytes(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                     bytes.end());
  return DecodeTransactionJson(tx_bytes, &out->replacement, error);
}

auto DecodeEditorJournalMaterializedHeadPayload(const std::vector<std::uint8_t>& bytes,
                                                EditorJournalMaterializedHeadPayload* out,
                                                std::string* error) -> bool {
  if (out == nullptr || bytes.size() < 16 + 8 + 4) {
    if (error) {
      *error = "materialized-head payload too short";
    }
    return false;
  }
  std::size_t offset = 0;
  out->timeline_hash = ReadHash(bytes.data() + offset);
  offset += 16;
  out->applied_cursor = ReadU64(bytes.data() + offset);
  offset += 8;
  const auto params_size = ReadU32(bytes.data() + offset);
  offset += 4;
  if (offset + params_size != bytes.size()) {
    if (error) {
      *error = "materialized-head params size mismatch";
    }
    return false;
  }
  std::vector<std::uint8_t> params(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                   bytes.end());
  return BytesToJson(params, &out->head_pipeline_params, error);
}

auto DecodeEditorJournalMarkerPayload(const std::vector<std::uint8_t>& bytes,
                                      EditorJournalMarkerPayload* out, std::string* error) -> bool {
  if (out == nullptr || bytes.size() < 8) {
    if (error) {
      *error = "marker payload too short";
    }
    return false;
  }
  std::size_t offset      = 0;
  out->last_valid_sequence = ReadU64(bytes.data() + offset);
  offset += 8;
  return DecodeStringPayload(bytes.data(), bytes.size(), &offset, &out->note, error);
}

auto DecodeEditorJournalBatchCommitPayload(const std::vector<std::uint8_t>& bytes,
                                           EditorJournalBatchCommitPayload* out, std::string* error)
    -> bool {
  constexpr std::size_t kPayloadBytes = 8 * 4 + 16;
  if (out == nullptr || bytes.size() != kPayloadBytes) {
    if (error) {
      *error = "journal-batch-commit payload size mismatch";
    }
    return false;
  }
  std::size_t offset = 0;
  out->previous_batch_commit_sequence = ReadU64(bytes.data() + offset);
  offset += 8;
  out->first_covered_sequence = ReadU64(bytes.data() + offset);
  offset += 8;
  out->last_covered_sequence = ReadU64(bytes.data() + offset);
  offset += 8;
  out->last_operation_sequence = ReadU64(bytes.data() + offset);
  offset += 8;
  out->record_chain_hash = ReadHash(bytes.data() + offset);
  return true;
}

auto EncodeEditorJournalRecord(EditorJournalRecordType type, std::uint64_t sequence,
                               const EditorJournalIdentity&     identity,
                               const std::vector<std::uint8_t>& payload_bytes)
    -> std::vector<std::uint8_t> {
  const std::uint32_t record_length =
      static_cast<std::uint32_t>(kFixedHeaderBytes + payload_bytes.size() + kChecksumBytes);

  std::vector<std::uint8_t> out;
  out.reserve(record_length);
  AppendU32(&out, kEditorJournalRecordMagic);
  AppendU32(&out, record_length);
  AppendU16(&out, kEditorJournalFormatVersion);
  AppendU16(&out, static_cast<std::uint16_t>(type));
  AppendU64(&out, sequence);
  AppendU64(&out, static_cast<std::uint64_t>(identity.element_id));
  AppendU64(&out, identity.version_id.low64());
  AppendU64(&out, identity.version_id.high64());
  AppendU64(&out, identity.session_generation);
  AppendU64(&out, identity.journal_generation);
  AppendU32(&out, static_cast<std::uint32_t>(payload_bytes.size()));
  AppendBytes(&out, payload_bytes.data(), payload_bytes.size());

  const Hash128 payload_checksum = Hash128::Compute(payload_bytes.data(), payload_bytes.size());
  AppendHash(&out, payload_checksum);

  const Hash128 record_checksum = Hash128::Compute(out.data(), out.size());
  AppendHash(&out, record_checksum);
  return out;
}

auto DecodeEditorJournalRecordChain(const std::uint8_t* data, std::size_t size)
    -> EditorJournalDecodeRecordChainResult {
  EditorJournalDecodeRecordChainResult result;
  std::size_t                     offset = 0;
  std::uint64_t                   expected_sequence = 1;

  while (offset < size) {
    const std::size_t remaining = size - offset;
    if (remaining < 8) {
      result.stopped_on_incomplete_tail = true;
      result.message                    = "incomplete record header";
      break;
    }

    const auto magic         = ReadU32(data + offset);
    const auto record_length = ReadU32(data + offset + 4);
    if (magic != kEditorJournalRecordMagic) {
      result.stopped_on_corrupt_record = true;
      result.message                   = "invalid journal record magic";
      break;
    }
    if (record_length < kFixedHeaderBytes + kChecksumBytes) {
      result.stopped_on_corrupt_record = true;
      result.message                   = "record length below minimum";
      break;
    }
    if (remaining < record_length) {
      result.stopped_on_incomplete_tail = true;
      result.message                    = "incomplete journal record tail";
      break;
    }

    const std::uint8_t* rec             = data + offset;
    const auto          format_version  = ReadU16(rec + 8);
    const auto          record_type_raw = ReadU16(rec + 10);
    if (format_version != kEditorJournalFormatVersion) {
      result.stopped_on_corrupt_record = true;
      result.message                   = "unsupported journal format version";
      break;
    }

    const auto payload_size = ReadU32(rec + (kFixedHeaderBytes - 4));
    if (kFixedHeaderBytes + payload_size + kChecksumBytes != record_length) {
      result.stopped_on_corrupt_record = true;
      result.message                   = "record length does not match payload size";
      break;
    }

    const std::uint8_t* payload_ptr = rec + kFixedHeaderBytes;
    const Hash128       payload_checksum = ReadHash(rec + kFixedHeaderBytes + payload_size);
    const Hash128       record_checksum  = ReadHash(rec + kFixedHeaderBytes + payload_size + 16);

    const Hash128       expected_payload = Hash128::Compute(payload_ptr, payload_size);
    if (expected_payload != payload_checksum) {
      result.stopped_on_corrupt_record = true;
      result.message                   = "payload checksum mismatch";
      break;
    }
    const Hash128 expected_record = Hash128::Compute(rec, kFixedHeaderBytes + payload_size + 16);
    if (expected_record != record_checksum) {
      result.stopped_on_corrupt_record = true;
      result.message                   = "record checksum mismatch";
      break;
    }

    EditorJournalDecodedRecord decoded;
    decoded.record_length  = record_length;
    decoded.format_version = format_version;
    decoded.record_type    = static_cast<EditorJournalRecordType>(record_type_raw);
    decoded.sequence       = ReadU64(rec + 12);
    if (decoded.sequence != expected_sequence) {
      result.stopped_on_corrupt_record = true;
      result.message                   = "journal record sequence gap";
      break;
    }
    decoded.identity.element_id         = static_cast<sl_element_id_t>(ReadU64(rec + 20));
    decoded.identity.version_id         = Hash128(ReadU64(rec + 28), ReadU64(rec + 36));
    decoded.identity.session_generation = ReadU64(rec + 44);
    decoded.identity.journal_generation = ReadU64(rec + 52);
    decoded.payload_checksum            = payload_checksum;
    decoded.record_checksum             = record_checksum;
    decoded.payload_bytes.assign(payload_ptr, payload_ptr + payload_size);

    std::string typed_error;
    if (!FillTypedPayload(&decoded, &typed_error)) {
      result.stopped_on_corrupt_record = true;
      result.message = typed_error.empty() ? "typed payload decode failed" : typed_error;
      break;
    }

    result.records.push_back(std::move(decoded));
    offset += record_length;
    result.valid_chain_byte_count = offset;
    ++expected_sequence;
  }

  return result;
}

auto ComputeEditorJournalRecordChainHash(const std::vector<EditorJournalDecodedRecord>& records,
                                         std::uint64_t last_sequence) -> Hash128 {
  Hash128 chain{};
  for (const auto& record : records) {
    if (record.sequence > last_sequence ||
        record.record_type == EditorJournalRecordType::JournalBatchCommit) {
      continue;
    }
    const Hash128 sequence_hash = Hash128::Compute(&record.sequence, sizeof(record.sequence));
    chain = Hash128::Blend(chain, Hash128::Blend(sequence_hash, record.record_checksum));
  }
  return chain;
}

auto IsEditorJournalEditHistoryRecord(EditorJournalRecordType type) -> bool {
  return type == EditorJournalRecordType::EditAppend ||
         type == EditorJournalRecordType::CursorMove ||
         type == EditorJournalRecordType::RewriteTimeline;
}

void EditorTransactionJournal::Clear() {
  bytes_.clear();
  next_sequence_ = 1;
}

void EditorTransactionJournal::AppendRaw(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr || size == 0) {
    return;
  }
  bytes_.insert(bytes_.end(), data, data + size);
}

auto EditorTransactionJournal::LoadBytes(const std::vector<std::uint8_t>& data, std::string* error,
                                         bool* truncated_corrupt_tail) -> bool {
  Clear();
  AppendRaw(data.data(), data.size());
  const auto decoded = DecodeRecordChain();
  if (decoded.stopped_on_corrupt_record || decoded.stopped_on_incomplete_tail) {
    if (truncated_corrupt_tail) {
      *truncated_corrupt_tail = true;
    }
    if (decoded.valid_chain_byte_count == 0) {
      // No valid prefix can be recovered. Clear the in-memory decoder state and
      // let the file-backed writer surface the failure without altering disk.
      Clear();
      if (error) {
        *error = decoded.message.empty() ? "corrupt journal with no valid prefix" : decoded.message;
      }
      return false;
    }
    // A valid prefix exists. Truncate the damaged tail so recovery can use the
    // last complete batch commit. The caller preserves the original bytes in a
    // diagnostic bundle and truncates the on-disk file to match the prefix.
    bytes_.resize(decoded.valid_chain_byte_count);
    if (error) {
      *error = decoded.message.empty() ? "journal ended with a partial or damaged record"
                                       : decoded.message;
    }
  }
  if (decoded.records.empty()) {
    next_sequence_ = 1;
  } else {
    next_sequence_ = decoded.records.back().sequence + 1;
  }
  return true;
}

auto EditorTransactionJournal::Truncate(std::size_t byte_count, std::string* error) -> bool {
  if (byte_count > bytes_.size()) {
    if (error) {
      *error = "cannot extend journal with truncate";
    }
    return false;
  }
  const auto decoded = DecodeEditorJournalRecordChain(bytes_.data(), byte_count);
  if (decoded.stopped_on_corrupt_record || decoded.valid_chain_byte_count != byte_count) {
    if (error) {
      *error = decoded.message.empty() ? "journal truncate would leave a corrupt prefix"
                                       : decoded.message;
    }
    return false;
  }
  bytes_.resize(byte_count);
  next_sequence_ = decoded.records.empty() ? 1 : decoded.records.back().sequence + 1;
  return true;
}

auto EditorTransactionJournal::AppendFramed(EditorJournalRecordType type,
                                            const EditorJournalIdentity& identity,
                                            const std::vector<std::uint8_t>& payload)
    -> std::uint64_t {
  const auto sequence = next_sequence_++;
  const auto framed   = EncodeEditorJournalRecord(type, sequence, identity, payload);
  AppendRaw(framed.data(), framed.size());
  return sequence;
}

auto EditorTransactionJournal::AppendEdit(const EditorJournalIdentity& identity,
                                          const EditTransaction& transaction) -> std::uint64_t {
  EditorJournalEditAppendPayload payload;
  payload.transaction = transaction;
  if (!payload.transaction.HasTransactionHash()) {
    payload.transaction.GenerateTransactionHash();
  }
  return AppendFramed(EditorJournalRecordType::EditAppend, identity,
                      EncodeEditorJournalEditAppendPayload(payload));
}

auto EditorTransactionJournal::AppendCursorMove(const EditorJournalIdentity& identity,
                                                std::uint64_t from_cursor, std::uint64_t to_cursor)
    -> std::uint64_t {
  EditorJournalCursorMovePayload payload;
  payload.from_cursor = from_cursor;
  payload.to_cursor   = to_cursor;
  return AppendFramed(EditorJournalRecordType::CursorMove, identity,
                      EncodeEditorJournalCursorMovePayload(payload));
}

auto EditorTransactionJournal::AppendRewriteTimeline(const EditorJournalIdentity& identity,
                                                     const Hash128&         expected_timeline_hash,
                                                     const Hash128&         discarded_tail_hash,
                                                     std::uint64_t          retained_cursor,
                                                     const EditTransaction& replacement)
    -> std::uint64_t {
  EditorJournalRewriteTimelinePayload payload;
  payload.expected_timeline_hash = expected_timeline_hash;
  payload.discarded_tail_hash    = discarded_tail_hash;
  payload.retained_cursor        = retained_cursor;
  payload.replacement            = replacement;
  if (!payload.replacement.HasTransactionHash()) {
    payload.replacement.GenerateTransactionHash();
  }
  return AppendFramed(EditorJournalRecordType::RewriteTimeline, identity,
                      EncodeEditorJournalRewriteTimelinePayload(payload));
}

auto EditorTransactionJournal::AppendMaterializedHead(const EditorJournalIdentity& identity,
                                                      const Hash128&               timeline_hash,
                                                      std::uint64_t                applied_cursor,
                                                      const nlohmann::json& head_pipeline_params)
    -> std::uint64_t {
  EditorJournalMaterializedHeadPayload payload;
  payload.timeline_hash         = timeline_hash;
  payload.applied_cursor        = applied_cursor;
  payload.head_pipeline_params  = head_pipeline_params;
  return AppendFramed(EditorJournalRecordType::MaterializedHead, identity,
                      EncodeEditorJournalMaterializedHeadPayload(payload));
}

auto EditorTransactionJournal::AppendRecoveryMarker(const EditorJournalIdentity& identity,
                                                    std::uint64_t last_valid_sequence,
                                                    std::string  note) -> std::uint64_t {
  EditorJournalMarkerPayload payload;
  payload.last_valid_sequence = last_valid_sequence;
  payload.note                = std::move(note);
  return AppendFramed(EditorJournalRecordType::RecoveryMarker, identity,
                      EncodeEditorJournalMarkerPayload(payload));
}

auto EditorTransactionJournal::AppendCompactionCheckpoint(const EditorJournalIdentity& identity,
                                                          std::uint64_t last_valid_sequence,
                                                          std::string  note) -> std::uint64_t {
  EditorJournalMarkerPayload payload;
  payload.last_valid_sequence = last_valid_sequence;
  payload.note                = std::move(note);
  return AppendFramed(EditorJournalRecordType::CompactionCheckpoint, identity,
                      EncodeEditorJournalMarkerPayload(payload));
}

auto EditorTransactionJournal::AppendJournalBatchCommit(
    const EditorJournalIdentity& identity, const EditorJournalBatchCommitPayload& payload)
    -> std::uint64_t {
  return AppendFramed(EditorJournalRecordType::JournalBatchCommit, identity,
                      EncodeEditorJournalBatchCommitPayload(payload));
}

auto EditorTransactionJournal::DecodeRecordChain() const -> EditorJournalDecodeRecordChainResult {
  return DecodeEditorJournalRecordChain(bytes_.data(), bytes_.size());
}

JournalTimelineSimulator::JournalTimelineSimulator(EditorJournalIdentity identity)
    : identity_(identity) {}

void JournalTimelineSimulator::Reset(EditorJournalIdentity identity) {
  identity_               = identity;
  transactions_.clear();
  cursor_                 = 0;
  tx_id_high_water_       = 0;
  last_sequence_          = 0;
  materialized_sequence_  = 0;
  head_pipeline_params_   = std::nullopt;
}

auto JournalTimelineSimulator::AllocateTransactionId() -> tx_id_t {
  ++tx_id_high_water_;
  return tx_id_high_water_;
}

void JournalTimelineSimulator::NoteTransactionId(tx_id_t id) {
  if (id > tx_id_high_water_) {
    tx_id_high_water_ = id;
  }
}

auto JournalTimelineSimulator::ApplyDecodedRecord(const EditorJournalDecodedRecord& record)
    -> EditorJournalApplyResult {
  EditorJournalApplyResult result;

  if (record.identity.element_id != identity_.element_id && identity_.element_id != 0) {
    result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
    result.message = "record element identity mismatch";
    return result;
  }
  if (identity_.version_id != Hash128{} && record.identity.version_id != identity_.version_id) {
    result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
    result.message = "record version identity mismatch";
    return result;
  }
  if (record.sequence == 0 || record.sequence <= last_sequence_) {
    result.status  = EditorJournalApplyStatus::RejectedOutOfOrder;
    result.message = "non-monotonic journal sequence";
    return result;
  }

  // Idempotent materialization: records at or before the durable head are ignored.
  if (materialized_sequence_ != 0 && record.sequence <= materialized_sequence_ &&
      record.record_type != EditorJournalRecordType::MaterializedHead &&
      record.record_type != EditorJournalRecordType::RecoveryMarker &&
      record.record_type != EditorJournalRecordType::CompactionCheckpoint) {
    result.status  = EditorJournalApplyStatus::IgnoredAlreadyMaterialized;
    result.message = "record already covered by materialized head";
    last_sequence_ = record.sequence;
    return result;
  }

  switch (record.record_type) {
    case EditorJournalRecordType::EditAppend: {
      if (!record.edit_append.has_value()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "missing edit-append payload";
        return result;
      }
      if (cursor_ < transactions_.size()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message =
            "edit-append while redo tail exists; use RewriteTimeline for atomic rewrite";
        return result;
      }
      EditTransaction tx = record.edit_append->transaction;
      if (!tx.HasTransactionHash()) {
        tx.GenerateTransactionHash();
      }
      NoteTransactionId(tx.GetTransactionID());
      transactions_.push_back(std::move(tx));
      cursor_ = transactions_.size();
      break;
    }
    case EditorJournalRecordType::CursorMove: {
      if (!record.cursor_move.has_value()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "missing cursor-move payload";
        return result;
      }
      if (record.cursor_move->from_cursor != cursor_) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "cursor-move from_cursor mismatch";
        return result;
      }
      const auto target = static_cast<std::size_t>(record.cursor_move->to_cursor);
      if (target > transactions_.size()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "cursor-move target beyond timeline";
        return result;
      }
      cursor_ = target;
      break;
    }
    case EditorJournalRecordType::RewriteTimeline: {
      if (!record.rewrite_timeline.has_value()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "missing rewrite-timeline payload";
        return result;
      }
      const auto& payload = *record.rewrite_timeline;
      if (payload.retained_cursor != cursor_) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "rewrite retained_cursor mismatch";
        return result;
      }
      if (payload.retained_cursor > transactions_.size()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "rewrite retained_cursor beyond timeline";
        return result;
      }

      const Hash128 current_hash = TimelineHash();
      if (current_hash != payload.expected_timeline_hash) {
        result.status  = EditorJournalApplyStatus::RejectedHashMismatch;
        result.message = "rewrite expected timeline hash mismatch";
        return result;
      }

      const Hash128 tail_hash = ComputeEditorTransactionSpanHash(
          transactions_, static_cast<std::size_t>(payload.retained_cursor), transactions_.size());
      if (tail_hash != payload.discarded_tail_hash) {
        result.status  = EditorJournalApplyStatus::RejectedHashMismatch;
        result.message = "rewrite discarded-tail hash mismatch";
        return result;
      }

      // Capture discarded ids into high-water before truncating so they are never reused.
      for (std::size_t i = static_cast<std::size_t>(payload.retained_cursor);
           i < transactions_.size(); ++i) {
        NoteTransactionId(transactions_[i].GetTransactionID());
      }

      transactions_.erase(
          transactions_.begin() + static_cast<std::ptrdiff_t>(payload.retained_cursor),
                          transactions_.end());

      EditTransaction replacement = payload.replacement;
      if (!replacement.HasTransactionHash()) {
        replacement.GenerateTransactionHash();
      }
      NoteTransactionId(replacement.GetTransactionID());
      transactions_.push_back(std::move(replacement));
      cursor_ = transactions_.size();
      break;
    }
    case EditorJournalRecordType::MaterializedHead: {
      if (!record.materialized_head.has_value()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "missing materialized-head payload";
        return result;
      }
      const auto& payload = *record.materialized_head;
      if (payload.applied_cursor != cursor_) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "materialized-head cursor mismatch";
        return result;
      }
      if (payload.timeline_hash != TimelineHash()) {
        result.status  = EditorJournalApplyStatus::RejectedHashMismatch;
        result.message = "materialized-head timeline hash mismatch";
        return result;
      }
      head_pipeline_params_  = payload.head_pipeline_params;
      materialized_sequence_ = record.sequence;
      break;
    }
    case EditorJournalRecordType::RecoveryMarker:
    case EditorJournalRecordType::CompactionCheckpoint: {
      if (!record.marker.has_value()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "missing marker payload";
        return result;
      }
      break;
    }
    case EditorJournalRecordType::JournalBatchCommit: {
      if (!record.batch_commit.has_value()) {
        result.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
        result.message = "missing journal-batch-commit payload";
        return result;
      }
      // The replay driver validates the covered range and cumulative checksum
      // because that requires the surrounding record chain. Applying the
      // control record itself only advances the observed sequence.
      break;
    }
  }

  if (identity_.element_id == 0) {
    identity_ = record.identity;
  }
  last_sequence_ = record.sequence;
  result.status  = EditorJournalApplyStatus::Applied;
  return result;
}

namespace {

auto ReplayDecodedRecords(JournalTimelineSimulator* simulator,
                          const EditorJournalDecodeRecordChainResult& decoded,
                          bool require_batch_commit) -> EditorJournalApplyResult {
  EditorJournalApplyResult last;
  last.status = EditorJournalApplyStatus::Applied;

  std::vector<std::uint64_t> valid_commit_sequences;
  std::uint64_t              previous_valid_commit = 0;
  for (const auto& record : decoded.records) {
    if (record.record_type != EditorJournalRecordType::JournalBatchCommit) {
      continue;
    }
    if (!record.batch_commit.has_value()) {
      continue;
    }
    const auto& payload = *record.batch_commit;
    const auto  expected_first = previous_valid_commit + 1;
    if (payload.previous_batch_commit_sequence != previous_valid_commit ||
        payload.first_covered_sequence != expected_first ||
        payload.last_covered_sequence != record.sequence - 1 ||
        payload.first_covered_sequence > payload.last_covered_sequence ||
        payload.last_operation_sequence > payload.last_covered_sequence ||
        ComputeEditorJournalRecordChainHash(decoded.records, payload.last_covered_sequence) !=
            payload.record_chain_hash) {
      continue;
    }

    std::uint64_t last_operation_sequence = 0;
    for (const auto& covered : decoded.records) {
      if (covered.sequence < payload.first_covered_sequence ||
          covered.sequence > payload.last_covered_sequence ||
          !IsEditorJournalEditHistoryRecord(covered.record_type)) {
        continue;
      }
      last_operation_sequence = covered.sequence;
    }
    if (last_operation_sequence != payload.last_operation_sequence) {
      continue;
    }
    previous_valid_commit = record.sequence;
    valid_commit_sequences.push_back(record.sequence);
  }

  if (require_batch_commit && valid_commit_sequences.empty()) {
    last.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
    last.message = "journal has no valid batch commit";
    return last;
  }

  const bool has_any_batch_commit =
      std::any_of(decoded.records.begin(), decoded.records.end(), [](const auto& record) {
        return record.record_type == EditorJournalRecordType::JournalBatchCommit;
      });
  if (has_any_batch_commit && valid_commit_sequences.empty()) {
    last.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
    last.message = "journal has no valid batch commit";
    return last;
  }

  const std::uint64_t last_committed_sequence =
      valid_commit_sequences.empty() ? 0 : valid_commit_sequences.back() - 1;

  for (const auto& record : decoded.records) {
    if (record.record_type == EditorJournalRecordType::JournalBatchCommit) {
      if (std::find(valid_commit_sequences.begin(), valid_commit_sequences.end(),
                    record.sequence) != valid_commit_sequences.end()) {
        last = simulator->ApplyDecodedRecord(record);
        if (last.status != EditorJournalApplyStatus::Applied &&
            last.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
          return last;
        }
      }
      continue;
    }
    if (!valid_commit_sequences.empty() && record.sequence > last_committed_sequence) {
      continue;
    }
    last = simulator->ApplyDecodedRecord(record);
    if (last.status != EditorJournalApplyStatus::Applied &&
        last.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
      return last;
    }
  }

  if (decoded.stopped_on_corrupt_record && valid_commit_sequences.empty()) {
    last.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
    last.message = decoded.message.empty() ? "corrupt journal tail" : decoded.message;
  }
  return last;
}

}  // namespace

auto JournalTimelineSimulator::ReplayRecordChain(const EditorTransactionJournal& journal)
    -> EditorJournalApplyResult {
  Reset(identity_);
  return ReplayDecodedRecords(this, journal.DecodeRecordChain(), false);
}

auto JournalTimelineSimulator::ReplayCommittedRecordChain(const EditorTransactionJournal& journal)
    -> EditorJournalApplyResult {
  Reset(identity_);
  return ReplayDecodedRecords(this, journal.DecodeRecordChain(), true);
}

void JournalTimelineSimulator::SeedMaterializedState(
    EditorJournalIdentity identity, std::vector<EditTransaction> transactions, std::size_t cursor,
    std::uint64_t materialized_operation_sequence,
    std::optional<nlohmann::json> head_pipeline_params) {
  identity_              = identity;
  transactions_          = std::move(transactions);
  cursor_                = cursor;
  if (cursor_ > transactions_.size()) {
    cursor_ = transactions_.size();
  }
  tx_id_high_water_      = 0;
  for (const auto& tx : transactions_) {
    NoteTransactionId(tx.GetTransactionID());
  }
  // last_sequence tracks the highest observed record sequence. Seeding from the
  // materialized edit-history operation sequence lets recovery apply only later
  // journal-committed operations without replaying the already-materialized prefix.
  last_sequence_         = materialized_operation_sequence;
  materialized_sequence_ = materialized_operation_sequence;
  head_pipeline_params_  = std::move(head_pipeline_params);
}

auto JournalTimelineSimulator::ReplayCommittedThroughOperationSequence(
    const EditorTransactionJournal& journal, std::uint64_t max_operation_sequence)
    -> EditorJournalApplyResult {
  Reset(identity_);
  const auto decoded = journal.DecodeRecordChain();
  EditorJournalApplyResult last;
  last.status = EditorJournalApplyStatus::Applied;

  std::vector<std::uint64_t> valid_commit_sequences;
  std::uint64_t              previous_valid_commit = 0;
  for (const auto& record : decoded.records) {
    if (record.record_type != EditorJournalRecordType::JournalBatchCommit ||
        !record.batch_commit.has_value()) {
      continue;
    }
    const auto& payload        = *record.batch_commit;
    const auto  expected_first = previous_valid_commit + 1;
    if (payload.previous_batch_commit_sequence != previous_valid_commit ||
        payload.first_covered_sequence != expected_first ||
        payload.last_covered_sequence != record.sequence - 1 ||
        payload.first_covered_sequence > payload.last_covered_sequence ||
        payload.last_operation_sequence > payload.last_covered_sequence ||
        ComputeEditorJournalRecordChainHash(decoded.records, payload.last_covered_sequence) !=
            payload.record_chain_hash) {
      continue;
    }
    if (max_operation_sequence != 0 && payload.last_operation_sequence > max_operation_sequence) {
      // An unflushed or not-yet-materializable batch: stop accepting later commits.
      break;
    }
    std::uint64_t last_operation_sequence = 0;
    for (const auto& covered : decoded.records) {
      if (covered.sequence < payload.first_covered_sequence ||
          covered.sequence > payload.last_covered_sequence ||
          !IsEditorJournalEditHistoryRecord(covered.record_type)) {
        continue;
      }
      last_operation_sequence = covered.sequence;
    }
    if (last_operation_sequence != payload.last_operation_sequence) {
      continue;
    }
    previous_valid_commit = record.sequence;
    valid_commit_sequences.push_back(record.sequence);
  }

  const std::uint64_t last_committed_sequence =
      valid_commit_sequences.empty() ? 0 : valid_commit_sequences.back() - 1;

  for (const auto& record : decoded.records) {
    if (!valid_commit_sequences.empty() && record.sequence > last_committed_sequence) {
      continue;
    }
    if (record.record_type == EditorJournalRecordType::JournalBatchCommit) {
      if (std::find(valid_commit_sequences.begin(), valid_commit_sequences.end(),
                    record.sequence) != valid_commit_sequences.end()) {
        last = ApplyDecodedRecord(record);
        if (last.status != EditorJournalApplyStatus::Applied &&
            last.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
          return last;
        }
      }
      continue;
    }
    last = ApplyDecodedRecord(record);
    if (last.status != EditorJournalApplyStatus::Applied &&
        last.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
      return last;
    }
  }
  return last;
}

auto JournalTimelineSimulator::ReplayCommittedAfterMaterialized(
    const EditorTransactionJournal& journal) -> EditorJournalApplyResult {
  const auto decoded = journal.DecodeRecordChain();
  EditorJournalApplyResult last;
  last.status = EditorJournalApplyStatus::Applied;

  std::vector<std::uint64_t> valid_commit_sequences;
  std::uint64_t              previous_valid_commit = 0;
  for (const auto& record : decoded.records) {
    if (record.record_type != EditorJournalRecordType::JournalBatchCommit ||
        !record.batch_commit.has_value()) {
      continue;
    }
    const auto& payload        = *record.batch_commit;
    const auto  expected_first = previous_valid_commit + 1;
    if (payload.previous_batch_commit_sequence != previous_valid_commit ||
        payload.first_covered_sequence != expected_first ||
        payload.last_covered_sequence != record.sequence - 1 ||
        payload.first_covered_sequence > payload.last_covered_sequence ||
        payload.last_operation_sequence > payload.last_covered_sequence ||
        ComputeEditorJournalRecordChainHash(decoded.records, payload.last_covered_sequence) !=
            payload.record_chain_hash) {
      continue;
    }
    std::uint64_t last_operation_sequence = 0;
    for (const auto& covered : decoded.records) {
      if (covered.sequence < payload.first_covered_sequence ||
          covered.sequence > payload.last_covered_sequence ||
          !IsEditorJournalEditHistoryRecord(covered.record_type)) {
        continue;
      }
      last_operation_sequence = covered.sequence;
    }
    if (last_operation_sequence != payload.last_operation_sequence) {
      continue;
    }
    previous_valid_commit = record.sequence;
    valid_commit_sequences.push_back(record.sequence);
  }

  if (valid_commit_sequences.empty() &&
      std::any_of(decoded.records.begin(), decoded.records.end(), [](const auto& record) {
        return record.record_type == EditorJournalRecordType::JournalBatchCommit;
      })) {
    last.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
    last.message = "journal has no valid batch commit";
    return last;
  }

  const std::uint64_t last_committed_sequence =
      valid_commit_sequences.empty() ? 0 : valid_commit_sequences.back() - 1;

  for (const auto& record : decoded.records) {
    if (!valid_commit_sequences.empty() && record.sequence > last_committed_sequence) {
      continue;
    }
    if (record.record_type == EditorJournalRecordType::JournalBatchCommit) {
      // Commit records are validation-only during recovery REDO.
      if (record.sequence > last_sequence_) {
        last_sequence_ = record.sequence;
      }
      continue;
    }
    if (IsEditorJournalEditHistoryRecord(record.record_type)) {
      if (record.sequence <= materialized_sequence_) {
        if (record.sequence > last_sequence_) {
          last_sequence_ = record.sequence;
        }
        continue;
      }
      last = ApplyDecodedRecord(record);
      if (last.status != EditorJournalApplyStatus::Applied &&
          last.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
        return last;
      }
      continue;
    }
    // MaterializedHead / markers after the materialization point are optional
    // diagnostics; apply when they post-date the seed so hashes stay consistent.
    if (record.sequence <= materialized_sequence_) {
      if (record.sequence > last_sequence_) {
        last_sequence_ = record.sequence;
      }
      continue;
    }
    last = ApplyDecodedRecord(record);
    if (last.status != EditorJournalApplyStatus::Applied &&
        last.status != EditorJournalApplyStatus::IgnoredAlreadyMaterialized) {
      return last;
    }
  }

  if (decoded.stopped_on_corrupt_record && valid_commit_sequences.empty()) {
    last.status  = EditorJournalApplyStatus::RejectedInvalidPayload;
    last.message = decoded.message.empty() ? "corrupt journal tail" : decoded.message;
  }
  return last;
}

WorkingVersionJournalRecorder::WorkingVersionJournalRecorder(EditorTransactionJournal* journal,
                                                             EditorJournalIdentity     identity)
    : journal_(journal), identity_(identity) {}

void WorkingVersionJournalRecorder::RecordAfterAppend(
    const std::vector<EditTransaction>& before_transactions, std::size_t before_cursor,
    const EditTransaction& appended, const std::vector<EditTransaction>& after_transactions,
    std::size_t after_cursor) {
  if (journal_ == nullptr) {
    return;
  }
  (void)after_transactions;
  (void)after_cursor;

  if (before_cursor < before_transactions.size()) {
    const Hash128 expected  = ComputeEditorTimelineHash(before_transactions, before_cursor);
    const Hash128 discarded = ComputeEditorTransactionSpanHash(before_transactions, before_cursor,
                                                               before_transactions.size());
    journal_->AppendRewriteTimeline(identity_, expected, discarded, before_cursor, appended);
    return;
  }
  journal_->AppendEdit(identity_, appended);
}

void WorkingVersionJournalRecorder::RecordCursorMove(std::size_t from_cursor,
                                                     std::size_t to_cursor) {
  if (journal_ == nullptr) {
    return;
  }
  journal_->AppendCursorMove(identity_, from_cursor, to_cursor);
}

void WorkingVersionJournalRecorder::RecordMaterializedHead(
    const std::vector<EditTransaction>& transactions, std::size_t cursor,
    const nlohmann::json& head_pipeline_params) {
  if (journal_ == nullptr) {
    return;
  }
  journal_->AppendMaterializedHead(identity_, ComputeEditorTimelineHash(transactions, cursor),
                                   cursor, head_pipeline_params);
}

}  // namespace alcedo
