//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/edit_commit.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace alcedo {
namespace {

void AppendBytes(std::vector<std::uint8_t>& out, const void* data, std::size_t size) {
  const auto* bytes = static_cast<const std::uint8_t*>(data);
  out.insert(out.end(), bytes, bytes + size);
}

// Explicit little-endian integer encoding so commit/chain hashes are host-endian independent.
void AppendU32LE(std::vector<std::uint8_t>& out, std::uint32_t value) {
  out.push_back(static_cast<std::uint8_t>(value & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFFu));
  out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFFu));
}

void AppendU64LE(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xFFu));
  }
}

void AppendU8(std::vector<std::uint8_t>& out, std::uint8_t value) { out.push_back(value); }

void AppendHash(std::vector<std::uint8_t>& out, const Hash128& hash) {
  // Hash128::ToBytes is already little-endian per 64-bit half.
  const auto bytes = hash.ToBytes();
  out.insert(out.end(), bytes.begin(), bytes.end());
}

void AppendOptionalHash(std::vector<std::uint8_t>& out, const std::optional<Hash128>& hash) {
  if (!hash.has_value()) {
    AppendU8(out, 0);
    return;
  }
  AppendU8(out, 1);
  AppendHash(out, *hash);
}

auto OperatorTypeToInt(OperatorType type) -> int { return static_cast<int>(type); }
auto StageNameToInt(PipelineStageName stage) -> int { return static_cast<int>(stage); }

auto RequireObjectField(const nlohmann::json& j, const char* key) -> const nlohmann::json& {
  if (!j.is_object() || !j.contains(key)) {
    throw std::runtime_error(std::string("EditCommit payload missing required field '") + key +
                             "'");
  }
  return j.at(key);
}

void RequireExactObjectKeys(const nlohmann::json& j, std::initializer_list<const char*> keys,
                            const char* context) {
  if (!j.is_object()) {
    throw std::runtime_error(std::string(context) + ": expected object");
  }
  if (j.size() != keys.size()) {
    throw std::runtime_error(std::string(context) + ": unexpected field count");
  }
  for (const char* key : keys) {
    if (!j.contains(key)) {
      throw std::runtime_error(std::string(context) + ": missing required field '" + key + "'");
    }
  }
}

auto ParseOperatorType(const nlohmann::json& value) -> OperatorType {
  if (!value.is_number_integer()) {
    throw std::runtime_error("EditCommit payload: operator_type must be an integer");
  }
  const int raw = value.get<int>();
  switch (static_cast<OperatorType>(raw)) {
    case OperatorType::RAW_DECODE:
    case OperatorType::RESIZE:
    case OperatorType::EXPOSURE:
    case OperatorType::CONTRAST:
    case OperatorType::WHITE:
    case OperatorType::BLACK:
    case OperatorType::SHADOWS:
    case OperatorType::HIGHLIGHTS:
    case OperatorType::CURVE:
    case OperatorType::HLS:
    case OperatorType::SATURATION:
    case OperatorType::TINT:
    case OperatorType::VIBRANCE:
    case OperatorType::CST:
    case OperatorType::TO_WS:
    case OperatorType::TO_OUTPUT:
    case OperatorType::LMT:
    case OperatorType::ODT:
    case OperatorType::CLARITY:
    case OperatorType::SHARPEN:
    case OperatorType::COLOR_WHEEL:
    case OperatorType::ACES_TONE_MAPPING:
    case OperatorType::AUTO_EXPOSURE:
    case OperatorType::CROP_ROTATE:
    case OperatorType::LENS_CALIBRATION:
    case OperatorType::COLOR_TEMP:
    case OperatorType::FILM_GRAIN:
    case OperatorType::HALATION:
      return static_cast<OperatorType>(raw);
  }
  throw std::runtime_error("EditCommit payload: invalid OperatorType value");
}

auto ParsePipelineStageName(const nlohmann::json& value) -> PipelineStageName {
  if (!value.is_number_integer()) {
    throw std::runtime_error("EditCommit payload: stage_name must be an integer");
  }
  const int raw = value.get<int>();
  switch (static_cast<PipelineStageName>(raw)) {
    case PipelineStageName::Image_Loading:
    case PipelineStageName::Geometry_Adjustment:
    case PipelineStageName::To_WorkingSpace:
    case PipelineStageName::Basic_Adjustment:
    case PipelineStageName::Color_Adjustment:
    case PipelineStageName::Detail_Adjustment:
    case PipelineStageName::Output_Transform:
      return static_cast<PipelineStageName>(raw);
  }
  throw std::runtime_error("EditCommit payload: invalid PipelineStageName value");
}

void RequireCanonicalPayloadText(const nlohmann::json& stored, const nlohmann::json& canonical,
                                 const char* context) {
  // dump() order is fixed by CanonicalJSON construction; non-canonical stored text is corrupt.
  if (stored.dump() != canonical.dump()) {
    throw std::runtime_error(std::string(context) + ": stored payload is not in canonical form");
  }
}

std::mutex                 g_commit_clock_mutex;
std::atomic<std::uint64_t> g_commit_clock_previous{0};

auto                       WallClockNowNs() -> std::uint64_t {
  const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}

}  // namespace

auto OrdinaryEditPayload::CanonicalJSON() const -> nlohmann::json {
  return nlohmann::json{{"kind", "edit"},
                        {"operator_type", OperatorTypeToInt(operator_type)},
                        {"stage_name", StageNameToInt(stage_name)},
                        {"field_name", field_name},
                        {"before_value", before_value},
                        {"after_value", after_value},
                        {"before_enabled", before_enabled},
                        {"after_enabled", after_enabled}};
}

auto OrdinaryEditPayload::FromJSON(const nlohmann::json& j) -> OrdinaryEditPayload {
  RequireExactObjectKeys(j,
                         {"kind", "operator_type", "stage_name", "field_name", "before_value",
                          "after_value", "before_enabled", "after_enabled"},
                         "OrdinaryEditPayload");
  if (!j.at("kind").is_string() || j.at("kind").get<std::string>() != "edit") {
    throw std::runtime_error("OrdinaryEditPayload: kind must be 'edit'");
  }
  if (!j.at("field_name").is_string()) {
    throw std::runtime_error("OrdinaryEditPayload: field_name must be a string");
  }
  if (!j.at("before_enabled").is_boolean() || !j.at("after_enabled").is_boolean()) {
    throw std::runtime_error("OrdinaryEditPayload: enabled flags must be booleans");
  }

  OrdinaryEditPayload payload;
  payload.operator_type  = ParseOperatorType(j.at("operator_type"));
  payload.stage_name     = ParsePipelineStageName(j.at("stage_name"));
  payload.field_name     = j.at("field_name").get<std::string>();
  payload.before_value   = j.at("before_value");
  payload.after_value    = j.at("after_value");
  payload.before_enabled = j.at("before_enabled").get<bool>();
  payload.after_enabled  = j.at("after_enabled").get<bool>();
  RequireCanonicalPayloadText(j, payload.CanonicalJSON(), "OrdinaryEditPayload");
  return payload;
}

auto MergeFieldDelta::IdentityKey() const -> std::string {
  return std::to_string(OperatorTypeToInt(operator_type)) + "|" +
         std::to_string(StageNameToInt(stage_name)) + "|" + field_name;
}

auto MergeFieldDelta::CanonicalJSON() const -> nlohmann::json {
  return nlohmann::json{{"operator_type", OperatorTypeToInt(operator_type)},
                        {"stage_name", StageNameToInt(stage_name)},
                        {"field_name", field_name},
                        {"before_value", before_value},
                        {"before_enabled", before_enabled},
                        {"resolved_value", resolved_value},
                        {"resolved_enabled", resolved_enabled}};
}

auto MergeFieldDelta::FromJSON(const nlohmann::json& j) -> MergeFieldDelta {
  RequireExactObjectKeys(j,
                         {"operator_type", "stage_name", "field_name", "before_value",
                          "before_enabled", "resolved_value", "resolved_enabled"},
                         "MergeFieldDelta");
  if (!j.at("field_name").is_string()) {
    throw std::runtime_error("MergeFieldDelta: field_name must be a string");
  }
  if (!j.at("before_enabled").is_boolean() || !j.at("resolved_enabled").is_boolean()) {
    throw std::runtime_error("MergeFieldDelta: enabled flags must be booleans");
  }

  MergeFieldDelta field;
  field.operator_type    = ParseOperatorType(j.at("operator_type"));
  field.stage_name       = ParsePipelineStageName(j.at("stage_name"));
  field.field_name       = j.at("field_name").get<std::string>();
  field.before_value     = j.at("before_value");
  field.before_enabled   = j.at("before_enabled").get<bool>();
  field.resolved_value   = j.at("resolved_value");
  field.resolved_enabled = j.at("resolved_enabled").get<bool>();
  return field;
}

void MergeEditPayload::CanonicalizeAndValidate() {
  for (const auto& field : fields) {
    (void)ParseOperatorType(field.CanonicalJSON().at("operator_type"));
    (void)ParsePipelineStageName(field.CanonicalJSON().at("stage_name"));
  }
  std::sort(fields.begin(), fields.end(), [](const MergeFieldDelta& a, const MergeFieldDelta& b) {
    return a.IdentityKey() < b.IdentityKey();
  });
  for (std::size_t i = 1; i < fields.size(); ++i) {
    if (fields[i].IdentityKey() == fields[i - 1].IdentityKey()) {
      throw std::runtime_error("MergeEditPayload: duplicate field identity '" +
                               fields[i].IdentityKey() + "'");
    }
  }
}

auto MergeEditPayload::CanonicalJSON() const -> nlohmann::json {
  auto canonical = *this;
  canonical.CanonicalizeAndValidate();
  nlohmann::json field_array = nlohmann::json::array();
  for (const auto& field : canonical.fields) {
    field_array.push_back(field.CanonicalJSON());
  }
  return nlohmann::json{{"kind", "merge"}, {"fields", std::move(field_array)}};
}

auto MergeEditPayload::FromJSON(const nlohmann::json& j) -> MergeEditPayload {
  RequireExactObjectKeys(j, {"kind", "fields"}, "MergeEditPayload");
  if (!j.at("kind").is_string() || j.at("kind").get<std::string>() != "merge") {
    throw std::runtime_error("MergeEditPayload: kind must be 'merge'");
  }
  const auto& fields_j = j.at("fields");
  if (!fields_j.is_array()) {
    throw std::runtime_error("MergeEditPayload: fields must be an array");
  }
  MergeEditPayload payload;
  payload.fields.reserve(fields_j.size());
  for (const auto& field_j : fields_j) {
    payload.fields.push_back(MergeFieldDelta::FromJSON(field_j));
  }
  payload.CanonicalizeAndValidate();
  RequireCanonicalPayloadText(j, payload.CanonicalJSON(), "MergeEditPayload");
  return payload;
}

auto EditCommit::MakeEdit(root_id_t root_id, head_commit_hash_t first_parent,
                          OrdinaryEditPayload payload) -> EditCommit {
  return MakeEditAtTimestamp(root_id, std::move(first_parent),
                             CommitClock::NextGlobal(WallClockNowNs()), std::move(payload));
}

auto EditCommit::MakeEditAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                     std::uint64_t created_at_ns, OrdinaryEditPayload payload)
    -> EditCommit {
  EditCommit commit;
  commit.root_id_            = root_id;
  commit.first_parent_hash_  = std::move(first_parent);
  commit.second_parent_hash_ = std::nullopt;
  commit.created_at_ns_      = created_at_ns;
  commit.kind_               = EditCommitKind::kEdit;
  commit.edit_payload_       = payload.CanonicalJSON();
  commit.ValidateStructure();
  commit.FinalizeHash();
  return commit;
}

auto EditCommit::MakeMerge(root_id_t root_id, head_commit_hash_t first_parent,
                           commit_hash_t second_parent, MergeEditPayload payload) -> EditCommit {
  return MakeMergeAtTimestamp(root_id, std::move(first_parent), second_parent,
                              CommitClock::NextGlobal(WallClockNowNs()), std::move(payload));
}

auto EditCommit::MakeMergeAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                      commit_hash_t second_parent, std::uint64_t created_at_ns,
                                      MergeEditPayload payload) -> EditCommit {
  payload.CanonicalizeAndValidate();
  EditCommit commit;
  commit.root_id_            = root_id;
  commit.first_parent_hash_  = std::move(first_parent);
  commit.second_parent_hash_ = second_parent;
  commit.created_at_ns_      = created_at_ns;
  commit.kind_               = EditCommitKind::kMerge;
  commit.edit_payload_       = payload.CanonicalJSON();
  commit.ValidateStructure();
  commit.FinalizeHash();
  return commit;
}

void EditCommit::ValidateStructure() const {
  if (kind_ == EditCommitKind::kEdit) {
    if (second_parent_hash_.has_value()) {
      throw std::runtime_error("EditCommit: Edit kind must not have a second parent");
    }
    // FromJSON requires exact keys, enum ranges, and canonical dump equality.
    (void)OrdinaryEditPayload::FromJSON(edit_payload_);
    return;
  }
  if (kind_ == EditCommitKind::kMerge) {
    if (!second_parent_hash_.has_value()) {
      throw std::runtime_error("EditCommit: Merge kind requires exactly one second parent");
    }
    (void)MergeEditPayload::FromJSON(edit_payload_);
    return;
  }
  throw std::runtime_error("EditCommit: unknown commit kind");
}

auto EditCommit::CanonicalHashInput() const -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(128);
  AppendU32LE(bytes, kCommitFormatVersion);
  AppendHash(bytes, root_id_);
  AppendOptionalHash(bytes, first_parent_hash_);
  AppendOptionalHash(bytes, second_parent_hash_);
  AppendU64LE(bytes, created_at_ns_);
  AppendU8(bytes, static_cast<std::uint8_t>(kind_));
  const std::string payload_text = edit_payload_.dump();
  AppendBytes(bytes, payload_text.data(), payload_text.size());
  return bytes;
}

auto EditCommit::ComputeCommitHash() const -> commit_hash_t {
  const auto bytes = CanonicalHashInput();
  return Hash128::Compute(bytes.data(), bytes.size());
}

void EditCommit::FinalizeHash() { commit_hash_ = ComputeCommitHash(); }

auto EditCommit::ToJSON() const -> nlohmann::json {
  nlohmann::json j;
  j["commit_hash"]       = commit_hash_.ToString();
  j["root_id"]           = root_id_.ToString();
  j["first_parent_hash"] = HeadCommitHashToStorage(first_parent_hash_);
  j["second_parent_hash"] =
      second_parent_hash_.has_value() ? second_parent_hash_->ToString() : std::string{};
  j["created_at_ns"] = created_at_ns_;
  j["kind"]          = EditCommitKindToString(kind_);
  j["edit_payload"]  = edit_payload_;
  return j;
}

auto EditCommit::FromJSON(const nlohmann::json& j) -> EditCommit {
  if (!j.is_object()) {
    throw std::runtime_error("EditCommit: expected object");
  }
  EditCommit commit;
  commit.commit_hash_ = Hash128::FromString(j.at("commit_hash").get<std::string>());
  commit.root_id_     = Hash128::FromString(j.at("root_id").get<std::string>());
  commit.first_parent_hash_ =
      HeadCommitHashFromStorage(j.value("first_parent_hash", std::string{}));
  const auto second = j.value("second_parent_hash", std::string{});
  if (!second.empty()) {
    commit.second_parent_hash_ = Hash128::FromString(second);
  }
  commit.created_at_ns_ = j.at("created_at_ns").get<std::uint64_t>();
  if (j.at("kind").is_string()) {
    commit.kind_ = EditCommitKindFromString(j.at("kind").get<std::string>());
  } else if (j.at("kind").is_number_integer()) {
    commit.kind_ = EditCommitKindFromInt(j.at("kind").get<int>());
  } else {
    throw std::runtime_error("EditCommit: kind must be a string or integer");
  }
  commit.edit_payload_ = j.at("edit_payload");
  commit.ValidateStructure();
  const auto expected = commit.ComputeCommitHash();
  if (expected != commit.commit_hash_) {
    throw std::runtime_error("EditCommit: stored commit_hash does not match payload");
  }
  return commit;
}

auto CommitClock::PreviousGlobal() -> std::uint64_t {
  return g_commit_clock_previous.load(std::memory_order_acquire);
}

auto CommitClock::NextGlobal(std::uint64_t now_ns) -> std::uint64_t {
  std::lock_guard     lock(g_commit_clock_mutex);
  const std::uint64_t previous = g_commit_clock_previous.load(std::memory_order_relaxed);
  if (previous == std::numeric_limits<std::uint64_t>::max()) {
    throw std::runtime_error("CommitClock: timestamp space exhausted");
  }

  std::uint64_t next = 0;
  if (previous == 0) {
    next = now_ns == 0 ? 1 : now_ns;
  } else if (now_ns > previous) {
    next = now_ns;
  } else {
    if (previous == std::numeric_limits<std::uint64_t>::max()) {
      throw std::runtime_error("CommitClock: timestamp space exhausted");
    }
    next = previous + 1;
    if (next == 0) {
      throw std::runtime_error("CommitClock: timestamp space exhausted");
    }
  }

  if (next <= previous && previous != 0) {
    throw std::runtime_error("CommitClock: failed to advance timestamp");
  }
  g_commit_clock_previous.store(next, std::memory_order_relaxed);
  return next;
}

void CommitClock::ResetGlobalForTesting(std::uint64_t previous_ns) {
  std::lock_guard lock(g_commit_clock_mutex);
  g_commit_clock_previous.store(previous_ns, std::memory_order_relaxed);
}

auto RootChainHashInput(const root_id_t& root_id) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  AppendU32LE(bytes, kChainFormatVersion);
  AppendHash(bytes, root_id);
  return bytes;
}

auto ComputeRootChainHash(const root_id_t& root_id) -> transaction_chain_hash_t {
  const auto bytes = RootChainHashInput(root_id);
  return Hash128::Compute(bytes.data(), bytes.size());
}

auto TransactionChainFoldInput(const transaction_chain_hash_t& previous,
                               const commit_hash_t& commit_hash) -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  AppendHash(bytes, previous);
  AppendHash(bytes, commit_hash);
  return bytes;
}

auto FoldTransactionChainHash(const transaction_chain_hash_t& previous,
                              const commit_hash_t& commit_hash) -> transaction_chain_hash_t {
  const auto bytes = TransactionChainFoldInput(previous, commit_hash);
  return Hash128::Compute(bytes.data(), bytes.size());
}

auto FoldFirstParentChain(const root_id_t&                  root_id,
                          const std::vector<commit_hash_t>& first_parent_commits)
    -> transaction_chain_hash_t {
  auto chain = ComputeRootChainHash(root_id);
  for (const auto& commit_hash : first_parent_commits) {
    chain = FoldTransactionChainHash(chain, commit_hash);
  }
  return chain;
}

}  // namespace alcedo
