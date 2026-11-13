//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/operators/models/dirty_field_mask.hpp"
#include "edit/operators/models/parameter_revision.hpp"
#include "edit/runtime/byte_range.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {

/**
 * @brief Grow-only host+device parameter buffer with dirty-range H2D copies.
 *
 * Offsets assigned by BindSlot stay stable across renders. Grows only when the
 * backend has no in-flight GPU submission. Each slot also records the Model revision whose
 * values its host bytes hold, so the owner packs a slot again only when that Model changed.
 * The arena belongs to one render workspace; it never writes the Model. Not thread-safe.
 *
 * @tparam Backend Must provide Buffer, CreateBuffer, UploadBufferRange,
 *         DownloadBufferRange, and HasInFlightSubmission.
 */
template <class Backend>
class ParameterArena {
 public:
  static constexpr std::uint32_t kSlotAlignment = 16;

  explicit ParameterArena(Backend& backend) : backend_(&backend) {}

  ParameterArena(const ParameterArena&)                    = delete;
  auto operator=(const ParameterArena&) -> ParameterArena& = delete;

  /**
   * @brief Pre-size host mirror and device buffer. No-op if @p bytes <= capacity.
   * @throws std::runtime_error if a GPU submission is still in flight.
   */
  void Reserve(std::size_t bytes) {
    if (bytes <= capacity_) {
      return;
    }
    ThrowIfBusy();
    auto new_device = backend_->CreateBuffer(bytes);
    host_.resize(bytes, std::byte{0});
    device_   = std::move(new_device);
    capacity_ = bytes;
    if (used_ > 0) {
      pending_.push_back(ByteRange{0, static_cast<std::uint32_t>(used_)});
    }
  }

  /**
   * @brief Assign the next aligned slot. Field destination_offset is relative to the slot.
   * @return Binding with absolute field destinations.
   */
  auto BindSlot(ParameterSlotKey key, std::uint32_t size,
                std::span<const ParameterFieldBinding> fields) -> ParameterBinding {
    const auto offset = static_cast<std::uint32_t>(AlignUp(used_, kSlotAlignment));
    const auto end    = static_cast<std::size_t>(offset) + size;
    if (end > capacity_) {
      Reserve(end);
    }
    ParameterBinding binding;
    binding.offset = offset;
    binding.size   = size;
    binding.fields.reserve(fields.size());
    for (const auto& field : fields) {
      ParameterFieldBinding absolute = field;
      absolute.destination_offset    = offset + field.destination_offset;
      binding.fields.push_back(absolute);
    }
    used_       = end;
    slots_[key] = binding;
    applied_revisions_.erase(key);
    return slots_[key];
  }

  [[nodiscard]] auto Binding(const ParameterSlotKey& key) const -> const ParameterBinding& {
    const auto it = slots_.find(key);
    if (it == slots_.end()) {
      throw std::runtime_error("ParameterArena: unknown slot");
    }
    return it->second;
  }

  [[nodiscard]] auto Contains(const ParameterSlotKey& key) const -> bool {
    return slots_.contains(key);
  }

  /**
   * @brief Copy packed GPU parameter bytes into a bound slot and queue the slot for upload.
   *
   * @p bytes must match the bound slot size. This writes the host mirror used for H2D
   * copies; it does not wrap a Model DTO. Not thread-safe.
   * @throws std::runtime_error if the slot is missing or @p bytes does not match the slot.
   */
  void WritePackedBytes(const ParameterSlotKey& key, std::span<const std::byte> bytes,
                        ParameterRevision applied_revision = kNoParameterRevision) {
    const auto& binding = Binding(key);
    if (bytes.size() != binding.size) {
      throw std::runtime_error("ParameterArena: packed slot size mismatch");
    }
    std::memcpy(host_.data() + binding.offset, bytes.data(), bytes.size());
    pending_.push_back(ByteRange{binding.offset, binding.size});
    applied_revisions_[key] = applied_revision;
  }

  /**
   * @brief Copy a trivially-copyable GPU parameter struct into a bound slot.
   *
   * @param applied_revision Model revision the packed values come from, or
   *        @ref kNoParameterRevision when they do not come from one Model revision (for
   *        example an export color override). The slot is recorded with this revision.
   */
  template <class Packed>
  void WritePackedSlot(const ParameterSlotKey& key, const Packed& packed,
                       ParameterRevision applied_revision = kNoParameterRevision) {
    WritePackedBytes(
        key,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(&packed), sizeof(Packed)),
        applied_revision);
  }

  /**
   * @brief Bind the slot if missing, then write packed GPU bytes.
   *
   * This is the production arena write for already-resolved GPU parameter structs.
   * It does not wrap a Model DTO.
   */
  template <class Packed>
  void BindOrWritePackedSlot(const ParameterSlotKey& key, DirtyFieldMask fields,
                             const Packed&     packed,
                             ParameterRevision applied_revision = kNoParameterRevision) {
    if (!Contains(key)) {
      const ParameterFieldBinding field{fields, 0, 0, static_cast<std::uint32_t>(sizeof(Packed))};
      BindSlot(key, static_cast<std::uint32_t>(sizeof(Packed)), std::span{&field, 1});
    }
    WritePackedSlot(key, packed, applied_revision);
  }

  /**
   * @brief Model revision whose values the slot's host bytes hold.
   *
   * The bytes reach the device with the next successful @ref UploadDirty; a failed upload keeps
   * them queued, so the recorded revision stays true for the device buffer after the retry.
   * @return @ref kNoParameterRevision when the slot is missing or was written without a revision.
   */
  [[nodiscard]] auto AppliedRevision(const ParameterSlotKey& key) const -> ParameterRevision {
    const auto it = applied_revisions_.find(key);
    return it == applied_revisions_.end() ? kNoParameterRevision : it->second;
  }

  /**
   * @brief Merge queued dirty ranges and upload them. No copy when nothing is dirty.
   */
  void UploadDirty(typename Backend::CommandContext& command_context) {
    backend_->NoteHostToDeviceBegin();
    if (pending_.empty()) {
      return;
    }
    auto merged = MergeAdjacentRanges(std::move(pending_));
    pending_.clear();
    try {
      for (const auto& range : merged) {
        if (range.size == 0) {
          continue;
        }
        backend_->UploadBufferRange(
            device_, range.offset,
            std::span<const std::byte>(host_.data() + range.offset, range.size), command_context);
      }
    } catch (...) {
      pending_.insert(pending_.end(), merged.begin(), merged.end());
      throw;
    }
  }

  void Download(std::uint32_t offset, std::span<std::byte> out,
                typename Backend::CommandContext& command_context) const {
    backend_->DownloadBufferRange(device_, offset, out, command_context);
  }

  [[nodiscard]] auto HostSpan() const -> std::span<const std::byte> { return host_; }
  [[nodiscard]] auto capacity_bytes() const -> std::size_t { return capacity_; }
  [[nodiscard]] auto used_bytes() const -> std::size_t { return used_; }
  [[nodiscard]] auto SlotCount() const -> std::size_t { return slots_.size(); }
  [[nodiscard]] auto HasPendingUpload() const -> bool { return !pending_.empty(); }
  [[nodiscard]] auto DeviceBuffer() const -> const typename Backend::Buffer& { return device_; }

  /**
   * @brief Drop host and device parameter storage. Caller must WaitIdle first.
   */
  void Clear() {
    slots_.clear();
    applied_revisions_.clear();
    pending_.clear();
    host_.clear();
    device_   = {};
    used_     = 0;
    capacity_ = 0;
  }

 private:
  static auto AlignUp(std::size_t value, std::uint32_t alignment) -> std::size_t {
    return (value + alignment - 1) & ~(static_cast<std::size_t>(alignment) - 1);
  }

  void ThrowIfBusy() const {
    if (backend_->HasInFlightSubmission()) {
      throw std::runtime_error("ParameterArena: cannot grow while a GPU submission is in flight");
    }
  }

  Backend*                                     backend_ = nullptr;
  typename Backend::Buffer                     device_{};
  std::vector<std::byte>                       host_;
  std::size_t                                  capacity_ = 0;
  std::size_t                                  used_     = 0;
  std::map<ParameterSlotKey, ParameterBinding> slots_;
  std::map<ParameterSlotKey, ParameterRevision> applied_revisions_;
  std::vector<ByteRange>                       pending_;
};

}  // namespace alcedo
