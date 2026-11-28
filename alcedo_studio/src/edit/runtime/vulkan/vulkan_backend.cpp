//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/vulkan/vulkan_backend.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "utils/logging/log.hpp"
#include "vulkan/vulkan_api_counters.hpp"
#include "vulkan/vulkan_device_capabilities.hpp"
#include "vulkan/vulkan_result.hpp"

namespace alcedo {
namespace {

using vulkan::ThrowIfVulkanError;

constexpr VkFormatFeatureFlags2 kRequiredImageFeatures = VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT |
                                                         VK_FORMAT_FEATURE_2_TRANSFER_SRC_BIT |
                                                         VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;

auto VulkanFormatOf(TextureFormat format) -> VkFormat {
  switch (format) {
    case TextureFormat::R8:
      return VK_FORMAT_R8_UNORM;
    case TextureFormat::Rgba8:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureFormat::R32f:
      return VK_FORMAT_R32_SFLOAT;
    case TextureFormat::Rgba32f:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TextureFormat::R16u:
      return VK_FORMAT_R16_UINT;
  }
  throw std::invalid_argument("VulkanBackend: unknown texture format");
}

auto ColorLayers() -> VkImageSubresourceLayers { return {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; }

/// A copy region between packed pixels at @p buffer_offset and the rectangle of an image.
auto PackedImageRegion(VkDeviceSize buffer_offset, std::int32_t x, std::int32_t y,
                       std::uint32_t width, std::uint32_t height) -> VkBufferImageCopy {
  VkBufferImageCopy region{};
  region.bufferOffset      = buffer_offset;
  region.bufferRowLength   = 0;  // packed rows
  region.bufferImageHeight = 0;
  region.imageSubresource  = ColorLayers();
  region.imageOffset       = {x, y, 0};
  region.imageExtent       = {width, height, 1};
  return region;
}

void FlushStaging(VmaAllocator allocator, const VulkanStagingRange& range) {
  ThrowIfVulkanError(vmaFlushAllocation(allocator, range.allocation, range.offset, range.bytes),
                     "vmaFlushAllocation");
}

}  // namespace

// ---- Buffer ---------------------------------------------------------------------------------

VulkanBackend::Buffer::Buffer(VulkanBackend* owner, VkBuffer buffer, VmaAllocation allocation,
                              VkDeviceAddress address, std::size_t bytes, std::uint64_t resource_id)
    : owner_(owner),
      buffer_(buffer),
      allocation_(allocation),
      address_(address),
      bytes_(bytes),
      resource_id_(resource_id) {}

VulkanBackend::Buffer::~Buffer() { Reset(); }

VulkanBackend::Buffer::Buffer(Buffer&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      buffer_(std::exchange(other.buffer_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      address_(std::exchange(other.address_, 0)),
      bytes_(std::exchange(other.bytes_, 0)),
      resource_id_(std::exchange(other.resource_id_, 0)) {}

auto VulkanBackend::Buffer::operator=(Buffer&& other) noexcept -> Buffer& {
  if (this != &other) {
    Reset();
    owner_       = std::exchange(other.owner_, nullptr);
    buffer_      = std::exchange(other.buffer_, VK_NULL_HANDLE);
    allocation_  = std::exchange(other.allocation_, VK_NULL_HANDLE);
    address_     = std::exchange(other.address_, 0);
    bytes_       = std::exchange(other.bytes_, 0);
    resource_id_ = std::exchange(other.resource_id_, 0);
  }
  return *this;
}

void VulkanBackend::Buffer::Reset() noexcept {
  if (owner_ != nullptr && buffer_ != VK_NULL_HANDLE) {
    owner_->Retire(RetiredResource{.buffer = buffer_, .allocation = allocation_}, 0);
  }
  owner_       = nullptr;
  buffer_      = VK_NULL_HANDLE;
  allocation_  = VK_NULL_HANDLE;
  address_     = 0;
  bytes_       = 0;
  resource_id_ = 0;
}

// ---- Texture2D ------------------------------------------------------------------------------

VulkanBackend::Texture2D::Texture2D(VulkanBackend* owner, VkImage image, VkImageView view,
                                    VmaAllocation allocation, std::size_t bytes,
                                    std::uint32_t width, std::uint32_t height, TextureFormat format,
                                    std::uint64_t resource_id, float memory_priority)
    : owner_(owner),
      image_(image),
      view_(view),
      allocation_(allocation),
      bytes_(bytes),
      width_(width),
      height_(height),
      format_(format),
      resource_id_(resource_id),
      memory_priority_(memory_priority) {}

VulkanBackend::Texture2D::~Texture2D() { Reset(); }

VulkanBackend::Texture2D::Texture2D(Texture2D&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      image_(std::exchange(other.image_, VK_NULL_HANDLE)),
      view_(std::exchange(other.view_, VK_NULL_HANDLE)),
      allocation_(std::exchange(other.allocation_, VK_NULL_HANDLE)),
      bytes_(std::exchange(other.bytes_, 0)),
      width_(std::exchange(other.width_, 0)),
      height_(std::exchange(other.height_, 0)),
      format_(std::exchange(other.format_, TextureFormat::R8)),
      resource_id_(std::exchange(other.resource_id_, 0)),
      memory_priority_(std::exchange(other.memory_priority_, 0.0F)) {}

auto VulkanBackend::Texture2D::operator=(Texture2D&& other) noexcept -> Texture2D& {
  if (this != &other) {
    Reset();
    owner_           = std::exchange(other.owner_, nullptr);
    image_           = std::exchange(other.image_, VK_NULL_HANDLE);
    view_            = std::exchange(other.view_, VK_NULL_HANDLE);
    allocation_      = std::exchange(other.allocation_, VK_NULL_HANDLE);
    bytes_           = std::exchange(other.bytes_, 0);
    width_           = std::exchange(other.width_, 0);
    height_          = std::exchange(other.height_, 0);
    format_          = std::exchange(other.format_, TextureFormat::R8);
    resource_id_     = std::exchange(other.resource_id_, 0);
    memory_priority_ = std::exchange(other.memory_priority_, 0.0F);
  }
  return *this;
}

void VulkanBackend::Texture2D::Reset() noexcept {
  if (owner_ != nullptr && image_ != VK_NULL_HANDLE) {
    owner_->Retire(RetiredResource{.image = image_, .view = view_, .allocation = allocation_},
                   resource_id_);
  }
  owner_       = nullptr;
  image_       = VK_NULL_HANDLE;
  view_        = VK_NULL_HANDLE;
  allocation_  = VK_NULL_HANDLE;
  bytes_       = 0;
  width_       = 0;
  height_      = 0;
  format_      = TextureFormat::R8;
  resource_id_ = 0;
}

// ---- Backend --------------------------------------------------------------------------------

VulkanBackend::VulkanBackend()
    : context_(&vulkan::VulkanContext::Instance()),
      functions_(&context_->PipelineDeviceFunctions()),
      device_(context_->PipelineQueue().device),
      allocator_(context_->PipelineAllocator()) {
  const VkPhysicalDeviceMemoryProperties* memory = nullptr;
  vmaGetMemoryProperties(allocator_, &memory);
  bool found = false;
  for (std::uint32_t index = 0; index < memory->memoryHeapCount; ++index) {
    const auto& heap = memory->memoryHeaps[index];
    if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0 && heap.size > heap_bytes_) {
      heap_index_ = index;
      heap_bytes_ = static_cast<std::size_t>(heap.size);
      found       = true;
    }
  }
  if (!found) {
    // The requirement check accepts no device without device-local memory.
    throw std::logic_error("VulkanBackend: the pipeline device has no device-local heap");
  }
  const auto& capabilities = context_->Capabilities();
  max_buffer_bytes_ = static_cast<std::size_t>(capabilities.max_buffer_bytes) & ~std::size_t{255};
  max_image_dimension_ = capabilities.limits.maxImageDimension2D;
}

VulkanBackend::~VulkanBackend() {
  if (recording_context_ != nullptr) {
    // A recording that was never submitted uses no GPU time; its context forgets it.
    recording_context_->DropRecording();
  }
  // The cached and dummy resources go to the retired list first, so the loop destroys them.
  lut_cache_.clear();
  dummy_lut_.Reset();
  dummy_rgba32f_image_.Reset();
  dummy_r8_image_.Reset();
  for (auto& resource : retired_) {
    DestroyResource(resource);
  }
  retired_.clear();
}

auto VulkanBackend::MaxSlabBytes() const -> std::size_t {
  return max_slab_bytes_override_ != 0 ? max_slab_bytes_override_ : max_buffer_bytes_;
}

auto VulkanBackend::MaxTransientBytes() const -> std::size_t {
  return heap_bytes_ - heap_bytes_ / 4;
}

auto VulkanBackend::ReadHeapBudget() const -> VmaBudget {
  // VMA reads VK_EXT_memory_budget again at each frame index change. The budget changes with the
  // memory use of other processes, so each allocation reads it again (plan section 4.2).
  static std::atomic<std::uint32_t> frame_index{0};
  vmaSetCurrentFrameIndex(allocator_, frame_index.fetch_add(1, std::memory_order_relaxed) + 1);
  std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
  vmaGetHeapBudgets(allocator_, budgets.data());
  return budgets[heap_index_];
}

auto VulkanBackend::QueryDeviceMemory() const -> GpuDeviceMemorySnapshot {
  const VmaBudget         budget = ReadHeapBudget();
  GpuDeviceMemorySnapshot snapshot;
  snapshot.total_bytes = static_cast<std::size_t>(budget.budget);
  snapshot.free_bytes =
      budget.budget > budget.usage ? static_cast<std::size_t>(budget.budget - budget.usage) : 0;
  snapshot.valid = snapshot.total_bytes > 0;
  return snapshot;
}

void VulkanBackend::ThrowAllocationFailure(const char* operation, std::size_t bytes,
                                           const std::string& message) const {
  gpu_memory_log::AllocationFailed(kName, operation, bytes, QueryDeviceMemory(), AllocationTotals(),
                                   message);
  throw std::runtime_error(message);
}

void VulkanBackend::CheckMemoryLimit(const char* operation, std::size_t bytes) const {
  if (memory_limit_bytes_ != 0 && live_bytes_ + bytes > memory_limit_bytes_) {
    std::ostringstream message;
    message << operation << ": " << bytes
            << " bytes are above the application memory limit: " << live_bytes_
            << " bytes are live of the limit " << memory_limit_bytes_;
    ThrowAllocationFailure(operation, bytes, message.str());
  }
  const VmaBudget budget = ReadHeapBudget();
  const auto      limit =
      budget.budget > kBudgetReserveBytes ? budget.budget - kBudgetReserveBytes : VkDeviceSize{0};
  if (budget.usage + bytes > limit) {
    std::ostringstream message;
    message << operation << ": " << bytes
            << " bytes are above the memory limit: the device-local heap has " << budget.usage
            << " bytes in use of the limit " << limit << " (budget " << budget.budget
            << " minus a reserve of " << kBudgetReserveBytes << ")";
    ThrowAllocationFailure(operation, bytes, message.str());
  }
}

void VulkanBackend::NoteAllocation(std::size_t bytes) {
  ++malloc_count_;
  live_bytes_ += bytes;
  ++live_allocations_;
  if (!first_allocation_logged_) {
    first_allocation_logged_ = true;
    if (log::IsEnabled(log::Level::Info, log::Category::GpuPool)) {
      gpu_memory_log::FirstAllocation(kName, context_->Capabilities().name, bytes,
                                      QueryDeviceMemory());
    }
  }
}

auto VulkanBackend::ConsumeInjectedFailure(std::optional<std::uint32_t>& calls_to_pass) -> bool {
  if (!calls_to_pass.has_value()) {
    return false;
  }
  if (*calls_to_pass == 0) {
    calls_to_pass.reset();
    return true;
  }
  --*calls_to_pass;
  return false;
}

auto VulkanBackend::CreateBuffer(std::size_t bytes) -> Buffer {
  return CreateBufferWithPriority(bytes, kWorkingSetPriority);
}

auto VulkanBackend::CreateBufferWithPriority(std::size_t bytes, float priority) -> Buffer {
  if (bytes == 0) {
    return {};
  }
  constexpr const char* kOperation = "VulkanBackend::CreateBuffer";
  CheckMemoryLimit(kOperation, bytes);
  if (bytes > MaxSlabBytes()) {
    std::ostringstream message;
    message << kOperation << ": size " << bytes << " exceeds the device max buffer size "
            << MaxSlabBytes();
    ThrowAllocationFailure(kOperation, bytes, message.str());
  }

  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size  = bytes;
  buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                      VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                      VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VmaAllocationCreateInfo allocation_info{};
  allocation_info.usage        = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  allocation_info.priority     = priority;
  VkBuffer          buffer     = VK_NULL_HANDLE;
  VmaAllocation     allocation = VK_NULL_HANDLE;
  VmaAllocationInfo result{};
  if (const VkResult status = vmaCreateBuffer(allocator_, &buffer_info, &allocation_info, &buffer,
                                              &allocation, &result);
      status != VK_SUCCESS) {
    std::ostringstream message;
    message << kOperation << ": vmaCreateBuffer failed: " << vulkan::VulkanResultName(status)
            << " size=" << bytes;
    ThrowAllocationFailure(kOperation, bytes, message.str());
  }
  VkBufferDeviceAddressInfo address_info{};
  address_info.sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
  address_info.buffer = buffer;
  const auto address  = functions_->vkGetBufferDeviceAddress(device_, &address_info);
  NoteAllocation(static_cast<std::size_t>(result.size));
  ++buffer_create_count_;
  vulkan::NoteVulkanBufferCreation();
  return Buffer{this, buffer, allocation, address, bytes, next_resource_id_++};
}

auto VulkanBackend::CreateTexture2D(std::uint32_t width, std::uint32_t height, TextureFormat format)
    -> Texture2D {
  return CreateTexture2DWithPriority(width, height, format, kWorkingSetPriority);
}

auto VulkanBackend::CreateCacheTexture2D(std::uint32_t width, std::uint32_t height,
                                         TextureFormat format) -> Texture2D {
  return CreateTexture2DWithPriority(width, height, format, kCachePriority);
}

auto VulkanBackend::CreateTexture2DWithPriority(std::uint32_t width, std::uint32_t height,
                                                TextureFormat format, float priority) -> Texture2D {
  const auto bytes = static_cast<std::size_t>(width) * height * TextureFormatBytesPerPixel(format);
  if (bytes == 0) {
    return {};
  }
  if (ConsumeInjectedFailure(allocations_before_failure_)) {
    throw std::runtime_error("VulkanBackend::CreateTexture2D: injected failure");
  }
  constexpr const char* kOperation = "VulkanBackend::CreateTexture2D";
  CheckMemoryLimit(kOperation, bytes);
  if (width > max_image_dimension_ || height > max_image_dimension_) {
    std::ostringstream message;
    message << kOperation << ": " << width << "x" << height
            << " exceeds the device maxImageDimension2D " << max_image_dimension_;
    ThrowAllocationFailure(kOperation, bytes, message.str());
  }
  const VkFormat vk_format = VulkanFormatOf(format);
  if ((context_->Capabilities().FormatFeatures(vk_format) & kRequiredImageFeatures) !=
      kRequiredImageFeatures) {
    throw std::runtime_error(std::string(kOperation) + ": the device has no storage and transfer " +
                             "support for " +
                             std::string(vulkan::PipelineImageFormatName(vk_format)) + " (" +
                             TextureFormatName(format) + ")");
  }

  VkImageCreateInfo image_info{};
  image_info.sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image_info.imageType   = VK_IMAGE_TYPE_2D;
  image_info.format      = vk_format;
  image_info.extent      = {width, height, 1};
  image_info.mipLevels   = 1;
  image_info.arrayLayers = 1;
  image_info.samples     = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling      = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage       = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VmaAllocationCreateInfo allocation_info{};
  allocation_info.usage        = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
  allocation_info.priority     = priority;
  VkImage           image      = VK_NULL_HANDLE;
  VmaAllocation     allocation = VK_NULL_HANDLE;
  VmaAllocationInfo result{};
  if (const VkResult status =
          vmaCreateImage(allocator_, &image_info, &allocation_info, &image, &allocation, &result);
      status != VK_SUCCESS) {
    std::ostringstream message;
    message << kOperation << ": vmaCreateImage failed: " << vulkan::VulkanResultName(status)
            << " size=" << width << "x" << height << " format=" << TextureFormatName(format);
    ThrowAllocationFailure(kOperation, bytes, message.str());
  }

  VkImageViewCreateInfo view_info{};
  view_info.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  view_info.image            = image;
  view_info.viewType         = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format           = vk_format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view           = VK_NULL_HANDLE;
  if (const VkResult status = functions_->vkCreateImageView(device_, &view_info, nullptr, &view);
      status != VK_SUCCESS) {
    vmaDestroyImage(allocator_, image, allocation);
    ThrowIfVulkanError(status, "vkCreateImageView");
  }
  NoteAllocation(static_cast<std::size_t>(result.size));
  ++texture_create_count_;
  vulkan::NoteVulkanImageCreation();
  return Texture2D{this,  image,  view,   allocation,          bytes,
                   width, height, format, next_resource_id_++, priority};
}

auto VulkanBackend::CreateSceneWorkImage(std::uint32_t width, std::uint32_t height)
    -> SceneWorkImage {
  const auto bytes =
      static_cast<std::size_t>(width) * height * TextureFormatBytesPerPixel(TextureFormat::Rgba32f);
  if (bytes == 0) {
    return {};
  }
  return SceneWorkImage{CreateBuffer(bytes), width, height};
}

// ---- Resource lifetime ----------------------------------------------------------------------

void VulkanBackend::Retire(RetiredResource resource, std::uint64_t image_resource_id) noexcept {
  if (image_resource_id != 0) {
    initialized_images_.erase(image_resource_id);
  }
  // The open recording can use the resource; it becomes the next batch.
  resource.last_batch = issued_batch_ + (recording_context_ != nullptr ? 1 : 0);
  if (resource.last_batch <= completed_batch_) {
    DestroyResource(resource);
    return;
  }
  // Destroying a resource that a batch still uses is not an option: without memory for the list
  // entry the process stops.
  retired_.push_back(resource);
}

void VulkanBackend::DestroyResource(RetiredResource& resource) noexcept {
  VmaAllocationInfo info{};
  vmaGetAllocationInfo(allocator_, resource.allocation, &info);
  if (resource.view != VK_NULL_HANDLE) {
    functions_->vkDestroyImageView(device_, resource.view, nullptr);
  }
  if (resource.image != VK_NULL_HANDLE) {
    vmaDestroyImage(allocator_, resource.image, resource.allocation);
  } else {
    vmaDestroyBuffer(allocator_, resource.buffer, resource.allocation);
  }
  vulkan::NoteVulkanResourceDestruction();
  live_bytes_ -= std::min(live_bytes_, static_cast<std::size_t>(info.size));
  --live_allocations_;
  ++free_count_;
  resource = {};
}

void VulkanBackend::DestroyCompletedRetired() noexcept {
  auto keep = retired_.begin();
  for (auto& resource : retired_) {
    if (resource.last_batch <= completed_batch_) {
      DestroyResource(resource);
    } else {
      *keep++ = resource;
    }
  }
  retired_.erase(keep, retired_.end());
}

void VulkanBackend::NoteRecordingOpened(CommandContext& command_context) {
  if (recording_context_ != nullptr && recording_context_ != &command_context) {
    throw std::logic_error(
        "VulkanBackend: another command context has an open recording for this backend");
  }
  recording_context_ = &command_context;
}

void VulkanBackend::NoteRecordingClosed(CommandContext& command_context) noexcept {
  if (recording_context_ == &command_context) {
    recording_context_ = nullptr;
  }
}

// ---- Recording ------------------------------------------------------------------------------

auto VulkanBackend::BeginCommand(CommandContext& command_context) -> VkCommandBuffer {
  return command_context.BeginCommand(*this);
}

auto VulkanBackend::IsImageInitialized(const Texture2D&      texture,
                                       const CommandContext& command_context) const -> bool {
  return initialized_images_.contains(texture.ResourceId()) ||
         command_context.RecordsLayoutOf(texture.ResourceId());
}

void VulkanBackend::PrepareImage(const Texture2D& texture, CommandContext& command_context) {
  if (texture.Empty()) {
    throw std::runtime_error("VulkanBackend::PrepareImage: empty texture");
  }
  if (!IsImageInitialized(texture, command_context)) {
    command_context.InitializeImageLayout(texture.Native(), texture.ResourceId());
  }
}

void VulkanBackend::RequirePackedTextureBytes(const Texture2D& texture, std::size_t bytes,
                                              const char* operation) const {
  if (texture.Empty()) {
    throw std::runtime_error(std::string(operation) + ": empty texture");
  }
  if (bytes != texture.Bytes()) {
    throw std::runtime_error(std::string(operation) + ": " + std::to_string(bytes) +
                             " bytes for a texture of " + std::to_string(texture.Bytes()));
  }
}

void VulkanBackend::UploadBufferRange(Buffer& buffer, std::uint32_t offset,
                                      std::span<const std::byte> bytes,
                                      CommandContext&            command_context) {
  if (ConsumeInjectedFailure(uploads_before_failure_)) {
    throw std::runtime_error("VulkanBackend::UploadBufferRange: injected failure");
  }
  if (bytes.empty()) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + bytes.size() > buffer.Bytes()) {
    throw std::runtime_error("VulkanBackend::UploadBufferRange: range exceeds buffer");
  }
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  const auto            staging        = command_context.upload_staging_.Allocate(bytes.size());
  std::memcpy(staging.mapped, bytes.data(), bytes.size());
  FlushStaging(allocator_, staging);
  const VkBufferCopy region{staging.offset, offset, bytes.size()};
  functions_->vkCmdCopyBuffer(command_buffer, staging.buffer, buffer.Native(), 1, &region);
  command_context.NoteWrite();
  vulkan::NoteVulkanUpload(bytes.size());
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
  last_h2d_ranges_.push_back(ByteRange{offset, static_cast<std::uint32_t>(bytes.size())});
}

void VulkanBackend::FillBufferRange(Buffer& buffer, std::uint32_t offset, std::size_t bytes,
                                    std::uint32_t value, CommandContext& command_context) {
  if (bytes == 0) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + bytes > buffer.Bytes()) {
    throw std::runtime_error("VulkanBackend::FillBufferRange: range exceeds buffer");
  }
  if (offset % 4 != 0 || bytes % 4 != 0) {
    throw std::runtime_error(
        "VulkanBackend::FillBufferRange: the offset and the size must be multiples of 4");
  }
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  functions_->vkCmdFillBuffer(command_buffer, buffer.Native(), offset, bytes, value);
  command_context.NoteWrite();
}

void VulkanBackend::DownloadBufferRange(const Buffer& buffer, std::uint32_t offset,
                                        std::span<std::byte> out, CommandContext& command_context) {
  ++d2h_copy_count_;
  if (out.empty()) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + out.size() > buffer.Bytes()) {
    throw std::runtime_error("VulkanBackend::DownloadBufferRange: range exceeds buffer");
  }
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  const auto            staging        = command_context.download_staging_.Allocate(out.size());
  const VkBufferCopy    region{offset, staging.offset, out.size()};
  functions_->vkCmdCopyBuffer(command_buffer, buffer.Native(), staging.buffer, 1, &region);
  command_context.RecordHostReadBarrier();
  command_context.NoteWrite();
  SubmitRecording(command_context);
  const std::uint64_t serial = command_context.WaitSubmittedBatches();
  completed_batch_           = std::max(completed_batch_, serial);
  ++wait_count_;
  ThrowIfVulkanError(
      vmaInvalidateAllocation(allocator_, staging.allocation, staging.offset, staging.bytes),
      "vmaInvalidateAllocation");
  std::memcpy(out.data(), staging.mapped, out.size());
  vulkan::NoteVulkanDownload(out.size());
  command_context.RecycleStaging(serial);
  DestroyCompletedRetired();
}

void VulkanBackend::UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes,
                                    CommandContext& command_context) {
  if (ConsumeInjectedFailure(uploads_before_failure_)) {
    throw std::runtime_error("VulkanBackend::UploadTexture2D: injected failure");
  }
  RequirePackedTextureBytes(texture, bytes.size(), "VulkanBackend::UploadTexture2D");
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  PrepareImage(texture, command_context);
  const auto staging = command_context.upload_staging_.Allocate(bytes.size());
  std::memcpy(staging.mapped, bytes.data(), bytes.size());
  FlushStaging(allocator_, staging);
  const auto region = PackedImageRegion(staging.offset, 0, 0, texture.Width(), texture.Height());
  functions_->vkCmdCopyBufferToImage(command_buffer, staging.buffer, texture.Native(),
                                     VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  command_context.NoteWrite();
  vulkan::NoteVulkanUpload(bytes.size());
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
}

void VulkanBackend::UploadR8TextureRect(Texture2D& texture, RectI rectangle,
                                        std::span<const std::byte> bytes,
                                        CommandContext&            command_context) {
  if (ConsumeInjectedFailure(uploads_before_failure_)) {
    throw std::runtime_error("VulkanBackend::UploadR8TextureRect: injected failure");
  }
  if (texture.Empty() || texture.Format() != TextureFormat::R8 || rectangle.x < 0 ||
      rectangle.y < 0 || rectangle.width <= 0 || rectangle.height <= 0 ||
      rectangle.X1() > static_cast<std::int32_t>(texture.Width()) ||
      rectangle.Y1() > static_cast<std::int32_t>(texture.Height()) ||
      bytes.size() != static_cast<std::size_t>(rectangle.width) * rectangle.height) {
    throw std::runtime_error("VulkanBackend::UploadR8TextureRect: invalid rectangle");
  }
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  PrepareImage(texture, command_context);
  const auto staging = command_context.upload_staging_.Allocate(bytes.size());
  std::memcpy(staging.mapped, bytes.data(), bytes.size());
  FlushStaging(allocator_, staging);
  const auto region = PackedImageRegion(staging.offset, rectangle.x, rectangle.y,
                                        static_cast<std::uint32_t>(rectangle.width),
                                        static_cast<std::uint32_t>(rectangle.height));
  functions_->vkCmdCopyBufferToImage(command_buffer, staging.buffer, texture.Native(),
                                     VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  command_context.NoteWrite();
  vulkan::NoteVulkanUpload(bytes.size());
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
  last_texture_rectangles_.push_back(rectangle);
}

void VulkanBackend::DownloadTexture2D(const Texture2D& texture, std::span<std::byte> out,
                                      CommandContext& command_context) {
  ++d2h_copy_count_;
  RequirePackedTextureBytes(texture, out.size(), "VulkanBackend::DownloadTexture2D");
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  PrepareImage(texture, command_context);
  const auto staging = command_context.download_staging_.Allocate(out.size());
  const auto region  = PackedImageRegion(staging.offset, 0, 0, texture.Width(), texture.Height());
  functions_->vkCmdCopyImageToBuffer(command_buffer, texture.Native(), VK_IMAGE_LAYOUT_GENERAL,
                                     staging.buffer, 1, &region);
  command_context.RecordHostReadBarrier();
  command_context.NoteWrite();
  SubmitRecording(command_context);
  const std::uint64_t serial = command_context.WaitSubmittedBatches();
  completed_batch_           = std::max(completed_batch_, serial);
  ++wait_count_;
  ThrowIfVulkanError(
      vmaInvalidateAllocation(allocator_, staging.allocation, staging.offset, staging.bytes),
      "vmaInvalidateAllocation");
  std::memcpy(out.data(), staging.mapped, out.size());
  vulkan::NoteVulkanDownload(out.size());
  command_context.RecycleStaging(serial);
  DestroyCompletedRetired();
}

void VulkanBackend::CopyTexture2D(const Texture2D& src, Texture2D& dst,
                                  CommandContext& command_context) {
  if (src.Empty() || dst.Empty()) {
    throw std::runtime_error("VulkanBackend::CopyTexture2D: empty texture");
  }
  if (src.Width() != dst.Width() || src.Height() != dst.Height() || src.Format() != dst.Format()) {
    throw std::runtime_error("VulkanBackend::CopyTexture2D: size or format mismatch");
  }
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  PrepareImage(src, command_context);
  PrepareImage(dst, command_context);
  VkImageCopy region{};
  region.srcSubresource = ColorLayers();
  region.dstSubresource = ColorLayers();
  region.extent         = {src.Width(), src.Height(), 1};
  functions_->vkCmdCopyImage(command_buffer, src.Native(), VK_IMAGE_LAYOUT_GENERAL, dst.Native(),
                             VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  command_context.NoteWrite();
}

void VulkanBackend::CopyTexture2DRegion(const Texture2D& src, RectI rectangle, Texture2D& dst,
                                        CommandContext& command_context) {
  if (src.Empty() || dst.Empty()) {
    throw std::runtime_error("VulkanBackend::CopyTexture2DRegion: empty texture");
  }
  if (src.Format() != dst.Format()) {
    throw std::runtime_error("VulkanBackend::CopyTexture2DRegion: format mismatch");
  }
  if (rectangle.x < 0 || rectangle.y < 0 || rectangle.width <= 0 || rectangle.height <= 0 ||
      static_cast<std::int64_t>(rectangle.x) + rectangle.width > src.Width() ||
      static_cast<std::int64_t>(rectangle.y) + rectangle.height > src.Height()) {
    throw std::runtime_error("VulkanBackend::CopyTexture2DRegion: rectangle (" +
                             std::to_string(rectangle.x) + ", " + std::to_string(rectangle.y) +
                             ", " + std::to_string(rectangle.width) + " x " +
                             std::to_string(rectangle.height) + ") is not inside the source of " +
                             std::to_string(src.Width()) + " x " + std::to_string(src.Height()));
  }
  if (dst.Width() != static_cast<std::uint32_t>(rectangle.width) ||
      dst.Height() != static_cast<std::uint32_t>(rectangle.height)) {
    throw std::runtime_error(
        "VulkanBackend::CopyTexture2DRegion: the destination is not the "
        "size of the rectangle");
  }
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  PrepareImage(src, command_context);
  PrepareImage(dst, command_context);
  VkImageCopy region{};
  region.srcSubresource = ColorLayers();
  region.srcOffset      = {rectangle.x, rectangle.y, 0};
  region.dstSubresource = ColorLayers();
  region.extent         = {dst.Width(), dst.Height(), 1};
  functions_->vkCmdCopyImage(command_buffer, src.Native(), VK_IMAGE_LAYOUT_GENERAL, dst.Native(),
                             VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  command_context.NoteWrite();
}

void VulkanBackend::CopyBufferToImage(const Buffer& src, std::uint32_t src_offset, Texture2D& dst,
                                      CommandContext& command_context) {
  if (src.Empty() || dst.Empty()) {
    throw std::runtime_error("VulkanBackend::CopyBufferToImage: empty resource");
  }
  if (static_cast<std::size_t>(src_offset) + dst.Bytes() > src.Bytes()) {
    throw std::runtime_error("VulkanBackend::CopyBufferToImage: range exceeds buffer");
  }
  const auto texel_bytes = TextureFormatBytesPerPixel(dst.Format());
  if (src_offset % 4 != 0 || src_offset % texel_bytes != 0) {
    throw std::runtime_error(
        "VulkanBackend::CopyBufferToImage: the offset is not a multiple of 4 and of the texel "
        "size");
  }
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  PrepareImage(dst, command_context);
  const auto region = PackedImageRegion(src_offset, 0, 0, dst.Width(), dst.Height());
  functions_->vkCmdCopyBufferToImage(command_buffer, src.Native(), dst.Native(),
                                     VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  command_context.NoteWrite();
}

void VulkanBackend::CopyImageToBuffer(const Texture2D& src, Buffer& dst, std::uint32_t dst_offset,
                                      CommandContext& command_context) {
  if (src.Empty() || dst.Empty()) {
    throw std::runtime_error("VulkanBackend::CopyImageToBuffer: empty resource");
  }
  if (static_cast<std::size_t>(dst_offset) + src.Bytes() > dst.Bytes()) {
    throw std::runtime_error("VulkanBackend::CopyImageToBuffer: range exceeds buffer");
  }
  const auto texel_bytes = TextureFormatBytesPerPixel(src.Format());
  if (dst_offset % 4 != 0 || dst_offset % texel_bytes != 0) {
    throw std::runtime_error(
        "VulkanBackend::CopyImageToBuffer: the offset is not a multiple of 4 and of the texel "
        "size");
  }
  const VkCommandBuffer command_buffer = BeginCommand(command_context);
  PrepareImage(src, command_context);
  const auto region = PackedImageRegion(dst_offset, 0, 0, src.Width(), src.Height());
  functions_->vkCmdCopyImageToBuffer(command_buffer, src.Native(), VK_IMAGE_LAYOUT_GENERAL,
                                     dst.Native(), 1, &region);
  command_context.NoteWrite();
}

// ---- LUT cache and pass resources -------------------------------------------------------------

auto VulkanBackend::AcquireLut(ContentKey key, std::span<const std::byte> packed_rgba,
                               std::uint32_t edge, CommandContext& command_context)
    -> VulkanLutBinding {
  if (key.Empty() || edge <= 1 || packed_rgba.empty()) {
    return DummyLut();
  }
  for (auto& entry : lut_cache_) {
    if (entry.key == key && entry.edge_size == edge) {
      entry.lru_tick        = ++lut_lru_clock_;
      last_lut_resource_id_ = entry.buffer.ResourceId();
      return VulkanLutBinding{entry.buffer.DeviceAddress(), entry.buffer.ResourceId(), edge};
    }
  }
  EvictLutsFor(packed_rgba.size());
  auto buffer = CreateBufferWithPriority(packed_rgba.size(), kCachePriority);
  UploadBufferRange(buffer, 0, packed_rgba, command_context);
  lut_upload_bytes_ += packed_rgba.size();
  LutCacheEntry entry;
  entry.key       = key;
  entry.edge_size = edge;
  entry.lru_tick  = ++lut_lru_clock_;
  entry.buffer    = std::move(buffer);
  lut_cache_bytes_ += entry.buffer.Bytes();
  lut_cache_.push_back(std::move(entry));
  const auto& stored    = lut_cache_.back().buffer;
  last_lut_resource_id_ = stored.ResourceId();
  return VulkanLutBinding{stored.DeviceAddress(), stored.ResourceId(), edge};
}

void VulkanBackend::EvictLutsFor(std::size_t incoming) {
  if (lut_byte_budget_ == 0) {
    return;
  }
  // A recording that uses an entry keeps it: Reset gives the buffer back by the lifetime rule.
  while (!lut_cache_.empty() && lut_cache_bytes_ + incoming > lut_byte_budget_) {
    const auto oldest = std::min_element(
        lut_cache_.begin(), lut_cache_.end(),
        [](const LutCacheEntry& a, const LutCacheEntry& b) { return a.lru_tick < b.lru_tick; });
    lut_cache_bytes_ -= oldest->buffer.Bytes();
    lut_cache_.erase(oldest);
  }
}

auto VulkanBackend::DummyLut() -> VulkanLutBinding {
  if (dummy_lut_.Empty()) {
    dummy_lut_ = CreateBuffer(16);
  }
  return VulkanLutBinding{dummy_lut_.DeviceAddress(), dummy_lut_.ResourceId(), 0};
}

void VulkanBackend::SetLutByteBudget(std::size_t bytes) {
  lut_byte_budget_ = bytes;
  EvictLutsFor(0);
}

auto VulkanBackend::DummyStorageImage(TextureFormat format) -> const Texture2D& {
  Texture2D* image = nullptr;
  if (format == TextureFormat::Rgba32f) {
    image = &dummy_rgba32f_image_;
  } else if (format == TextureFormat::R8) {
    image = &dummy_r8_image_;
  } else {
    throw std::invalid_argument(
        std::string("VulkanBackend::DummyStorageImage: no dummy image of ") +
        TextureFormatName(format));
  }
  if (image->Empty()) {
    *image = CreateTexture2D(1, 1, format);
  }
  return *image;
}

// ---- Submission -----------------------------------------------------------------------------

void VulkanBackend::SubmitRecording(CommandContext& command_context) {
  if (!command_context.IsRecording()) {
    return;
  }
  initialized_images_.reserve(initialized_images_.size() +
                              command_context.recorded_layouts_.size());
  const auto initialized = command_context.SubmitRecording(issued_batch_ + 1);
  ++issued_batch_;
  initialized_images_.insert(initialized.begin(), initialized.end());
  ++submit_count_;
}

void VulkanBackend::WaitBatches(CommandContext& command_context) {
  const std::uint64_t serial = command_context.WaitSubmittedBatches();
  if (serial != 0) {
    completed_batch_ = std::max(completed_batch_, serial);
    ++wait_count_;
    command_context.RecycleStaging(serial);
  }
  DestroyCompletedRetired();
}

void VulkanBackend::Submit(CommandContext& command_context) {
  SubmitRecording(command_context);
  in_flight_submission_ = command_context.SubmissionId();
}

void VulkanBackend::Wait(CommandContext& command_context) {
  SubmitRecording(command_context);
  WaitBatches(command_context);
  if (in_flight_submission_ != 0) {
    completed_submission_ = in_flight_submission_;
    in_flight_submission_ = 0;
  }
}

void VulkanBackend::SynchronizeRecordedWork(CommandContext& command_context) {
  SubmitRecording(command_context);
  WaitBatches(command_context);
}

void VulkanBackend::ResetCounters() {
  malloc_count_         = 0;
  free_count_           = 0;
  buffer_create_count_  = 0;
  texture_create_count_ = 0;
  submit_count_         = 0;
  wait_count_           = 0;
  h2d_copy_count_       = 0;
  h2d_bytes_            = 0;
  d2h_copy_count_       = 0;
  lut_upload_bytes_     = 0;
  last_h2d_ranges_.clear();
  last_texture_rectangles_.clear();
}

}  // namespace alcedo
