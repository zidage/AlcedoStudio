//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>

#include <cstddef>
#include <cstdint>
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

inline constexpr std::uint32_t kOpenClDagBackendCapabilityVersion = 1;

struct OpenClLutBinding {
  cl_mem        native      = nullptr;
  std::uint64_t resource_id = 0;
  std::uint32_t edge_size   = 0;
};

/**
 * @brief In-order product-queue encode state for one OpenCL render.
 *
 * Holds the current submission id and retained events for this render. Does not
 * own a command queue. Not thread-safe.
 */
class OpenClCommandContext {
 public:
  OpenClCommandContext() = default;
  ~OpenClCommandContext();

  OpenClCommandContext(const OpenClCommandContext&)                    = delete;
  auto operator=(const OpenClCommandContext&) -> OpenClCommandContext& = delete;
  OpenClCommandContext(OpenClCommandContext&& other) noexcept;
  auto operator=(OpenClCommandContext&& other) noexcept -> OpenClCommandContext&;

  [[nodiscard]] auto SubmissionId() const -> std::uint64_t { return submission_id_; }
  void               SetSubmissionId(std::uint64_t id) { submission_id_ = id; }

  /**
   * @brief Take ownership of an enqueue-produced event (refcount 1).
   * @param event Event to release when this render waits or is cancelled.
   */
  void TrackEvent(cl_event event);

  [[nodiscard]] auto TrackedEventCount() const -> std::size_t { return live_events_.size(); }
  [[nodiscard]] auto FinalEvent() const -> cl_event { return final_event_; }

  /**
   * @brief Release every tracked event. Does not wait.
   * @return Number of events released.
   */
  auto ReleaseTrackedEvents() noexcept -> std::size_t;

 private:
  friend class OpenClBackend;

  std::vector<cl_event> live_events_;
  cl_event              final_event_   = nullptr;
  std::uint64_t         submission_id_ = 0;
};

/**
 * @brief OpenCL 1.2 resource factory for the DAG workspace.
 *
 * Uses OpenClContext::Instance() device, context, and product queue. Move-only
 * Buffer and Texture2D wrappers own cl_mem. One in-flight submission. Not thread-safe.
 */
class OpenClBackend {
 public:
  static constexpr std::uint32_t kCapabilityVersion = kOpenClDagBackendCapabilityVersion;
  static constexpr const char*   kName              = "OpenCL";

  static auto DefaultTextureBudgetBytes() -> std::size_t;

  class Buffer {
   public:
    Buffer() = default;
    Buffer(OpenClBackend* owner, cl_mem native, void* device_pointer, std::size_t bytes,
           std::uint64_t id);
    ~Buffer();
    Buffer(const Buffer&)                    = delete;
    auto operator=(const Buffer&) -> Buffer& = delete;
    Buffer(Buffer&& other) noexcept;
    auto               operator=(Buffer&& other) noexcept -> Buffer&;

    [[nodiscard]] auto DevicePointer() const -> void* { return ptr_; }
    [[nodiscard]] auto Native() const -> cl_mem { return native_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }
    [[nodiscard]] auto Empty() const -> bool { return native_ == nullptr; }
    void               Reset() noexcept;

   private:
    OpenClBackend* owner_       = nullptr;
    cl_mem         native_      = nullptr;
    void*          ptr_         = nullptr;
    std::size_t    bytes_       = 0;
    std::uint64_t  resource_id_ = 0;
  };

  class Texture2D {
   public:
    Texture2D() = default;
    Texture2D(OpenClBackend* owner, cl_mem native, std::size_t bytes, std::uint32_t width,
              std::uint32_t height, TextureFormat format, std::uint64_t id);
    ~Texture2D();
    Texture2D(const Texture2D&)                    = delete;
    auto operator=(const Texture2D&) -> Texture2D& = delete;
    Texture2D(Texture2D&& other) noexcept;
    auto               operator=(Texture2D&& other) noexcept -> Texture2D&;

    [[nodiscard]] auto DevicePointer() const -> void* { return native_; }
    [[nodiscard]] auto Native() const -> cl_mem { return native_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto Width() const -> std::uint32_t { return width_; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return height_; }
    [[nodiscard]] auto Format() const -> TextureFormat { return format_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }
    [[nodiscard]] auto Empty() const -> bool { return native_ == nullptr; }
    void               Reset() noexcept;

   private:
    OpenClBackend* owner_       = nullptr;
    cl_mem         native_      = nullptr;
    std::size_t    bytes_       = 0;
    std::uint32_t  width_       = 0;
    std::uint32_t  height_      = 0;
    TextureFormat  format_      = TextureFormat::R8;
    std::uint64_t  resource_id_ = 0;
  };

  using Slab           = Buffer;
  using CommandContext = OpenClCommandContext;

  OpenClBackend();
  ~OpenClBackend();
  OpenClBackend(const OpenClBackend&)                                  = delete;
  auto               operator=(const OpenClBackend&) -> OpenClBackend& = delete;

  [[nodiscard]] auto CreateBuffer(std::size_t bytes) -> Buffer;
  [[nodiscard]] auto CreateSlab(std::size_t bytes) -> Buffer { return CreateBuffer(bytes); }
  /**
   * @brief Largest legal `clCreateBuffer` size for this device.
   *
   * Taken from `CL_DEVICE_MAX_MEM_ALLOC_SIZE` when the OpenCL context is created,
   * aligned down to 256 bytes for the transient arena. Tests may override with
   * @ref SetMaxSlabBytes; 0 restores the device-reported value.
   */
  [[nodiscard]] auto MaxSlabBytes() const -> std::size_t;
  [[nodiscard]] auto MaxTransientBytes() const -> std::size_t;
  /**
   * @brief Override @ref MaxSlabBytes. Zero restores the device-reported cap.
   * @param bytes Slab cap in bytes, or 0 for the device default.
   */
  void SetMaxSlabBytes(std::size_t bytes);
  [[nodiscard]] auto CreateTexture2D(std::uint32_t width, std::uint32_t height,
                                     TextureFormat format) -> Texture2D;

  void UploadBufferRange(Buffer& buffer, std::uint32_t offset, std::span<const std::byte> bytes,
                         CommandContext& command_context);
  void DownloadBufferRange(const Buffer& buffer, std::uint32_t offset, std::span<std::byte> out,
                           CommandContext& command_context);
  void UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes,
                       CommandContext& command_context);
  void CopyTexture2D(const Texture2D& src, Texture2D& dst, CommandContext& command_context);
  void UploadR8TextureRect(Texture2D& texture, RectI rectangle, std::span<const std::byte> bytes,
                           CommandContext& command_context);
  void DownloadTexture2D(const Texture2D& texture, std::span<std::byte> out,
                         CommandContext& command_context);
  void CopyBufferToImage(const Buffer& src, std::uint32_t src_offset, Texture2D& dst,
                         CommandContext& command_context);
  void CopyImageToBuffer(const Texture2D& src, Buffer& dst, std::uint32_t dst_offset,
                         CommandContext& command_context);
  void CopyImageToDeviceMemory(const Texture2D& src, void* dst, std::size_t bytes,
                               CommandContext& command_context);
  void CopyDeviceMemoryToImage(void* src, Texture2D& dst, CommandContext& command_context);
  void UploadDeviceMemory(void* dst, std::span<const std::byte> bytes,
                          CommandContext& command_context);
  void FillDeviceMemory(void* dst, std::size_t bytes, std::uint8_t value,
                        CommandContext& command_context);
  void CopyDeviceMemoryToBuffer(void* src, Buffer& dst, std::uint32_t dst_offset, std::size_t bytes,
                                CommandContext& command_context);
  [[nodiscard]] auto ResolveDeviceMemory(void* device_pointer, std::size_t bytes) const
      -> std::pair<cl_mem, std::uint32_t>;

  /**
   * @brief Take ownership of an enqueue event (refcount 1) and count it.
   * @param command_context Current render command context.
   * @param event Event produced by clEnqueue*; null is ignored.
   */
  void TrackKernelEvent(CommandContext& command_context, cl_event event);

  void Submit(CommandContext& command_context);
  /** @brief Append and flush presentation work to the current product submission. */
  void FinalizePresentation(CommandContext& command_context);
  /** @brief Release backend-owned neural activation workspace after queue completion. */
  void ReleaseNeuralDemosaicWorkspace();
  void Wait(CommandContext& command_context);
  /**
   * @brief Flush and wait for commands already on the product queue.
   *
   * Used to free Develop scratch after SensorDevelop. Flushes before
   * `clWaitForEvents` so a large in-order dispatch cannot stall forever with
   * work still sitting in the host queue. Does not replace product Submit/Wait
   * with `clFinish`.
   */
  void SynchronizeRecordedWork(CommandContext& command_context);

  void WarmUpPlan(const ExecutionPlan& plan);

  [[nodiscard]] auto AcquireLut(ContentKey key, std::span<const std::byte> packed_rgba,
                                std::uint32_t edge, CommandContext& command_context)
      -> OpenClLutBinding;
  [[nodiscard]] auto DummyLut() -> OpenClLutBinding;
  void               SetLutByteBudget(std::size_t bytes);
  /** @brief Release busy markers belonging to an encode cancelled before submission. */
  void               ReleaseUnsubmittedResourceUses() noexcept;
  void               SetGradeCommandTopologyHash(std::uint64_t hash) {
    grade_command_topology_hash_ = hash;
  }

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

  [[nodiscard]] auto NativeDevice() const -> cl_device_id { return device_; }
  [[nodiscard]] auto NativeContext() const -> cl_context { return context_; }
  [[nodiscard]] auto NativeQueue() const -> cl_command_queue { return queue_; }
  [[nodiscard]] auto WorkingSetBudgetBytes() const -> std::size_t;
  [[nodiscard]] auto QueryDeviceMemory() const -> GpuDeviceMemorySnapshot;
  [[nodiscard]] auto MallocCount() const -> std::uint64_t { return malloc_count_; }
  [[nodiscard]] auto FreeCount() const -> std::uint64_t { return free_count_; }
  [[nodiscard]] auto BufferCreateCount() const -> std::uint64_t { return buffer_create_count_; }
  [[nodiscard]] auto TextureCreateCount() const -> std::uint64_t { return texture_create_count_; }
  [[nodiscard]] auto ProgramBuildCount() const -> std::uint64_t;
  [[nodiscard]] auto KernelCreateCount() const -> std::uint64_t;
  [[nodiscard]] auto KernelHitCount() const -> std::uint64_t;
  [[nodiscard]] auto EventCreateCount() const -> std::uint64_t { return event_create_count_; }
  [[nodiscard]] auto EventReleaseCount() const -> std::uint64_t { return event_release_count_; }
  [[nodiscard]] auto FlushCount() const -> std::uint64_t { return flush_count_; }
  [[nodiscard]] auto WaitCount() const -> std::uint64_t { return wait_count_; }
  [[nodiscard]] auto HostToDeviceCopyCount() const -> std::uint64_t { return h2d_copy_count_; }
  [[nodiscard]] auto HostToDeviceBytes() const -> std::uint64_t { return h2d_bytes_; }
  [[nodiscard]] auto LastHostToDeviceRanges() const -> const std::vector<ByteRange>& {
    return last_h2d_ranges_;
  }
  [[nodiscard]] auto LastTextureRectangles() const -> const std::vector<RectI>& {
    return last_texture_rectangles_;
  }
  [[nodiscard]] auto LutUploadBytes() const -> std::uint64_t { return lut_upload_bytes_; }
  [[nodiscard]] auto LastLutResourceId() const -> std::uint64_t { return last_lut_resource_id_; }
  [[nodiscard]] auto GradeCommandTopologyHash() const -> std::uint64_t {
    return grade_command_topology_hash_;
  }

 private:
  friend class Buffer;
  friend class Texture2D;

  struct LiveBuffer {
    cl_mem        native      = nullptr;
    std::uint64_t gpu_address = 0;
    std::size_t   bytes       = 0;
  };

  void NoteMalloc() noexcept { ++malloc_count_; }
  void NoteBufferCreate() noexcept {
    ++buffer_create_count_;
    NoteMalloc();
  }
  void NoteTextureCreate() noexcept {
    ++texture_create_count_;
    NoteMalloc();
  }
  void NoteEventCreate() noexcept { ++event_create_count_; }
  void NoteEventRelease(std::size_t count = 1) noexcept { event_release_count_ += count; }
  void UnregisterBuffer(cl_mem native) noexcept;
  void TrackEnqueueEvent(CommandContext& command_context, cl_event event);
  auto EnqueueMarker(CommandContext& command_context) -> cl_event;
  void FlushQueue();

  cl_device_id           device_  = nullptr;
  cl_context             context_ = nullptr;
  cl_command_queue       queue_   = nullptr;
  std::vector<LiveBuffer> live_buffers_;
  std::uint64_t          next_virtual_address_     = 0x100000000ull;
  std::uint64_t          malloc_count_             = 0;
  std::uint64_t          free_count_               = 0;
  std::uint64_t          buffer_create_count_      = 0;
  std::uint64_t          texture_create_count_     = 0;
  std::uint64_t          event_create_count_       = 0;
  std::uint64_t          event_release_count_      = 0;
  std::uint64_t          flush_count_              = 0;
  std::uint64_t          wait_count_               = 0;
  std::uint64_t          h2d_copy_count_           = 0;
  std::uint64_t          h2d_bytes_                = 0;
  std::uint64_t          next_resource_id_         = 1;
  std::uint64_t          next_submission_          = 0;
  std::uint64_t          in_flight_submission_     = 0;
  std::uint64_t          completed_submission_     = 0;
  std::uint64_t          kernel_create_baseline_   = 0;
  std::uint64_t          kernel_hit_baseline_      = 0;
  std::uint64_t          program_build_baseline_   = 0;
  bool                   fail_next_upload_         = false;
  std::vector<ByteRange> last_h2d_ranges_;
  std::vector<RectI>     last_texture_rectangles_;
  std::uint64_t          lut_upload_bytes_     = 0;
  std::uint64_t          last_lut_resource_id_ = 0;
  std::uint64_t          grade_command_topology_hash_ = 0;
  std::size_t            lut_byte_budget_      = 64ull << 20;
  std::size_t            lut_cache_bytes_      = 0;
  std::uint64_t          lut_lru_clock_        = 0;
  struct LutCacheEntry {
    ContentKey    key;
    Buffer        buffer;
    std::uint32_t edge_size = 0;
    std::size_t   bytes     = 0;
    std::uint64_t lru_tick  = 0;
    std::uint64_t last_used_submission = 0;
  };
  std::vector<LutCacheEntry> lut_cache_;
  Buffer                     dummy_lut_;
  std::size_t                max_slab_bytes_device_   = 0;
  std::size_t                max_slab_bytes_override_ = 0;
  std::size_t                max_image_width_         = 0;
  std::size_t                max_image_height_        = 0;
};

using OpenClRenderDevice    = BasicRenderDevice<OpenClBackend>;
using OpenClRenderWorkspace = BasicRenderWorkspace<OpenClBackend>;

}  // namespace alcedo

#endif  // HAVE_OPENCL
