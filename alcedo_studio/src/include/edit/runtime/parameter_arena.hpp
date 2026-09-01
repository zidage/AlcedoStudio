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

#include "edit/operators/models/operator_param_dto.hpp"
#include "edit/runtime/byte_range.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {

/**
 * @brief Grow-only host+device parameter buffer with dirty-range H2D copies.
 *
 * Offsets assigned by BindSlot stay stable across renders. Grows only when the
 * backend has no in-flight GPU submission. Not thread-safe.
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
   * @brief Copy every bound field from a full DTO. Does not read dirty bits.
   * Queues the whole slot for upload.
   */
  void InitializeFromFullDto(const ParameterSlotKey& key, const OperatorParamDto& dto) {
    const auto& binding = Binding(key);
    CopyFields(binding, dto.payload.get(), /*dirty_only=*/false, {});
    pending_.push_back(ByteRange{binding.offset, binding.size});
  }

  /**
   * @brief Copy dirty fields from a patch into the host mirror and queue ranges.
   */
  void ApplyPatch(const ParameterSlotKey& key, const OperatorParamPatchDto& patch) {
    const auto& binding = Binding(key);
    CopyFields(binding, patch.payload.get(), /*dirty_only=*/true, patch.dirty_fields);
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

  void CopyFields(const ParameterBinding& binding, const IOperatorParamPayload* payload,
                  bool dirty_only, DirtyFieldMask dirty) {
    if (payload == nullptr) {
      throw std::runtime_error("ParameterArena: missing payload");
    }
    const auto bytes = payload->Bytes();
    for (const auto& field : binding.fields) {
      if (dirty_only && !dirty.Contains(field.dirty_bit)) {
        continue;
      }
      if (static_cast<std::size_t>(field.source_offset) + field.size > bytes.size()) {
        throw std::runtime_error("ParameterArena: field exceeds payload");
      }
      if (static_cast<std::size_t>(field.destination_offset) + field.size > host_.size()) {
        throw std::runtime_error("ParameterArena: field exceeds arena");
      }
      std::memcpy(host_.data() + field.destination_offset, bytes.data() + field.source_offset,
                  field.size);
      if (dirty_only) {
        pending_.push_back(ByteRange{field.destination_offset, field.size});
      }
    }
  }

  Backend*                                     backend_ = nullptr;
  typename Backend::Buffer                     device_{};
  std::vector<std::byte>                       host_;
  std::size_t                                  capacity_ = 0;
  std::size_t                                  used_     = 0;
  std::map<ParameterSlotKey, ParameterBinding> slots_;
  std::vector<ByteRange>                       pending_;
};

}  // namespace alcedo
