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

auto WallClockNowNs() -> std::uint64_t {
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}


std::mutex                g_commit_clock_mutex;
std::atomic<std::uint64_t> g_commit_clock_previous{0};

}  // namespace

auto EditCommit::MakePipelineEdit(root_id_t root_id, head_commit_hash_t first_parent,
                                 PipelineEditBatch payload) -> EditCommit {
  return MakePipelineEditAtTimestamp(root_id, std::move(first_parent),
                                     CommitClock::NextGlobal(WallClockNowNs()), std::move(payload));
}

auto EditCommit::MakePipelineEditAtTimestamp(root_id_t root_id, head_commit_hash_t first_parent,
                                            std::uint64_t created_at_ns,
                                            PipelineEditBatch payload) -> EditCommit {
  payload.Validate();
  EditCommit commit;
  commit.root_id_           = root_id;
  commit.first_parent_hash_ = std::move(first_parent);
  commit.created_at_ns_     = created_at_ns;
  commit.edit_payload_      = payload.CanonicalJSON();
  commit.ValidateStructure();
  commit.FinalizeHash();
  return commit;
}

void EditCommit::ValidateStructure() const {
  if (!IsPipelineEditBatchJson(edit_payload_)) {
    throw std::runtime_error("EditCommit: payload must be a pipeline edit batch");
  }
  (void)PipelineEditBatch::FromJSON(edit_payload_);
}

auto EditCommit::CanonicalHashInput() const -> std::vector<std::uint8_t> {
  std::vector<std::uint8_t> bytes;
  bytes.reserve(128);
  AppendU32LE(bytes, kCommitFormatVersion);
  AppendHash(bytes, root_id_);
  AppendOptionalHash(bytes, first_parent_hash_);
  AppendOptionalHash(bytes, std::nullopt);  // empty second parent
  AppendU64LE(bytes, created_at_ns_);
  AppendU8(bytes, 0);  // fixed edit marker (0)
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
  j["commit_hash"]        = commit_hash_.ToString();
  j["root_id"]            = root_id_.ToString();
  j["first_parent_hash"]  = HeadCommitHashToStorage(first_parent_hash_);
  j["second_parent_hash"] = std::string{};
  j["created_at_ns"]      = created_at_ns_;
  j["kind"]               = "edit";
  j["edit_payload"]       = edit_payload_;
  return j;
}

auto EditCommit::FromJSON(const nlohmann::json& j) -> EditCommit {
  RequireExactObjectKeys(
      j,
      {"commit_hash", "root_id", "first_parent_hash", "second_parent_hash", "created_at_ns",
       "kind", "edit_payload"},
      "EditCommit");
  if (!j.at("kind").is_string() || j.at("kind").get<std::string>() != "edit") {
    throw std::runtime_error("EditCommit: kind must be 'edit'");
  }
  if (!j.at("second_parent_hash").is_string() ||
      !j.at("second_parent_hash").get<std::string>().empty()) {
    throw std::runtime_error("EditCommit: second parent must be empty");
  }
  if (!j.at("created_at_ns").is_number_unsigned()) {
    throw std::runtime_error("EditCommit: created_at_ns must be an unsigned integer");
  }
  if (!j.at("first_parent_hash").is_string()) {
    throw std::runtime_error("EditCommit: first_parent_hash must be a string");
  }
  if (!j.at("root_id").is_string()) {
    throw std::runtime_error("EditCommit: root_id must be a string");
  }
  if (!j.at("commit_hash").is_string()) {
    throw std::runtime_error("EditCommit: commit_hash must be a string");
  }

  EditCommit commit;
  commit.commit_hash_       = Hash128::FromString(j.at("commit_hash").get<std::string>());
  commit.root_id_           = Hash128::FromString(j.at("root_id").get<std::string>());
  commit.first_parent_hash_ = HeadCommitHashFromStorage(j.at("first_parent_hash").get<std::string>());
  commit.created_at_ns_     = j.at("created_at_ns").get<std::uint64_t>();
  commit.edit_payload_      = j.at("edit_payload");

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
