//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>

#include "edit/runtime/content_key.hpp"
#include "edit/runtime/scene_work_member.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

/**
 * @brief Two RGBA32F scene working images owned by one render workspace.
 *
 * Owns native allocations only: extent, format, and byte/allocation counters.
 * Does not store GraphValueId, NodeId, revision, document, parameters, or
 * whether the pixels are valid. Same extent/format keeps the allocations;
 * a change releases both members after the previous GPU submission has
 * completed and recreates them together.
 *
 * @tparam Backend Factory with CreateSceneWorkImage(width, height) returning
 *         Backend::SceneWorkImage. CUDA and Metal use ordinary RGBA32F
 *         textures; OpenCL uses a dedicated row-major buffer type.
 */
template <class Backend>
class SceneWorkImagePair {
 public:
  using Image = typename Backend::SceneWorkImage;

  SceneWorkImagePair() = default;

  SceneWorkImagePair(const SceneWorkImagePair&)                    = delete;
  auto operator=(const SceneWorkImagePair&) -> SceneWorkImagePair& = delete;

  SceneWorkImagePair(SceneWorkImagePair&&)                    = default;
  auto operator=(SceneWorkImagePair&&) -> SceneWorkImagePair& = default;

  /**
   * @brief Confirm both members match @p extent as RGBA32F, allocating or replacing as needed.
   *
   * @pre The previous GPU submission has completed so old members have no readers.
   * @throws std::runtime_error when @p extent is empty.
   */
  void Ensure(Backend& backend, ImageExtent extent) {
    if (extent.width == 0 || extent.height == 0) {
      throw std::runtime_error("SceneWorkImagePair::Ensure: extent must be positive");
    }
    if (allocated_ && extent_ == extent && format_ == TextureFormat::Rgba32f) {
      return;
    }
    Release();
    members_[0] = backend.CreateSceneWorkImage(extent.width, extent.height);
    members_[1] = backend.CreateSceneWorkImage(extent.width, extent.height);
    extent_     = extent;
    format_     = TextureFormat::Rgba32f;
    allocated_  = true;
    allocation_count_ += 2;
    current_bytes_ = PairBytes(extent);
    if (current_bytes_ > peak_bytes_) {
      peak_bytes_ = current_bytes_;
    }
  }

  /** @brief Destroy both native members. Bytes drop to zero; allocation count is kept. */
  void Release() {
    members_[0]    = Image{};
    members_[1]    = Image{};
    allocated_     = false;
    current_bytes_ = 0;
    extent_        = {};
    format_        = TextureFormat::Rgba32f;
  }

  [[nodiscard]] auto Member(SceneWorkMember member) -> Image& {
    RequireAllocated();
    return members_[Index(member)];
  }

  [[nodiscard]] auto Member(SceneWorkMember member) const -> const Image& {
    RequireAllocated();
    return members_[Index(member)];
  }

  [[nodiscard]] auto Peer(SceneWorkMember member) -> Image& { return Member(PeerOf(member)); }

  [[nodiscard]] auto Peer(SceneWorkMember member) const -> const Image& {
    return Member(PeerOf(member));
  }

  [[nodiscard]] auto Allocated() const -> bool { return allocated_; }
  [[nodiscard]] auto MemberCount() const -> std::size_t { return allocated_ ? 2 : 0; }
  [[nodiscard]] auto Extent() const -> ImageExtent { return extent_; }
  [[nodiscard]] auto Format() const -> TextureFormat { return format_; }
  [[nodiscard]] auto CurrentBytes() const -> std::size_t { return current_bytes_; }
  [[nodiscard]] auto PeakBytes() const -> std::size_t { return peak_bytes_; }
  [[nodiscard]] auto AllocationCount() const -> std::uint64_t { return allocation_count_; }

  /** @brief RGBA32F byte size of both members for @p extent. */
  [[nodiscard]] static auto PairBytes(ImageExtent extent) -> std::size_t {
    return static_cast<std::size_t>(extent.width) * static_cast<std::size_t>(extent.height) *
           TextureFormatBytesPerPixel(TextureFormat::Rgba32f) * 2;
  }

 private:
  [[nodiscard]] static auto Index(SceneWorkMember member) -> std::size_t {
    return member == SceneWorkMember::Member0 ? 0 : 1;
  }

  void RequireAllocated() const {
    if (!allocated_) {
      throw std::runtime_error("SceneWorkImagePair: members are not allocated");
    }
  }

  std::array<Image, 2> members_{};
  ImageExtent          extent_{};
  TextureFormat        format_           = TextureFormat::Rgba32f;
  std::size_t          current_bytes_    = 0;
  std::size_t          peak_bytes_       = 0;
  std::uint64_t        allocation_count_ = 0;
  bool                 allocated_        = false;
};

}  // namespace alcedo
