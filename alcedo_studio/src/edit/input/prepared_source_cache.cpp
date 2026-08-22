//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/input/prepared_source_cache.hpp"

#include <stdexcept>
#include <utility>

namespace alcedo {
namespace {

auto DefaultUnpack(std::span<const std::byte> encoded, DecodeRes decode_res) -> PreparedRawInput {
  return RawInputLoader::LoadEncoded(encoded, decode_res);
}

auto HostBytes(const PreparedRawInput& input) -> std::size_t { return input.pixels.ByteCount(); }

}  // namespace

PreparedSourceCache::Lease::Lease(const Lease& other) : entry_(other.entry_) {}

auto PreparedSourceCache::Lease::operator=(const Lease& other) -> Lease& {
  entry_ = other.entry_;
  return *this;
}

PreparedSourceCache::Lease::Lease(Lease&& other) noexcept : entry_(std::move(other.entry_)) {}

auto PreparedSourceCache::Lease::operator=(Lease&& other) noexcept -> Lease& {
  entry_ = std::move(other.entry_);
  return *this;
}

PreparedSourceCache::Lease::~Lease() = default;

auto PreparedSourceCache::Lease::Get() const -> const PreparedRawInput& {
  if (!entry_ || !entry_->input) {
    throw std::runtime_error("PreparedSourceCache::Lease: empty lease");
  }
  return *entry_->input;
}

auto PreparedSourceCache::Lease::Key() const -> const PreparedSourceKey& {
  if (!entry_) {
    throw std::runtime_error("PreparedSourceCache::Lease: empty lease");
  }
  return entry_->key;
}

auto PreparedSourceCache::Lease::Shared() const -> std::shared_ptr<const PreparedRawInput> {
  if (!entry_) {
    return nullptr;
  }
  return entry_->input;
}

PreparedSourceCache::PreparedSourceCache() : unpack_(DefaultUnpack) {}

PreparedSourceCache::PreparedSourceCache(UnpackFn unpack)
    : unpack_(unpack ? std::move(unpack) : UnpackFn{DefaultUnpack}) {}

auto PreparedSourceCache::HashEncoded(std::span<const std::byte> encoded) -> std::uint64_t {
  if (encoded.data() == memo_ptr_ && encoded.size() == memo_size_) {
    return memo_hash_;
  }
  const auto hash = HashContentBytes(encoded);
  memo_ptr_       = encoded.data();
  memo_size_      = encoded.size();
  memo_hash_      = hash;
  return hash;
}

void PreparedSourceCache::SetHostByteBudget(std::size_t bytes) {
  byte_budget_ = bytes;
  EvictUnleasedToFit(0);
}

void PreparedSourceCache::EvictUnleasedToFit(std::size_t extra_bytes) {
  if (byte_budget_ == 0) {
    return;
  }
  while (bytes_used_ + extra_bytes > byte_budget_) {
    std::shared_ptr<Entry> victim;
    PreparedSourceLookup   victim_key{};
    for (auto& [key, entry] : entries_) {
      if (!entry || entry.use_count() != 1) {
        continue;
      }
      if (!victim || entry->last_used < victim->last_used) {
        victim     = entry;
        victim_key = key;
      }
    }
    if (!victim) {
      return;
    }
    bytes_used_ -= victim->bytes;
    entries_.erase(victim_key);
  }
}

auto PreparedSourceCache::MakeLease(std::shared_ptr<Entry> entry) -> Lease {
  entry->last_used = use_clock_++;
  Lease lease;
  lease.entry_ = std::move(entry);
  return lease;
}

auto PreparedSourceCache::AcquireEncoded(std::span<const std::byte> encoded, DecodeRes decode_res)
    -> Lease {
  const auto lookup = PreparedSourceLookup{
      HashEncoded(encoded), encoded.size(), DecodeResToDownsamplePasses(decode_res),
      kRawInputPreparationVersion};
  if (auto it = entries_.find(lookup); it != entries_.end()) {
    ++stats_.hits;
    return MakeLease(it->second);
  }

  ++stats_.misses;
  ++stats_.libraw_open_unpack_count;
  auto prepared                        = unpack_(encoded, decode_res);
  prepared.content_key.content_hash    = lookup.encoded_content_hash;
  prepared.source_key.encoded_content_hash = lookup.encoded_content_hash;
  prepared.source_key.encoded_byte_count   = lookup.encoded_byte_count;
  prepared.source_key.downsample_passes    = lookup.downsample_passes;
  prepared.source_key.preparation_version  = lookup.preparation_version;

  auto entry          = std::make_shared<Entry>();
  entry->key          = prepared.source_key;
  entry->lookup       = lookup;
  entry->bytes        = HostBytes(prepared);
  entry->input        = std::make_shared<const PreparedRawInput>(std::move(prepared));
  EvictUnleasedToFit(entry->bytes);
  bytes_used_ += entry->bytes;
  auto [it, inserted] = entries_.emplace(lookup, entry);
  (void)inserted;
  return MakeLease(it->second);
}

}  // namespace alcedo
