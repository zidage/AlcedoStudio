//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/runtime/basic_render_device.hpp"
#include "edit/runtime/byte_range.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {

struct ExecutionPlan;
class MetalBackendImpl;

inline constexpr std::uint32_t kMetalDagBackendCapabilityVersion = 2;

struct MetalPipelineWarmup {
  const char* metallib_path = nullptr;
  const char* function_name = nullptr;
  const char* debug_label   = nullptr;
};

struct MetalLutBinding {
  void*         native      = nullptr;
  std::uint64_t resource_id = 0;
  std::uint32_t edge_size   = 0;
};

class MetalCommandContext {
 public:
  MetalCommandContext();
  ~MetalCommandContext();
  MetalCommandContext(const MetalCommandContext&)                    = delete;
  auto operator=(const MetalCommandContext&) -> MetalCommandContext& = delete;
  MetalCommandContext(MetalCommandContext&&) noexcept;
  auto               operator=(MetalCommandContext&&) noexcept -> MetalCommandContext&;

  [[nodiscard]] auto SubmissionId() const -> std::uint64_t { return submission_id_; }
  void               SetSubmissionId(std::uint64_t id) { submission_id_ = id; }
  [[nodiscard]] auto NativeCommandBuffer() const -> void*;

 private:
  friend class MetalBackend;
  friend class MetalBackendImpl;
  class Gpu;
  std::unique_ptr<Gpu> gpu_;
  std::uint64_t        submission_id_ = 0;
};

class MetalBackend {
 public:
  static constexpr std::uint32_t kCapabilityVersion = kMetalDagBackendCapabilityVersion;
  static constexpr const char*   kName              = "Metal";
  /** @brief Metal Develop allocates command-buffer-owned scratch instead of arena slabs. */
  static constexpr bool          kUsesDevelopTransientArena = false;
  static auto                    DefaultTextureBudgetBytes() -> std::size_t { return 256ull << 20; }

  class Buffer {
   public:
    Buffer() = default;
    Buffer(MetalBackend* owner, void* native, void* device_pointer, std::size_t bytes,
           std::uint64_t id);
    ~Buffer();
    Buffer(const Buffer&)                    = delete;
    auto operator=(const Buffer&) -> Buffer& = delete;
    Buffer(Buffer&& other) noexcept;
    auto               operator=(Buffer&& other) noexcept -> Buffer&;

    [[nodiscard]] auto DevicePointer() const -> void* { return ptr_; }
    [[nodiscard]] auto Native() const -> void* { return native_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }
    [[nodiscard]] auto Empty() const -> bool { return native_ == nullptr; }
    void               Reset() noexcept;

   private:
    MetalBackend* owner_       = nullptr;
    void*         native_      = nullptr;
    void*         ptr_         = nullptr;
    std::size_t   bytes_       = 0;
    std::uint64_t resource_id_ = 0;
  };

  class Texture2D {
   public:
    Texture2D() = default;
    Texture2D(MetalBackend* owner, void* native, std::size_t bytes, std::uint32_t width,
              std::uint32_t height, TextureFormat format, std::uint64_t id);
    ~Texture2D();
    Texture2D(const Texture2D&)                    = delete;
    auto operator=(const Texture2D&) -> Texture2D& = delete;
    Texture2D(Texture2D&& other) noexcept;
    auto               operator=(Texture2D&& other) noexcept -> Texture2D&;

    [[nodiscard]] auto DevicePointer() const -> void* { return native_; }
    [[nodiscard]] auto Native() const -> void* { return native_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto Width() const -> std::uint32_t { return width_; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return height_; }
    [[nodiscard]] auto Format() const -> TextureFormat { return format_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }
    void               Reset() noexcept;

   private:
    MetalBackend* owner_       = nullptr;
    void*         native_      = nullptr;
    std::size_t   bytes_       = 0;
    std::uint32_t width_       = 0;
    std::uint32_t height_      = 0;
    TextureFormat format_      = TextureFormat::R8;
    std::uint64_t resource_id_ = 0;
  };

  using Slab           = Buffer;
  using CommandContext = MetalCommandContext;

  MetalBackend();
  ~MetalBackend();
  MetalBackend(const MetalBackend&)                                  = delete;
  auto               operator=(const MetalBackend&) -> MetalBackend& = delete;

  [[nodiscard]] auto CreateBuffer(std::size_t bytes) -> Buffer;
  [[nodiscard]] auto CreateSlab(std::size_t bytes) -> Buffer;
  [[nodiscard]] auto CreateTexture2D(std::uint32_t width, std::uint32_t height,
                                     TextureFormat format) -> Texture2D;

  /**
   * @brief Create a scratch buffer owned until the recorded command buffer is finished.
   *
   * The backend owns the returned buffer. Its reference stays valid until
   * SynchronizeRecordedWork(), Wait(), or cancellation discards the recorded work.
   */
  [[nodiscard]] auto AcquireRecordedWorkScratchBuffer(std::size_t bytes) -> Buffer&;
  /** @brief Create a scratch texture with the same recorded-work lifetime as a scratch buffer. */
  [[nodiscard]] auto AcquireRecordedWorkScratchTexture(std::uint32_t width, std::uint32_t height,
                                                       TextureFormat format) -> Texture2D&;
  /** @brief Return the number of scratch buffers awaiting recorded-work completion. */
  [[nodiscard]] auto RecordedWorkScratchBufferCount() const -> std::size_t {
    return recorded_work_scratch_buffers_.size();
  }
  /** @brief Return the total bytes in scratch buffers awaiting recorded-work completion. */
  [[nodiscard]] auto RecordedWorkScratchBufferBytes() const -> std::size_t {
    std::size_t bytes = 0;
    for (const auto& buffer : recorded_work_scratch_buffers_) {
      bytes += buffer.Bytes();
    }
    return bytes;
  }
  /** @brief Return the number of scratch textures awaiting recorded-work completion. */
  [[nodiscard]] auto RecordedWorkScratchTextureCount() const -> std::size_t {
    return recorded_work_scratch_textures_.size();
  }

  void UploadBufferRange(Buffer& buffer, std::uint32_t offset, std::span<const std::byte> bytes,
                         CommandContext& command_context);
  void DownloadBufferRange(const Buffer& buffer, std::uint32_t offset, std::span<std::byte> out,
                           CommandContext& command_context);
  /**
   * @brief Copy packed host texels into a private texture through command-buffer staging.
   *
   * Each call reserves a distinct host-visible range. A later upload in the same
   * command buffer must not overwrite bytes still referenced by an encoded blit.
   */
  void UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes,
                       CommandContext& command_context);
  void CopyTexture2D(const Texture2D& src, Texture2D& dst, CommandContext& command_context);
  void UploadR8TextureRect(Texture2D& texture, RectI rectangle, std::span<const std::byte> bytes,
                           CommandContext& command_context);
  void DownloadTexture2D(const Texture2D& texture, std::span<std::byte> out,
                         CommandContext& command_context);
  void UploadDeviceMemory(void* dst, std::span<const std::byte> bytes,
                          CommandContext& command_context);
  void FillDeviceMemory(void* dst, std::size_t bytes, std::uint8_t value,
                        CommandContext& command_context);
  void CopyDeviceMemoryToBuffer(void* src, Buffer& dst, std::uint32_t dst_offset, std::size_t bytes,
                                CommandContext& command_context);
  [[nodiscard]] auto ResolveDeviceMemory(void* device_pointer, std::size_t bytes) const
      -> std::pair<void*, std::uint32_t>;

  [[nodiscard]] auto EnsureComputeCommandEncoder(CommandContext& command_context) -> void*;
  void               EndCommandEncoders(CommandContext& command_context);

  void               Submit(CommandContext& command_context);
  void               Wait(CommandContext& command_context);
  /**
   * @brief Commit and wait the current command buffer. Recorded-work scratch stays alive.
   *
   * The next encode creates a new command buffer. Call this before MPSGraph Neural
   * Engine work: `encodeToCommandBuffer` may `commitAndContinue`, so it cannot share
   * this buffer, and later passes must not create encoders on the committed original.
   */
  void CompleteCurrentCommandBuffer(CommandContext& command_context);
  /**
   * @brief Commit recorded Metal work and wait. The next encode gets a new command buffer.
   *
   * Used to finish and release Develop scratch before Geometry runs.
   */
  void SynchronizeRecordedWork(CommandContext& command_context);

  void               WarmUpPipelines(std::span<const MetalPipelineWarmup> pipelines);
  void               WarmUpPlan(const ExecutionPlan& plan);

  [[nodiscard]] auto AcquireLut(ContentKey key, std::span<const std::byte> packed_rgba,
                                std::uint32_t edge, CommandContext& command_context)
      -> MetalLutBinding;
  [[nodiscard]] auto DummyLut() -> MetalLutBinding;
  void               SetLutByteBudget(std::size_t bytes);
  /**
   * @brief Count a compute dispatch and order later kernels after its writes.
   *
   * Metal does not imply a texture or buffer barrier between dispatches in one
   * encoder. Heap resources stay tracked; this still serializes write-then-read
   * so a later Grade mix cannot observe another Grade's mask or scratch.
   */
  void NoteComputeDispatch(CommandContext& command_context);
  void SetGradeCommandTopologyHash(std::uint64_t hash) { grade_command_topology_hash_ = hash; }

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

  [[nodiscard]] auto NativeDevice() const -> void*;
  [[nodiscard]] auto NativeQueue() const -> void*;
  [[nodiscard]] auto WorkingSetBudgetBytes() const -> std::size_t;
  [[nodiscard]] auto QueryDeviceMemory() const -> GpuDeviceMemorySnapshot;
  [[nodiscard]] auto MallocCount() const -> std::uint64_t { return malloc_count_; }
  [[nodiscard]] auto FreeCount() const -> std::uint64_t { return free_count_; }
  [[nodiscard]] auto BufferCreateCount() const -> std::uint64_t { return buffer_create_count_; }
  [[nodiscard]] auto TextureCreateCount() const -> std::uint64_t { return texture_create_count_; }
  [[nodiscard]] auto HeapCreateCount() const -> std::uint64_t { return heap_create_count_; }
  [[nodiscard]] auto CommandBufferCreateCount() const -> std::uint64_t {
    return command_buffer_create_count_;
  }
  [[nodiscard]] auto PipelineCreateCount() const -> std::uint64_t;
  [[nodiscard]] auto PipelineHitCount() const -> std::uint64_t;
  [[nodiscard]] auto HostToDeviceCopyCount() const -> std::uint64_t { return h2d_copy_count_; }
  [[nodiscard]] auto HostToDeviceBytes() const -> std::uint64_t { return h2d_bytes_; }
  [[nodiscard]] auto LastHostToDeviceRanges() const -> const std::vector<ByteRange>& {
    return last_h2d_ranges_;
  }
  [[nodiscard]] auto LastTextureRectangles() const -> const std::vector<RectI>& {
    return last_texture_rectangles_;
  }
  [[nodiscard]] auto ComputeDispatchCount() const -> std::uint64_t {
    return compute_dispatch_count_;
  }
  [[nodiscard]] auto LutUploadBytes() const -> std::uint64_t { return lut_upload_bytes_; }
  [[nodiscard]] auto LastLutResourceId() const -> std::uint64_t { return last_lut_resource_id_; }
  [[nodiscard]] auto GradeCommandTopologyHash() const -> std::uint64_t {
    return grade_command_topology_hash_;
  }

 private:
  friend class Buffer;
  friend class Texture2D;
  friend class MetalBackendImpl;
  class Gpu;
  void NoteMalloc() noexcept { ++malloc_count_; }
  void NoteBufferCreate() noexcept {
    ++buffer_create_count_;
    NoteMalloc();
  }
  void NoteTextureCreate() noexcept {
    ++texture_create_count_;
    NoteMalloc();
  }
  void NoteHeapCreate() noexcept { ++heap_create_count_; }
  void ReleaseRecordedWorkScratchResources() noexcept {
    recorded_work_scratch_buffers_.clear();
    recorded_work_scratch_textures_.clear();
  }

  std::unique_ptr<Gpu>  gpu_{};
  std::deque<Buffer>    recorded_work_scratch_buffers_;
  std::deque<Texture2D> recorded_work_scratch_textures_;
  std::uint64_t          malloc_count_                = 0;
  std::uint64_t          free_count_                  = 0;
  std::uint64_t          buffer_create_count_         = 0;
  std::uint64_t          texture_create_count_        = 0;
  std::uint64_t          heap_create_count_           = 0;
  std::uint64_t          command_buffer_create_count_ = 0;
  std::uint64_t          h2d_copy_count_              = 0;
  std::uint64_t          h2d_bytes_                   = 0;
  std::uint64_t          next_resource_id_            = 1;
  std::uint64_t          next_submission_             = 0;
  std::uint64_t          in_flight_submission_        = 0;
  std::uint64_t          completed_submission_        = 0;
  bool                   fail_next_upload_            = false;
  std::vector<ByteRange> last_h2d_ranges_;
  std::vector<RectI>     last_texture_rectangles_;
  std::uint64_t          compute_dispatch_count_      = 0;
  std::uint64_t          lut_upload_bytes_            = 0;
  std::uint64_t          last_lut_resource_id_        = 0;
  std::uint64_t          grade_command_topology_hash_ = 0;
  std::size_t            lut_byte_budget_             = 64ull << 20;
  std::size_t            lut_cache_bytes_             = 0;
  struct LutCacheEntry {
    ContentKey    key;
    Buffer        buffer;
    std::uint32_t edge_size = 0;
    std::size_t   bytes     = 0;
  };
  std::vector<LutCacheEntry> lut_cache_;
  Buffer                     dummy_lut_;
};

[[nodiscard]] auto BindSystemDefaultMetalPresentationDevice() -> void*;
[[nodiscard]] auto MetalPresentationDeviceHandle() -> void*;

using MetalRenderDevice    = BasicRenderDevice<MetalBackend>;
using MetalRenderWorkspace = BasicRenderWorkspace<MetalBackend>;

}  // namespace alcedo
