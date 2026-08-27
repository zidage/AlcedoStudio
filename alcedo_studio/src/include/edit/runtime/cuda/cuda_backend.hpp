//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/runtime/byte_range.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

/**
 * @brief CUDA stream plus a recorded completion event for one in-flight submission.
 *
 * Owns the stream and event. Not thread-safe.
 */
class CudaCommandContext {
 public:
  CudaCommandContext();
  ~CudaCommandContext();

  CudaCommandContext(const CudaCommandContext&)                    = delete;
  auto operator=(const CudaCommandContext&) -> CudaCommandContext& = delete;
  CudaCommandContext(CudaCommandContext&& other) noexcept;
  auto               operator=(CudaCommandContext&& other) noexcept -> CudaCommandContext&;

  [[nodiscard]] auto Stream() const -> cudaStream_t { return stream_; }
  [[nodiscard]] auto Event() const -> cudaEvent_t { return event_; }
  [[nodiscard]] auto SubmissionId() const -> std::uint64_t { return submission_id_; }
  void               SetSubmissionId(std::uint64_t id) { submission_id_ = id; }

 private:
  void          Destroy() noexcept;

  cudaStream_t  stream_        = nullptr;
  cudaEvent_t   event_         = nullptr;
  std::uint64_t submission_id_ = 0;
};

/**
 * @brief CUDA resource factory and allocation/H2D counters for the DAG workspace.
 *
 * All cudaMalloc / cudaFree for this device go through CreateBuffer / CreateTexture2D
 * and Buffer/Texture2D destructors. Not thread-safe. One in-flight submission.
 */
class CudaBackend {
 public:
  static constexpr std::uint32_t kCapabilityVersion = 1;
  static constexpr const char*   kName              = "CUDA";

  /** @brief Session texture budget from device memory, floored at 256 MiB. */
  static auto DefaultTextureBudgetBytes() -> std::size_t;

  class Buffer {
   public:
    Buffer() = default;
    Buffer(CudaBackend* owner, void* ptr, std::size_t bytes, std::uint64_t id);
    ~Buffer();

    Buffer(const Buffer&)                    = delete;
    auto operator=(const Buffer&) -> Buffer& = delete;
    Buffer(Buffer&& other) noexcept;
    auto               operator=(Buffer&& other) noexcept -> Buffer&;

    [[nodiscard]] auto DevicePointer() const -> void* { return ptr_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }
    [[nodiscard]] auto Empty() const -> bool { return ptr_ == nullptr; }

    void               Reset() noexcept;

   private:
    CudaBackend*  owner_       = nullptr;
    void*         ptr_         = nullptr;
    std::size_t   bytes_       = 0;
    std::uint64_t resource_id_ = 0;
  };

  class Texture2D {
   public:
    Texture2D() = default;
    Texture2D(CudaBackend* owner, void* ptr, std::size_t bytes, std::uint32_t width,
              std::uint32_t height, TextureFormat format, std::uint64_t id);
    ~Texture2D();

    Texture2D(const Texture2D&)                    = delete;
    auto operator=(const Texture2D&) -> Texture2D& = delete;
    Texture2D(Texture2D&& other) noexcept;
    auto               operator=(Texture2D&& other) noexcept -> Texture2D&;

    [[nodiscard]] auto DevicePointer() const -> void* { return ptr_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto Width() const -> std::uint32_t { return width_; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return height_; }
    [[nodiscard]] auto Format() const -> TextureFormat { return format_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }

    void               Reset() noexcept;

   private:
    CudaBackend*  owner_       = nullptr;
    void*         ptr_         = nullptr;
    std::size_t   bytes_       = 0;
    std::uint32_t width_       = 0;
    std::uint32_t height_      = 0;
    TextureFormat format_      = TextureFormat::R8;
    std::uint64_t resource_id_ = 0;
  };

  using Slab                                                       = Buffer;
  using CommandContext                                             = CudaCommandContext;

  CudaBackend()                                                    = default;
  ~CudaBackend()                                                   = default;

  CudaBackend(const CudaBackend&)                                  = delete;
  auto               operator=(const CudaBackend&) -> CudaBackend& = delete;

  [[nodiscard]] auto CreateBuffer(std::size_t bytes) -> Buffer;
  [[nodiscard]] auto CreateSlab(std::size_t bytes) -> Buffer { return CreateBuffer(bytes); }
  [[nodiscard]] auto CreateTexture2D(std::uint32_t width, std::uint32_t height,
                                     TextureFormat format) -> Texture2D;

  void UploadBufferRange(Buffer& buffer, std::uint32_t offset, std::span<const std::byte> bytes,
                         CommandContext& command_context);
  void DownloadBufferRange(const Buffer& buffer, std::uint32_t offset, std::span<std::byte> out,
                           CommandContext& command_context) const;

  /**
   * @brief Copy tightly packed host rows into a linear Texture2D. @p bytes size must equal
   *        texture.Bytes().
   */
  void UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes,
                       CommandContext& command_context);
  /**
   * @brief Device-to-device copy of matching RGBA32F textures for identity geometry/camera.
   */
  void CopyTexture2D(const Texture2D& src, Texture2D& dst, CommandContext& command_context);
  /** @brief Upload a tightly packed R8 rectangle into an R8 texture. */
  void UploadR8TextureRect(Texture2D& texture, RectI rectangle, std::span<const std::byte> bytes,
                           CommandContext& command_context);
  void DownloadTexture2D(const Texture2D& texture, std::span<std::byte> out,
                         CommandContext& command_context) const;

  /**
   * @brief Host-to-device copy into an arbitrary device pointer (transient CFA, etc.).
   */
  void UploadDeviceMemory(void* dst, std::span<const std::byte> bytes,
                          CommandContext& command_context);

  void Submit(CommandContext& command_context);
  void Wait(CommandContext& command_context);
  /**
   * @brief Block until kernels already recorded on this stream have finished.
   *
   * Used to free Develop scratch before Geometry runs. Does not submit or change
   * in-flight submission state.
   */
  void SynchronizeRecordedWork(CommandContext& command_context);

  [[nodiscard]] auto HasInFlightSubmission() const -> bool { return in_flight_submission_ != 0; }
  [[nodiscard]] auto CompletedSubmission() const -> std::uint64_t { return completed_submission_; }
  [[nodiscard]] auto IsResourceBusy(std::uint64_t submitted_on) const -> bool {
    return submitted_on != 0 && submitted_on > completed_submission_;
  }
  [[nodiscard]] auto NextSubmissionId() -> std::uint64_t { return ++next_submission_; }

  void               NoteFree() noexcept { ++free_count_; }

  void               ResetCounters();
  void               FailNextUpload();
  void               NoteHostToDeviceBegin() { last_h2d_ranges_.clear(); }

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

  [[nodiscard]] auto QueryDeviceMemory() const -> GpuDeviceMemorySnapshot;

 private:
  friend class Buffer;
  friend class Texture2D;

  void                   NoteMalloc() noexcept { ++malloc_count_; }

  std::uint64_t          malloc_count_         = 0;
  std::uint64_t          free_count_           = 0;
  std::uint64_t          h2d_copy_count_       = 0;
  std::uint64_t          h2d_bytes_            = 0;
  std::uint64_t          next_resource_id_     = 1;
  std::uint64_t          next_submission_      = 0;
  std::uint64_t          in_flight_submission_ = 0;
  std::uint64_t          completed_submission_ = 0;
  bool                   fail_next_upload_     = false;
  std::vector<ByteRange> last_h2d_ranges_;
  std::vector<RectI>     last_texture_rectangles_;
};

/**
 * @brief Per-session TexturePool budget from device memory.
 *
 * Uses a quarter of total device memory, floored at 256 MiB so a preview ROI
 * still fits when the card reports a small total. Not the 64 MiB test leftover.
 */
[[nodiscard]] auto DefaultProductTextureBudgetBytes() -> std::size_t;

inline constexpr std::uint32_t kCudaDagBackendCapabilityVersion = CudaBackend::kCapabilityVersion;

}  // namespace alcedo
