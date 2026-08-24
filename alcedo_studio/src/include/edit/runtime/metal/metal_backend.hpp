//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/runtime/byte_range.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {

inline constexpr std::uint32_t kMetalDagBackendCapabilityVersion = 2;

/**
 * @brief Host command-buffer identity for one in-flight Metal submission.
 *
 * Native MTLCommandBuffer lives in later Metal runtime TUs. Not thread-safe.
 */
class MetalCommandContext {
 public:
  MetalCommandContext() = default;

  [[nodiscard]] auto SubmissionId() const -> std::uint64_t { return submission_id_; }
  void               SetSubmissionId(std::uint64_t id) { submission_id_ = id; }

 private:
  std::uint64_t submission_id_ = 0;
};

/**
 * @brief Host-side Metal backend traits for Renderer and workspace instantiation.
 *
 * Native Metal types are not included. Resource creation fails explicitly until
 * the Metal runtime lands. There is no CPU or CUDA substitute.
 */
class MetalBackend {
 public:
  class Buffer {
   public:
    Buffer() = default;
    Buffer(Buffer&&) noexcept = default;
    auto operator=(Buffer&&) noexcept -> Buffer& = default;
    Buffer(const Buffer&)                        = delete;
    auto operator=(const Buffer&) -> Buffer&     = delete;
    ~Buffer()                                    = default;

    [[nodiscard]] auto DevicePointer() const -> void* { return nullptr; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return 0; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return 0; }
    [[nodiscard]] auto Empty() const -> bool { return true; }
    void               Reset() noexcept {}
  };

  class Texture2D {
   public:
    Texture2D() = default;
    Texture2D(Texture2D&&) noexcept = default;
    auto operator=(Texture2D&&) noexcept -> Texture2D& = default;
    Texture2D(const Texture2D&)                        = delete;
    auto operator=(const Texture2D&) -> Texture2D&     = delete;
    ~Texture2D()                                       = default;

    [[nodiscard]] auto DevicePointer() const -> void* { return nullptr; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return 0; }
    [[nodiscard]] auto Width() const -> std::uint32_t { return 0; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return 0; }
    [[nodiscard]] auto Format() const -> TextureFormat { return TextureFormat::R8; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return 0; }
    void               Reset() noexcept {}
  };

  using Slab           = Buffer;
  using CommandContext = MetalCommandContext;

  static constexpr std::uint32_t kCapabilityVersion = kMetalDagBackendCapabilityVersion;
  static constexpr const char*   kName              = "Metal";

  /**
   * @brief Session texture budget used until Metal device queries land.
   *
   * Matches the CUDA floor so plan compilation and workspace setup share size policy.
   */
  static auto DefaultTextureBudgetBytes() -> std::size_t { return 256ull << 20; }

  MetalBackend()                                   = default;
  MetalBackend(const MetalBackend&)                = delete;
  auto operator=(const MetalBackend&) -> MetalBackend& = delete;

  [[nodiscard]] auto CreateBuffer(std::size_t) -> Buffer {
    throw std::runtime_error("MetalBackend: GPU buffer allocation is not implemented");
  }
  [[nodiscard]] auto CreateSlab(std::size_t bytes) -> Buffer { return CreateBuffer(bytes); }
  [[nodiscard]] auto CreateTexture2D(std::uint32_t, std::uint32_t, TextureFormat) -> Texture2D {
    throw std::runtime_error("MetalBackend: GPU texture allocation is not implemented");
  }

  void UploadBufferRange(Buffer&, std::uint32_t, std::span<const std::byte>, CommandContext&) {
    throw std::runtime_error("MetalBackend: buffer upload is not implemented");
  }
  void DownloadBufferRange(const Buffer&, std::uint32_t, std::span<std::byte>,
                           CommandContext&) const {
    throw std::runtime_error("MetalBackend: buffer download is not implemented");
  }
  void UploadTexture2D(Texture2D&, std::span<const std::byte>, CommandContext&) {
    throw std::runtime_error("MetalBackend: texture upload is not implemented");
  }
  void CopyTexture2D(const Texture2D&, Texture2D&, CommandContext&) {
    throw std::runtime_error("MetalBackend: texture copy is not implemented");
  }
  void UploadR8TextureRect(Texture2D&, RectI, std::span<const std::byte>, CommandContext&) {
    throw std::runtime_error("MetalBackend: R8 upload is not implemented");
  }
  void DownloadTexture2D(const Texture2D&, std::span<std::byte>, CommandContext&) const {
    throw std::runtime_error("MetalBackend: texture download is not implemented");
  }

  void Submit(CommandContext& command_context) {
    in_flight_submission_ = command_context.SubmissionId();
  }
  void Wait(CommandContext&) {
    if (in_flight_submission_ == 0) {
      return;
    }
    completed_submission_ = in_flight_submission_;
    in_flight_submission_ = 0;
  }

  [[nodiscard]] auto HasInFlightSubmission() const -> bool { return in_flight_submission_ != 0; }
  [[nodiscard]] auto CompletedSubmission() const -> std::uint64_t { return completed_submission_; }
  [[nodiscard]] auto IsResourceBusy(std::uint64_t submitted_on) const -> bool {
    return submitted_on != 0 && submitted_on > completed_submission_;
  }
  [[nodiscard]] auto NextSubmissionId() -> std::uint64_t { return ++next_submission_; }

  void NoteFree() noexcept { ++free_count_; }
  void ResetCounters() {
    malloc_count_   = 0;
    free_count_     = 0;
    h2d_copy_count_ = 0;
    h2d_bytes_      = 0;
    last_h2d_ranges_.clear();
    last_texture_rectangles_.clear();
  }
  void FailNextUpload() { fail_next_upload_ = true; }
  void NoteHostToDeviceBegin() { last_h2d_ranges_.clear(); }

  [[nodiscard]] auto MallocCount() const -> std::uint64_t { return malloc_count_; }
  [[nodiscard]] auto FreeCount() const -> std::uint64_t { return free_count_; }
  [[nodiscard]] auto HostToDeviceCopyCount() const -> std::uint64_t { return h2d_copy_count_; }
  [[nodiscard]] auto HostToDeviceBytes() const -> std::uint64_t { return h2d_bytes_; }
  [[nodiscard]] auto LastHostToDeviceRanges() const -> const std::vector<ByteRange>& {
    return last_h2d_ranges_;
  }
  [[nodiscard]] auto LastTextureRectangles() const -> const std::vector<RectI>& {
    return last_texture_rectangles_;
  }

 private:
  std::uint64_t          malloc_count_         = 0;
  std::uint64_t          free_count_           = 0;
  std::uint64_t          h2d_copy_count_       = 0;
  std::uint64_t          h2d_bytes_            = 0;
  std::uint64_t          next_submission_      = 0;
  std::uint64_t          in_flight_submission_ = 0;
  std::uint64_t          completed_submission_ = 0;
  bool                   fail_next_upload_     = false;
  std::vector<ByteRange> last_h2d_ranges_;
  std::vector<RectI>     last_texture_rectangles_;
};

}  // namespace alcedo
