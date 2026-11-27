//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

// clang-format off
#include <volk.h>
#include <vma/vk_mem_alloc.h>
// clang-format on

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "edit/geometry/types.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/byte_range.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_command_context.hpp"
#include "gpu/gpu_device_memory.hpp"
#include "vulkan/vulkan_context.hpp"

namespace alcedo {

inline constexpr std::uint32_t kVulkanDagBackendCapabilityVersion = 1;

/**
 * @brief A 3D LUT of the LUT cache of `VulkanBackend` (phase VK8): the Vulkan form of
 * `OpenClLutBinding`.
 *
 * @ref address is the device address of the RGBA32F voxels (X fastest). It stays valid for each
 * recording that uses it: an entry that leaves the cache is given back by the lifetime rule of
 * `VulkanBackend`. @ref edge_size 0 is the dummy LUT (a 16-byte buffer that no kernel reads).
 */
struct VulkanLutBinding {
  VkDeviceAddress address     = 0;
  std::uint64_t   resource_id = 0;
  std::uint32_t   edge_size   = 0;
};

/**
 * @brief The Vulkan resource factory of the render workspace (plan section 4.1, phase VK2).
 *
 * Satisfies the concept `RenderBackend`. `BasicRenderDevice<VulkanBackend>` and
 * `BasicRenderWorkspace<VulkanBackend>` use it as they use the OpenCL backend.
 *
 * Resources:
 * - `Buffer`: a `VkBuffer` with a device address (`DevicePointer()` is the address). A kernel
 *   reads a buffer through its address in the push constants (plan section 4.2).
 * - `Texture2D`: a `VkImage` with optimal tiling and one `VkImageView`, in
 *   `VK_IMAGE_LAYOUT_GENERAL` after its first use. Storage, sampled, and transfer usage.
 * - `SceneWorkImage`: an RGBA32F row-major buffer, as in the OpenCL backend.
 *
 * Memory (plan section 4.2): each allocation is in the device-local heap through VMA. Before each
 * allocation the backend reads the `VK_EXT_memory_budget` values of the heap. An allocation fails
 * with its size and the limit when the heap usage of the process plus the size is above the
 * budget minus @ref kBudgetReserveBytes, or when the live bytes of this backend plus the size are
 * above the application limit (@ref SetMemoryLimitBytes). The allocations have the memory
 * priority @ref kWorkingSetPriority.
 *
 * Lifetime of a released resource: `Reset` of a buffer or a texture gives its Vulkan objects back
 * to the backend. The backend destroys them when every batch that can use them is complete: the
 * batches that are submitted, and the open recording when one exists. So a resource that an
 * unfinished submission uses stays alive until its fence signals.
 *
 * Submission: transfers record into the command buffer of the command context. `Submit` submits
 * the recording as one batch. `Wait` submits a recording that is still open, waits for every
 * batch of the context, and completes the render submission. A download records its copy, submits
 * the recording, and waits for it, so the host has the bytes when it returns.
 *
 * Preconditions: `VulkanContext` exists from the construction to the destruction. Each command
 * context that recorded commands for this backend is destroyed, or waited, before the backend.
 * Not thread-safe; one render thread uses it.
 */
class VulkanBackend {
 public:
  static constexpr std::uint32_t kCapabilityVersion    = kVulkanDagBackendCapabilityVersion;
  static constexpr const char*   kName                 = "Vulkan";
  /// Plan section 4.2 (VX5): the budget of the heap minus this reserve is the memory limit.
  static constexpr std::size_t   kBudgetReserveBytes   = std::size_t{512} << 20;
  /// `VK_EXT_memory_priority` value of the allocations of this backend (plan section 4.2, VX5).
  static constexpr float         kWorkingSetPriority   = 0.8F;
  /// `VK_EXT_memory_priority` value of the LUT cache entries (plan section 4.2, VX5: Windows
  /// evicts the low priority first).
  static constexpr float         kCachePriority        = 0.2F;
  /// The default byte budget of the LUT cache (the OpenCL value).
  static constexpr std::size_t   kDefaultLutByteBudget = std::size_t{64} << 20;

  class Buffer {
   public:
    Buffer() = default;
    ~Buffer();
    Buffer(const Buffer&)                    = delete;
    auto operator=(const Buffer&) -> Buffer& = delete;
    Buffer(Buffer&& other) noexcept;
    auto               operator=(Buffer&& other) noexcept -> Buffer&;

    /// The device address as a pointer. Not a host address.
    [[nodiscard]] auto DevicePointer() const -> void* {
      return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address_));
    }
    [[nodiscard]] auto DeviceAddress() const -> VkDeviceAddress { return address_; }
    [[nodiscard]] auto Native() const -> VkBuffer { return buffer_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }
    [[nodiscard]] auto Empty() const -> bool { return buffer_ == VK_NULL_HANDLE; }
    /// Gives the buffer back to the backend; see the lifetime rule of `VulkanBackend`.
    void               Reset() noexcept;

   private:
    friend class VulkanBackend;
    Buffer(VulkanBackend* owner, VkBuffer buffer, VmaAllocation allocation, VkDeviceAddress address,
           std::size_t bytes, std::uint64_t resource_id);

    VulkanBackend*  owner_       = nullptr;
    VkBuffer        buffer_      = VK_NULL_HANDLE;
    VmaAllocation   allocation_  = VK_NULL_HANDLE;
    VkDeviceAddress address_     = 0;
    std::size_t     bytes_       = 0;
    std::uint64_t   resource_id_ = 0;
  };

  class Texture2D {
   public:
    Texture2D() = default;
    ~Texture2D();
    Texture2D(const Texture2D&)                    = delete;
    auto operator=(const Texture2D&) -> Texture2D& = delete;
    Texture2D(Texture2D&& other) noexcept;
    auto               operator=(Texture2D&& other) noexcept -> Texture2D&;

    [[nodiscard]] auto Native() const -> VkImage { return image_; }
    [[nodiscard]] auto View() const -> VkImageView { return view_; }
    [[nodiscard]] auto Bytes() const -> std::size_t { return bytes_; }
    [[nodiscard]] auto Width() const -> std::uint32_t { return width_; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return height_; }
    [[nodiscard]] auto Format() const -> TextureFormat { return format_; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return resource_id_; }
    [[nodiscard]] auto Empty() const -> bool { return image_ == VK_NULL_HANDLE; }
    /// Gives the image back to the backend; see the lifetime rule of `VulkanBackend`.
    void               Reset() noexcept;

   private:
    friend class VulkanBackend;
    Texture2D(VulkanBackend* owner, VkImage image, VkImageView view, VmaAllocation allocation,
              std::size_t bytes, std::uint32_t width, std::uint32_t height, TextureFormat format,
              std::uint64_t resource_id);

    VulkanBackend* owner_       = nullptr;
    VkImage        image_       = VK_NULL_HANDLE;
    VkImageView    view_        = VK_NULL_HANDLE;
    VmaAllocation  allocation_  = VK_NULL_HANDLE;
    std::size_t    bytes_       = 0;
    std::uint32_t  width_       = 0;
    std::uint32_t  height_      = 0;
    TextureFormat  format_      = TextureFormat::R8;
    std::uint64_t  resource_id_ = 0;
  };

  /// RGBA32F row-major buffer used only as a scene-work member (as in the OpenCL backend).
  class SceneWorkImage {
   public:
    SceneWorkImage() = default;
    SceneWorkImage(Buffer buffer, std::uint32_t width, std::uint32_t height)
        : buffer_(std::move(buffer)), width_(width), height_(height) {}
    SceneWorkImage(const SceneWorkImage&)                             = delete;
    auto operator=(const SceneWorkImage&) -> SceneWorkImage&          = delete;
    SceneWorkImage(SceneWorkImage&&)                                  = default;
    auto               operator=(SceneWorkImage&&) -> SceneWorkImage& = default;

    [[nodiscard]] auto Native() const -> VkBuffer { return buffer_.Native(); }
    [[nodiscard]] auto DevicePointer() const -> void* { return buffer_.DevicePointer(); }
    [[nodiscard]] auto Bytes() const -> std::size_t { return buffer_.Bytes(); }
    [[nodiscard]] auto Width() const -> std::uint32_t { return width_; }
    [[nodiscard]] auto Height() const -> std::uint32_t { return height_; }
    [[nodiscard]] auto Format() const -> TextureFormat { return TextureFormat::Rgba32f; }
    [[nodiscard]] auto ResourceId() const -> std::uint64_t { return buffer_.ResourceId(); }
    [[nodiscard]] auto Empty() const -> bool { return buffer_.Empty(); }
    [[nodiscard]] auto Storage() -> Buffer& { return buffer_; }
    [[nodiscard]] auto Storage() const -> const Buffer& { return buffer_; }

    void               Reset() noexcept {
      buffer_.Reset();
      width_  = 0;
      height_ = 0;
    }

   private:
    Buffer        buffer_{};
    std::uint32_t width_  = 0;
    std::uint32_t height_ = 0;
  };

  using Slab           = Buffer;
  using CommandContext = VulkanCommandContext;

  /// @throws std::logic_error Without a `VulkanContext`.
  VulkanBackend();
  /// Destroys the resources that are given back. See the preconditions of the class.
  ~VulkanBackend();
  VulkanBackend(const VulkanBackend&)                                  = delete;
  auto               operator=(const VulkanBackend&) -> VulkanBackend& = delete;

  // ---- Resources --------------------------------------------------------------------------

  /**
   * @brief A device-local buffer of @p bytes with a device address. Zero gives an empty buffer.
   * @throws std::runtime_error With the size and the limit when the size is above
   *         @ref MaxSlabBytes or above the memory limit, or a `VulkanError` from VMA.
   */
  [[nodiscard]] auto CreateBuffer(std::size_t bytes) -> Buffer;
  [[nodiscard]] auto CreateSlab(std::size_t bytes) -> Buffer { return CreateBuffer(bytes); }
  /// Vulkan 1.3 `maxBufferSize` aligned down to 256 bytes, or the value of @ref SetMaxSlabBytes.
  [[nodiscard]] auto MaxSlabBytes() const -> std::size_t;
  /// Three quarters of the device-local heap: the sum limit of the transient arena.
  [[nodiscard]] auto MaxTransientBytes() const -> std::size_t;
  /// Test hook: replaces @ref MaxSlabBytes. Zero restores the device value.
  void               SetMaxSlabBytes(std::size_t bytes) { max_slab_bytes_override_ = bytes; }
  /**
   * @brief A device-local image of @p format. A zero size gives an empty texture.
   * @throws std::runtime_error With the size and the limit when the image is larger than
   *         `maxImageDimension2D` or above the memory limit, with the format name when the device
   *         has no storage and transfer support for it, or a `VulkanError` from VMA.
   */
  [[nodiscard]] auto CreateTexture2D(std::uint32_t width, std::uint32_t height,
                                     TextureFormat format) -> Texture2D;
  /// One RGBA32F scene-work member as a row-major buffer.
  [[nodiscard]] auto CreateSceneWorkImage(std::uint32_t width, std::uint32_t height)
      -> SceneWorkImage;

  // ---- Transfers (recorded into the command context) ---------------------------------------

  /// Records a copy of @p bytes to @p buffer at @p offset. The bytes are copied at the call.
  /// @throws std::runtime_error For a range outside the buffer.
  void UploadBufferRange(Buffer& buffer, std::uint32_t offset, std::span<const std::byte> bytes,
                         CommandContext& command_context);
  /**
   * @brief Records a fill of @p bytes of @p buffer at @p offset with the 32-bit @p value
   * (`vkCmdFillBuffer`): the Vulkan form of `OpenClBackend::FillDeviceMemory` (phase VK5).
   * @throws std::runtime_error For a range outside the buffer, or an offset or a size that is not
   *         a multiple of 4. Nothing is recorded.
   */
  void FillBufferRange(Buffer& buffer, std::uint32_t offset, std::size_t bytes, std::uint32_t value,
                       CommandContext& command_context);
  /// Copies the range of @p buffer to @p out. Submits the recording and waits for it.
  void DownloadBufferRange(const Buffer& buffer, std::uint32_t offset, std::span<std::byte> out,
                           CommandContext& command_context);
  /// Records a copy of the packed pixels @p bytes (width * height * texel bytes) to @p texture.
  void UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes,
                       CommandContext& command_context);
  /// Records a copy of the packed R8 pixels of @p rectangle to @p texture.
  /// @throws std::runtime_error For a texture that is not R8 or a rectangle outside it.
  void UploadR8TextureRect(Texture2D& texture, RectI rectangle, std::span<const std::byte> bytes,
                           CommandContext& command_context);
  /// Copies the packed pixels of @p texture to @p out. Submits the recording and waits for it.
  void DownloadTexture2D(const Texture2D& texture, std::span<std::byte> out,
                         CommandContext& command_context);
  /// Records a copy of @p src to @p dst (same size and format).
  void CopyTexture2D(const Texture2D& src, Texture2D& dst, CommandContext& command_context);
  /**
   * @brief Records a copy of @p rectangle of @p src to all of @p dst: the Vulkan form of
   * `OpenClImage::CropTo` (phase VK5).
   * @throws std::runtime_error For an empty texture, two textures of different formats, a
   *         rectangle that is empty or not inside @p src, or a @p dst that is not the size of
   *         @p rectangle. Nothing is recorded.
   */
  void CopyTexture2DRegion(const Texture2D& src, RectI rectangle, Texture2D& dst,
                           CommandContext& command_context);
  /// Records a copy of packed pixels from @p src at @p src_offset to all of @p dst.
  void CopyBufferToImage(const Buffer& src, std::uint32_t src_offset, Texture2D& dst,
                         CommandContext& command_context);
  /// Records a copy of all of @p src as packed pixels to @p dst at @p dst_offset.
  void CopyImageToBuffer(const Texture2D& src, Buffer& dst, std::uint32_t dst_offset,
                         CommandContext& command_context);

  /**
   * @brief The AI Mask Feather and Expand of the workspace coverage cache (SM8.3).
   *
   * The Vulkan kernels of this work come with phase VK11. Until then the call fails.
   * @throws std::logic_error Always, with the phase that adds the kernels.
   */
  void RecordAiMaskEdge(const AiMaskEdgeDispatch&            dispatch,
                        const AiMaskEdgeTextures<Texture2D>& textures,
                        CommandContext&                      command_context);

  // ---- LUT cache and pass resources (phase VK8) ---------------------------------------------

  /**
   * @brief The device buffer of the 3D LUT @p packed_rgba with @p edge voxels on each axis, from
   * the LUT cache by content key: the Vulkan form of `OpenClBackend::AcquireLut`.
   *
   * A hit (same @p key and @p edge) records nothing and returns the entry. A miss creates a buffer
   * with the memory priority @ref kCachePriority, records the upload, and adds the entry. Before
   * the miss allocates, the least recently used entries leave the cache until the new entry fits
   * in the byte budget (@ref SetLutByteBudget). An entry that leaves the cache is given back by the
   * lifetime rule of this class, so a recording that uses it stays valid.
   *
   * @return @ref DummyLut for an empty key, an edge below 2, or no bytes.
   * @throws std::runtime_error From the allocation or the upload; the entry is not added.
   */
  [[nodiscard]] auto AcquireLut(ContentKey key, std::span<const std::byte> packed_rgba,
                                std::uint32_t edge, CommandContext& command_context)
      -> VulkanLutBinding;
  /// A 16-byte buffer for a kernel argument that no kernel reads (edge 0), created on first use.
  [[nodiscard]] auto DummyLut() -> VulkanLutBinding;
  /// The byte budget of the LUT cache; zero removes the limit. Removes the least recently used
  /// entries above a lower budget at once.
  void               SetLutByteBudget(std::size_t bytes);
  [[nodiscard]] auto LutCacheBytes() const -> std::size_t { return lut_cache_bytes_; }
  [[nodiscard]] auto LutCacheEntryCount() const -> std::size_t { return lut_cache_.size(); }
  /// Bytes that @ref AcquireLut uploaded since @ref ResetCounters.
  [[nodiscard]] auto LutUploadBytes() const -> std::uint64_t { return lut_upload_bytes_; }
  [[nodiscard]] auto LastLutResourceId() const -> std::uint64_t { return last_lut_resource_id_; }

  /**
   * @brief A 1x1 storage image of @p format for a binding that a kernel does not read, created on
   * first use: the Vulkan form of the OpenCL dummy scene images (phase VK8). One image fills each
   * unused binding of its format; its content is undefined.
   * @throws std::invalid_argument For a format other than RGBA32F and R8.
   */
  [[nodiscard]] auto DummyStorageImage(TextureFormat format) -> const Texture2D&;

  /// The hash of the fused command offsets of the last Grade that uploaded them (the OpenCL and
  /// Metal rule of `UploadFusedCommands`).
  [[nodiscard]] auto GradeCommandTopologyHash() const -> std::uint64_t {
    return grade_command_topology_hash_;
  }
  void SetGradeCommandTopologyHash(std::uint64_t hash) { grade_command_topology_hash_ = hash; }

  // ---- Recording support of the pass encoders ----------------------------------------------

  /**
   * @brief The command buffer of the open recording of @p command_context for one command.
   *
   * Opens a recording when none is open. Records a memory barrier first when the recording is new
   * or an earlier command of it wrote memory. After a command that writes, the caller calls
   * `VulkanCommandContext::NoteWrite`.
   */
  [[nodiscard]] auto BeginCommand(CommandContext& command_context) -> VkCommandBuffer;
  /// Records the move of @p texture to `VK_IMAGE_LAYOUT_GENERAL` when it is its first use.
  /// @pre `BeginCommand` opened the recording of @p command_context.
  void               PrepareImage(const Texture2D& texture, CommandContext& command_context);

  // ---- Submission --------------------------------------------------------------------------

  /// Submits the open recording as one batch; the render submission is then in flight.
  void               Submit(CommandContext& command_context);
  /// Submits an open recording, waits for every batch of @p command_context, and completes the
  /// render submission in flight. Destroys the resources that no batch uses now.
  void               Wait(CommandContext& command_context);
  /// Submits an open recording and waits for every batch of @p command_context, without a change
  /// of the render submission in flight: the render records on after the call (phase VK6, the
  /// Develop scratch rule of `PlanExecutor`). Destroys the resources that no batch uses now.
  void               SynchronizeRecordedWork(CommandContext& command_context);

  [[nodiscard]] auto HasInFlightSubmission() const -> bool { return in_flight_submission_ != 0; }
  [[nodiscard]] auto CompletedSubmission() const -> std::uint64_t { return completed_submission_; }
  [[nodiscard]] auto IsResourceBusy(std::uint64_t submitted_on) const -> bool {
    return submitted_on != 0 && submitted_on > completed_submission_;
  }
  [[nodiscard]] auto NextSubmissionId() -> std::uint64_t { return ++next_submission_; }

  // ---- Memory ------------------------------------------------------------------------------

  /// The application memory limit of this backend; zero removes it.
  void               SetMemoryLimitBytes(std::size_t bytes) { memory_limit_bytes_ = bytes; }
  /// Budget and usage of the device-local heap (`VK_EXT_memory_budget`), as free and total bytes.
  [[nodiscard]] auto QueryDeviceMemory() const -> GpuDeviceMemorySnapshot;
  [[nodiscard]] auto AllocationTotals() const -> GpuBackendAllocationTotals {
    return {.live_bytes = live_bytes_, .live_allocations = live_allocations_};
  }
  /// Resources that are given back and wait for their batches.
  [[nodiscard]] auto RetiredResourceCount() const -> std::size_t { return retired_.size(); }

  // ---- Counters and test hooks -------------------------------------------------------------

  void               NoteHostToDeviceBegin() { last_h2d_ranges_.clear(); }
  void               ResetCounters();
  /// Test hook: let @p uploads_to_pass more uploads run, then fail the next one.
  void FailUploadAfter(std::uint32_t uploads_to_pass) { uploads_before_failure_ = uploads_to_pass; }
  void FailNextUpload() { FailUploadAfter(0); }
  /// Test hook: let @p allocations_to_pass more texture creations run, then fail the next one.
  void FailTextureAllocationAfter(std::uint32_t allocations_to_pass) {
    allocations_before_failure_ = allocations_to_pass;
  }

  [[nodiscard]] auto MallocCount() const -> std::uint64_t { return malloc_count_; }
  [[nodiscard]] auto FreeCount() const -> std::uint64_t { return free_count_; }
  [[nodiscard]] auto BufferCreateCount() const -> std::uint64_t { return buffer_create_count_; }
  [[nodiscard]] auto TextureCreateCount() const -> std::uint64_t { return texture_create_count_; }
  [[nodiscard]] auto SubmitCount() const -> std::uint64_t { return submit_count_; }
  [[nodiscard]] auto WaitCount() const -> std::uint64_t { return wait_count_; }
  [[nodiscard]] auto HostToDeviceCopyCount() const -> std::uint64_t { return h2d_copy_count_; }
  [[nodiscard]] auto HostToDeviceBytes() const -> std::uint64_t { return h2d_bytes_; }
  [[nodiscard]] auto DeviceToHostCopyCount() const -> std::uint64_t { return d2h_copy_count_; }
  [[nodiscard]] auto LastHostToDeviceRanges() const -> const std::vector<ByteRange>& {
    return last_h2d_ranges_;
  }
  [[nodiscard]] auto LastTextureRectangles() const -> const std::vector<RectI>& {
    return last_texture_rectangles_;
  }

 private:
  friend class VulkanCommandContext;

  /// A buffer or an image that is given back, with the last batch serial that can use it.
  struct RetiredResource {
    VkBuffer      buffer     = VK_NULL_HANDLE;
    VkImage       image      = VK_NULL_HANDLE;
    VkImageView   view       = VK_NULL_HANDLE;
    VmaAllocation allocation = VK_NULL_HANDLE;
    std::uint64_t last_batch = 0;
  };

  /// Called by `VulkanCommandContext` when a recording for this backend opens and closes.
  void               NoteRecordingOpened(CommandContext& command_context);
  void               NoteRecordingClosed(CommandContext& command_context) noexcept;

  void               Retire(RetiredResource resource, std::uint64_t image_resource_id) noexcept;
  void               DestroyResource(RetiredResource& resource) noexcept;
  /// Destroys each retired resource whose last batch is complete.
  void               DestroyCompletedRetired() noexcept;
  /// Submits the open recording of @p command_context as a batch.
  void               SubmitRecording(CommandContext& command_context);
  /// Waits for every batch of @p command_context and destroys the resources free now.
  void               WaitBatches(CommandContext& command_context);

  /**
   * @brief Checks @p bytes against the memory limit of plan section 4.2 and the application
   * limit. Writes `gpu.alloc.failed` and throws std::runtime_error with the size and the limit.
   */
  void               CheckMemoryLimit(const char* operation, std::size_t bytes) const;
  /// Counts a successful allocation of @p bytes; the first one writes gpu.alloc.first.
  void               NoteAllocation(std::size_t bytes);
  [[noreturn]] void  ThrowAllocationFailure(const char* operation, std::size_t bytes,
                                            const std::string& message) const;
  [[nodiscard]] auto ReadHeapBudget() const -> VmaBudget;
  [[nodiscard]] auto ConsumeInjectedFailure(std::optional<std::uint32_t>& calls_to_pass) -> bool;
  /// @ref CreateBuffer with the memory priority @p priority.
  [[nodiscard]] auto CreateBufferWithPriority(std::size_t bytes, float priority) -> Buffer;
  /// Removes least recently used LUT cache entries until @p incoming more bytes fit in the budget.
  void               EvictLutsFor(std::size_t incoming);
  [[nodiscard]] auto IsImageInitialized(const Texture2D&      texture,
                                        const CommandContext& command_context) const -> bool;
  void               RequirePackedTextureBytes(const Texture2D& texture, std::size_t bytes,
                                               const char* operation) const;

  vulkan::VulkanContext*            context_;
  const VolkDeviceTable*            functions_;
  VkDevice                          device_;
  VmaAllocator                      allocator_;
  std::uint32_t                     heap_index_          = 0;
  std::size_t                       heap_bytes_          = 0;
  std::size_t                       max_buffer_bytes_    = 0;
  std::uint32_t                     max_image_dimension_ = 0;

  std::unordered_set<std::uint64_t> initialized_images_;
  std::vector<RetiredResource>      retired_;
  CommandContext*                   recording_context_       = nullptr;
  std::uint64_t                     issued_batch_            = 0;
  std::uint64_t                     completed_batch_         = 0;

  std::uint64_t                     next_resource_id_        = 1;
  std::uint64_t                     next_submission_         = 0;
  std::uint64_t                     in_flight_submission_    = 0;
  std::uint64_t                     completed_submission_    = 0;

  std::size_t                       memory_limit_bytes_      = 0;
  std::size_t                       max_slab_bytes_override_ = 0;
  // Not cleared by ResetCounters: they describe the device memory, not a measurement window.
  std::size_t                       live_bytes_              = 0;
  std::uint64_t                     live_allocations_        = 0;
  bool                              first_allocation_logged_ = false;

  std::uint64_t                     malloc_count_            = 0;
  std::uint64_t                     free_count_              = 0;
  std::uint64_t                     buffer_create_count_     = 0;
  std::uint64_t                     texture_create_count_    = 0;
  std::uint64_t                     submit_count_            = 0;
  std::uint64_t                     wait_count_              = 0;
  std::uint64_t                     h2d_copy_count_          = 0;
  std::uint64_t                     h2d_bytes_               = 0;
  std::uint64_t                     d2h_copy_count_          = 0;
  std::vector<ByteRange>            last_h2d_ranges_;
  std::vector<RectI>                last_texture_rectangles_;
  std::optional<std::uint32_t>      uploads_before_failure_;
  std::optional<std::uint32_t>      allocations_before_failure_;

  struct LutCacheEntry {
    ContentKey    key;
    Buffer        buffer;
    std::uint32_t edge_size = 0;
    std::uint64_t lru_tick  = 0;
  };
  std::vector<LutCacheEntry> lut_cache_;
  std::size_t                lut_byte_budget_             = kDefaultLutByteBudget;
  std::size_t                lut_cache_bytes_             = 0;
  std::uint64_t              lut_lru_clock_               = 0;
  std::uint64_t              lut_upload_bytes_            = 0;
  std::uint64_t              last_lut_resource_id_        = 0;
  std::uint64_t              grade_command_topology_hash_ = 0;
  Buffer                     dummy_lut_;
  Texture2D                  dummy_rgba32f_image_;
  Texture2D                  dummy_r8_image_;
};

}  // namespace alcedo

#endif  // HAVE_VULKAN
