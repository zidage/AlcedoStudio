//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_backend.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <alcedo/metal/Metal.hpp>

#include "edit/runtime/execution_plan.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
void WarmUpMetalDagPlan(MetalBackend& backend, const ExecutionPlan& plan);
}  // namespace alcedo

namespace alcedo {
namespace {

constexpr std::size_t kMinHeapPageBytes = 16ull << 20;
constexpr std::size_t kBlitRowAlign     = 256;

auto                  ThrowIfNull(const void* pointer, const char* message) {
  if (pointer == nullptr) {
    throw std::runtime_error(message);
  }
}

auto ToPixelFormat(TextureFormat format) -> MTL::PixelFormat {
  switch (format) {
    case TextureFormat::R8:
      return MTL::PixelFormatR8Unorm;
    case TextureFormat::Rgba8:
      return MTL::PixelFormatRGBA8Unorm;
    case TextureFormat::R32f:
      return MTL::PixelFormatR32Float;
    case TextureFormat::Rgba32f:
      return MTL::PixelFormatRGBA32Float;
    case TextureFormat::R16u:
      return MTL::PixelFormatR16Uint;
  }
  throw std::runtime_error("MetalBackend: unsupported texture format");
}

auto AlignedRowBytes(std::uint32_t width, TextureFormat format) -> std::size_t {
  const auto raw = static_cast<std::size_t>(width) * TextureFormatBytesPerPixel(format);
  return (raw + kBlitRowAlign - 1) & ~(kBlitRowAlign - 1);
}

auto RoundUpHeapPage(std::size_t bytes) -> std::size_t {
  if (bytes <= kMinHeapPageBytes) {
    return kMinHeapPageBytes;
  }
  const auto pages = (bytes + kMinHeapPageBytes - 1) / kMinHeapPageBytes;
  return pages * kMinHeapPageBytes;
}

auto ParameterResourceOptions(MTL::Device* device) -> MTL::ResourceOptions {
  if (device->hasUnifiedMemory()) {
    return MTL::ResourceStorageModeShared;
  }
  return MTL::ResourceStorageModeManaged;
}

auto MakePrivateTextureDescriptor(std::uint32_t width, std::uint32_t height, TextureFormat format)
    -> NS::SharedPtr<MTL::TextureDescriptor> {
  auto descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
  descriptor->setTextureType(MTL::TextureType2D);
  descriptor->setWidth(width);
  descriptor->setHeight(height);
  descriptor->setDepth(1);
  descriptor->setMipmapLevelCount(1);
  descriptor->setSampleCount(1);
  descriptor->setArrayLength(1);
  descriptor->setPixelFormat(ToPixelFormat(format));
  descriptor->setStorageMode(MTL::StorageModePrivate);
  descriptor->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageShaderWrite);
  descriptor->setHazardTrackingMode(MTL::HazardTrackingModeTracked);
  return descriptor;
}

void CopyPackedToAligned(std::span<const std::byte> packed, std::uint32_t width,
                         std::uint32_t height, TextureFormat format, void* destination) {
  const auto src_row = static_cast<std::size_t>(width) * TextureFormatBytesPerPixel(format);
  const auto dst_row = AlignedRowBytes(width, format);
  auto*      dst     = static_cast<std::byte*>(destination);
  for (std::uint32_t y = 0; y < height; ++y) {
    std::memcpy(dst + static_cast<std::size_t>(y) * dst_row, packed.data() + y * src_row, src_row);
  }
}

void CopyAlignedToPacked(const void* source, std::uint32_t width, std::uint32_t height,
                         TextureFormat format, std::span<std::byte> packed) {
  const auto  dst_row = static_cast<std::size_t>(width) * TextureFormatBytesPerPixel(format);
  const auto  src_row = AlignedRowBytes(width, format);
  const auto* src     = static_cast<const std::byte*>(source);
  for (std::uint32_t y = 0; y < height; ++y) {
    std::memcpy(packed.data() + y * dst_row, src + static_cast<std::size_t>(y) * src_row, dst_row);
  }
}

}  // namespace

class MetalCommandContext::Gpu {
 public:
  NS::SharedPtr<MTL::CommandBuffer>         buffer;
  NS::SharedPtr<MTL::BlitCommandEncoder>    blit;
  NS::SharedPtr<MTL::ComputeCommandEncoder> compute;

  void                                      EndEncoders() {
    if (blit) {
      blit->endEncoding();
      blit.reset();
    }
    if (compute) {
      compute->endEncoding();
      compute.reset();
    }
  }

  void Reset() {
    EndEncoders();
    buffer.reset();
  }
};

MetalCommandContext::MetalCommandContext() : gpu_(std::make_unique<Gpu>()) {}
MetalCommandContext::~MetalCommandContext() = default;
MetalCommandContext::MetalCommandContext(MetalCommandContext&& other) noexcept
    : gpu_(std::move(other.gpu_)), submission_id_(other.submission_id_) {
  other.gpu_           = std::make_unique<Gpu>();
  other.submission_id_ = 0;
}
auto MetalCommandContext::operator=(MetalCommandContext&& other) noexcept -> MetalCommandContext& {
  if (this != &other) {
    gpu_                 = std::move(other.gpu_);
    submission_id_       = other.submission_id_;
    other.gpu_           = std::make_unique<Gpu>();
    other.submission_id_ = 0;
  }
  return *this;
}
auto MetalCommandContext::NativeCommandBuffer() const -> void* {
  return gpu_ && gpu_->buffer ? gpu_->buffer.get() : nullptr;
}

struct HeapPage {
  NS::SharedPtr<MTL::Heap> heap;
  std::size_t              bytes = 0;
};

struct LiveBuffer {
  MTL::Buffer*  native      = nullptr;
  std::uint64_t gpu_address = 0;
  std::size_t   bytes       = 0;
};

class MetalBackend::Gpu {
 public:
  MTL::Device*                     device = nullptr;
  MTL::CommandQueue*               queue  = nullptr;
  std::vector<HeapPage>            heaps;
  NS::SharedPtr<MTL::Buffer>       staging;
  std::size_t                      staging_bytes = 0;
  NS::SharedPtr<MTL::SamplerState> linear_clamp;
  NS::SharedPtr<MTL::SamplerState> nearest_clamp;
  std::vector<LiveBuffer>          live_buffers;
  std::uint64_t                    pipeline_create_baseline = 0;
  std::uint64_t                    pipeline_hit_baseline    = 0;

  void                             UnregisterBuffer(MTL::Buffer* native) {
    live_buffers.erase(
        std::remove_if(live_buffers.begin(), live_buffers.end(),
                                                   [native](const LiveBuffer& entry) { return entry.native == native; }),
        live_buffers.end());
  }
};

class MetalBackendImpl {
 public:
  static void AttachGpu(MetalBackend::Gpu& gpu) {
    if (gpu.device != nullptr) {
      return;
    }
    auto& context = MetalContext::Instance();
    gpu.device    = context.Device();
    gpu.queue     = context.Queue();
    ThrowIfNull(gpu.device, "MetalBackend: Metal device is unavailable.");
    ThrowIfNull(gpu.queue, "MetalBackend: Metal command queue is unavailable.");

    auto linear_desc = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
    linear_desc->setMinFilter(MTL::SamplerMinMagFilterLinear);
    linear_desc->setMagFilter(MTL::SamplerMinMagFilterLinear);
    linear_desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    linear_desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    gpu.linear_clamp  = NS::TransferPtr(gpu.device->newSamplerState(linear_desc.get()));

    auto nearest_desc = NS::TransferPtr(MTL::SamplerDescriptor::alloc()->init());
    nearest_desc->setMinFilter(MTL::SamplerMinMagFilterNearest);
    nearest_desc->setMagFilter(MTL::SamplerMinMagFilterNearest);
    nearest_desc->setSAddressMode(MTL::SamplerAddressModeClampToEdge);
    nearest_desc->setTAddressMode(MTL::SamplerAddressModeClampToEdge);
    gpu.nearest_clamp            = NS::TransferPtr(gpu.device->newSamplerState(nearest_desc.get()));

    const auto stats             = metal::ComputePipelineCache::Instance().GetStats();
    gpu.pipeline_create_baseline = stats.creates;
    gpu.pipeline_hit_baseline    = stats.hits;
  }

  static void ThrowIfHeapBusy(const MetalBackend& backend) {
    if (backend.HasInFlightSubmission()) {
      throw std::runtime_error(
          "MetalBackend: cannot grow an MTLHeap page while a GPU submission is in flight");
    }
  }

  static void GrowHeap(MetalBackend& backend, MetalBackend::Gpu& gpu, std::size_t at_least) {
    ThrowIfHeapBusy(backend);
    auto descriptor = NS::TransferPtr(MTL::HeapDescriptor::alloc()->init());
    descriptor->setSize(RoundUpHeapPage(at_least));
    descriptor->setStorageMode(MTL::StorageModePrivate);
    descriptor->setHazardTrackingMode(MTL::HazardTrackingModeTracked);
    descriptor->setType(MTL::HeapTypeAutomatic);
    auto heap = NS::TransferPtr(gpu.device->newHeap(descriptor.get()));
    ThrowIfNull(heap.get(), "MetalBackend: failed to allocate MTLHeap page");
    HeapPage page;
    page.heap  = std::move(heap);
    page.bytes = descriptor->size();
    gpu.heaps.push_back(std::move(page));
    backend.NoteHeapCreate();
  }

  static auto AllocatePrivateBuffer(MetalBackend& backend, MetalBackend::Gpu& gpu,
                                    std::size_t bytes) -> MTL::Buffer* {
    const auto options      = MTL::ResourceStorageModePrivate;
    const auto needed       = gpu.device->heapBufferSizeAndAlign(bytes, options);
    auto       try_allocate = [&]() -> MTL::Buffer* {
      for (auto& page : gpu.heaps) {
        if (page.heap->maxAvailableSize(needed.align) < needed.size) {
          continue;
        }
        if (auto* buffer = page.heap->newBuffer(bytes, options)) {
          return buffer;
        }
      }
      return nullptr;
    };
    if (auto* buffer = try_allocate()) {
      return buffer;
    }
    GrowHeap(backend, gpu, needed.size);
    auto* buffer = try_allocate();
    ThrowIfNull(buffer, "MetalBackend: failed to allocate private buffer from MTLHeap");
    return buffer;
  }

  static auto AllocatePrivateTexture(MetalBackend& backend, MetalBackend::Gpu& gpu,
                                     const MTL::TextureDescriptor* descriptor) -> MTL::Texture* {
    const auto needed       = gpu.device->heapTextureSizeAndAlign(descriptor);
    auto       try_allocate = [&]() -> MTL::Texture* {
      for (auto& page : gpu.heaps) {
        if (page.heap->maxAvailableSize(needed.align) < needed.size) {
          continue;
        }
        if (auto* texture = page.heap->newTexture(descriptor)) {
          return texture;
        }
      }
      return nullptr;
    };
    if (auto* texture = try_allocate()) {
      return texture;
    }
    GrowHeap(backend, gpu, needed.size);
    auto* texture = try_allocate();
    ThrowIfNull(texture, "MetalBackend: failed to allocate private texture from MTLHeap");
    return texture;
  }

  static void EnsureCommandBuffer(MetalBackend& backend, MetalBackend::Gpu& gpu,
                                  MetalCommandContext& command_context) {
    auto& ctx = *command_context.gpu_;
    if (ctx.buffer) {
      return;
    }
    ctx.buffer = NS::RetainPtr(gpu.queue->commandBuffer());
    ThrowIfNull(ctx.buffer.get(), "MetalBackend: failed to create MTLCommandBuffer");
    ++backend.command_buffer_create_count_;
  }

  static void EnsureBlitEncoder(MetalBackend& backend, MetalBackend::Gpu& gpu,
                                MetalCommandContext& command_context) {
    EnsureCommandBuffer(backend, gpu, command_context);
    auto& ctx = *command_context.gpu_;
    if (ctx.compute) {
      ctx.compute->endEncoding();
      ctx.compute.reset();
    }
    if (!ctx.blit) {
      ctx.blit = NS::RetainPtr(ctx.buffer->blitCommandEncoder());
      ThrowIfNull(ctx.blit.get(), "MetalBackend: failed to create blit encoder");
    }
  }

  static void EnsureComputeEncoder(MetalBackend& backend, MetalBackend::Gpu& gpu,
                                   MetalCommandContext& command_context) {
    EnsureCommandBuffer(backend, gpu, command_context);
    auto& ctx = *command_context.gpu_;
    if (ctx.blit) {
      ctx.blit->endEncoding();
      ctx.blit.reset();
    }
    if (!ctx.compute) {
      ctx.compute = NS::RetainPtr(ctx.buffer->computeCommandEncoder());
      ThrowIfNull(ctx.compute.get(), "MetalBackend: failed to create compute encoder");
    }
  }

  static void EnsureStaging(MetalBackend& backend, MetalBackend::Gpu& gpu, std::size_t bytes) {
    if (bytes <= gpu.staging_bytes && gpu.staging) {
      return;
    }
    ThrowIfHeapBusy(backend);
    auto buffer = NS::TransferPtr(gpu.device->newBuffer(bytes, MTL::ResourceStorageModeShared));
    ThrowIfNull(buffer.get(), "MetalBackend: failed to allocate staging buffer");
    gpu.staging       = std::move(buffer);
    gpu.staging_bytes = bytes;
    backend.NoteBufferCreate();
  }

  static void MarkDidModifyIfManaged(MTL::Buffer* buffer, std::uint32_t offset, std::size_t bytes) {
    if (buffer->storageMode() == MTL::StorageModeManaged) {
      buffer->didModifyRange(NS::Range(offset, bytes));
    }
  }
};

MetalBackend::Buffer::Buffer(MetalBackend* owner, void* native, void* device_pointer,
                             std::size_t bytes, std::uint64_t id)
    : owner_(owner), native_(native), ptr_(device_pointer), bytes_(bytes), resource_id_(id) {}

MetalBackend::Buffer::~Buffer() { Reset(); }

MetalBackend::Buffer::Buffer(Buffer&& other) noexcept
    : owner_(other.owner_),
      native_(other.native_),
      ptr_(other.ptr_),
      bytes_(other.bytes_),
      resource_id_(other.resource_id_) {
  other.owner_       = nullptr;
  other.native_      = nullptr;
  other.ptr_         = nullptr;
  other.bytes_       = 0;
  other.resource_id_ = 0;
}

auto MetalBackend::Buffer::operator=(Buffer&& other) noexcept -> Buffer& {
  if (this != &other) {
    Reset();
    owner_             = other.owner_;
    native_            = other.native_;
    ptr_               = other.ptr_;
    bytes_             = other.bytes_;
    resource_id_       = other.resource_id_;
    other.owner_       = nullptr;
    other.native_      = nullptr;
    other.ptr_         = nullptr;
    other.bytes_       = 0;
    other.resource_id_ = 0;
  }
  return *this;
}

void MetalBackend::Buffer::Reset() noexcept {
  if (native_ != nullptr) {
    auto* buffer = static_cast<MTL::Buffer*>(native_);
    if (owner_ != nullptr && owner_->gpu_) {
      owner_->gpu_->UnregisterBuffer(buffer);
      owner_->NoteFree();
    }
    buffer->release();
  }
  owner_       = nullptr;
  native_      = nullptr;
  ptr_         = nullptr;
  bytes_       = 0;
  resource_id_ = 0;
}

MetalBackend::Texture2D::Texture2D(MetalBackend* owner, void* native, std::size_t bytes,
                                   std::uint32_t width, std::uint32_t height, TextureFormat format,
                                   std::uint64_t id)
    : owner_(owner),
      native_(native),
      bytes_(bytes),
      width_(width),
      height_(height),
      format_(format),
      resource_id_(id) {}

MetalBackend::Texture2D::~Texture2D() { Reset(); }

MetalBackend::Texture2D::Texture2D(Texture2D&& other) noexcept
    : owner_(other.owner_),
      native_(other.native_),
      bytes_(other.bytes_),
      width_(other.width_),
      height_(other.height_),
      format_(other.format_),
      resource_id_(other.resource_id_) {
  other.owner_       = nullptr;
  other.native_      = nullptr;
  other.bytes_       = 0;
  other.width_       = 0;
  other.height_      = 0;
  other.resource_id_ = 0;
}

auto MetalBackend::Texture2D::operator=(Texture2D&& other) noexcept -> Texture2D& {
  if (this != &other) {
    Reset();
    owner_             = other.owner_;
    native_            = other.native_;
    bytes_             = other.bytes_;
    width_             = other.width_;
    height_            = other.height_;
    format_            = other.format_;
    resource_id_       = other.resource_id_;
    other.owner_       = nullptr;
    other.native_      = nullptr;
    other.bytes_       = 0;
    other.width_       = 0;
    other.height_      = 0;
    other.resource_id_ = 0;
  }
  return *this;
}

void MetalBackend::Texture2D::Reset() noexcept {
  if (native_ != nullptr) {
    if (owner_ != nullptr) {
      owner_->NoteFree();
    }
    static_cast<MTL::Texture*>(native_)->release();
  }
  owner_       = nullptr;
  native_      = nullptr;
  bytes_       = 0;
  width_       = 0;
  height_      = 0;
  resource_id_ = 0;
}

MetalBackend::MetalBackend() : gpu_(std::make_unique<Gpu>()) { MetalBackendImpl::AttachGpu(*gpu_); }
MetalBackend::~MetalBackend() = default;

auto MetalBackend::CreateBuffer(std::size_t bytes) -> Buffer {
  if (bytes == 0) {
    return {};
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  auto* native = gpu_->device->newBuffer(bytes, ParameterResourceOptions(gpu_->device));
  ThrowIfNull(native, "MetalBackend: failed to allocate parameter buffer");
  NoteBufferCreate();
  gpu_->live_buffers.push_back(LiveBuffer{native, native->gpuAddress(), bytes});
  return Buffer{this, native, native->contents(), bytes, next_resource_id_++};
}

auto MetalBackend::CreateSlab(std::size_t bytes) -> Buffer {
  if (bytes == 0) {
    return {};
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  auto* native = MetalBackendImpl::AllocatePrivateBuffer(*this, *gpu_, bytes);
  NoteBufferCreate();
  gpu_->live_buffers.push_back(LiveBuffer{native, native->gpuAddress(), bytes});
  return Buffer{this, native, reinterpret_cast<void*>(static_cast<uintptr_t>(native->gpuAddress())),
                bytes, next_resource_id_++};
}

auto MetalBackend::CreateTexture2D(std::uint32_t width, std::uint32_t height, TextureFormat format)
    -> Texture2D {
  const auto bytes = static_cast<std::size_t>(width) * height * TextureFormatBytesPerPixel(format);
  if (bytes == 0) {
    return {};
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  auto  descriptor = MakePrivateTextureDescriptor(width, height, format);
  auto* native     = MetalBackendImpl::AllocatePrivateTexture(*this, *gpu_, descriptor.get());
  NoteTextureCreate();
  return Texture2D{this, native, bytes, width, height, format, next_resource_id_++};
}

void MetalBackend::UploadBufferRange(Buffer& buffer, std::uint32_t offset,
                                     std::span<const std::byte> bytes,
                                     CommandContext&            command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("MetalBackend::UploadBufferRange: injected failure");
  }
  if (bytes.empty()) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + bytes.size() > buffer.Bytes()) {
    throw std::runtime_error("MetalBackend::UploadBufferRange: range exceeds buffer");
  }
  auto* native = static_cast<MTL::Buffer*>(buffer.Native());
  if (native->contents() != nullptr) {
    std::memcpy(static_cast<std::byte*>(native->contents()) + offset, bytes.data(), bytes.size());
    MetalBackendImpl::MarkDidModifyIfManaged(native, offset, bytes.size());
  } else {
    MetalBackendImpl::AttachGpu(*gpu_);
    MetalBackendImpl::EnsureStaging(*this, *gpu_, bytes.size());
    std::memcpy(gpu_->staging->contents(), bytes.data(), bytes.size());
    MetalBackendImpl::EnsureBlitEncoder(*this, *gpu_, command_context);
    command_context.gpu_->blit->copyFromBuffer(gpu_->staging.get(), 0, native, offset,
                                               bytes.size());
  }
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
  last_h2d_ranges_.push_back(ByteRange{offset, static_cast<std::uint32_t>(bytes.size())});
}

void MetalBackend::DownloadBufferRange(const Buffer& buffer, std::uint32_t offset,
                                       std::span<std::byte> out, CommandContext& command_context) {
  if (out.empty()) {
    return;
  }
  if (buffer.Empty() || static_cast<std::size_t>(offset) + out.size() > buffer.Bytes()) {
    throw std::runtime_error("MetalBackend::DownloadBufferRange: range exceeds buffer");
  }
  if (HasInFlightSubmission()) {
    Wait(command_context);
  }
  auto* native = static_cast<MTL::Buffer*>(buffer.Native());
  if (native->contents() == nullptr) {
    throw std::runtime_error("MetalBackend::DownloadBufferRange: private buffer host map missing");
  }
  std::memcpy(out.data(), static_cast<const std::byte*>(native->contents()) + offset, out.size());
}

void MetalBackend::UploadTexture2D(Texture2D& texture, std::span<const std::byte> bytes,
                                   CommandContext& command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("MetalBackend::UploadTexture2D: injected failure");
  }
  if (texture.Native() == nullptr) {
    throw std::runtime_error("MetalBackend::UploadTexture2D: empty texture");
  }
  if (bytes.size() != texture.Bytes()) {
    throw std::runtime_error("MetalBackend::UploadTexture2D: size does not match texture");
  }
  if (bytes.empty()) {
    return;
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  const auto row_bytes = AlignedRowBytes(texture.Width(), texture.Format());
  const auto staging   = row_bytes * texture.Height();
  MetalBackendImpl::EnsureStaging(*this, *gpu_, staging);
  CopyPackedToAligned(bytes, texture.Width(), texture.Height(), texture.Format(),
                      gpu_->staging->contents());
  MetalBackendImpl::EnsureBlitEncoder(*this, *gpu_, command_context);
  command_context.gpu_->blit->copyFromBuffer(
      gpu_->staging.get(), 0, row_bytes, staging, MTL::Size{texture.Width(), texture.Height(), 1},
      static_cast<MTL::Texture*>(texture.Native()), 0, 0, MTL::Origin{0, 0, 0});
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
}

void MetalBackend::CopyTexture2D(const Texture2D& src, Texture2D& dst,
                                 CommandContext& command_context) {
  if (src.Native() == nullptr || dst.Native() == nullptr) {
    throw std::runtime_error("MetalBackend::CopyTexture2D: empty texture");
  }
  if (src.Width() != dst.Width() || src.Height() != dst.Height() || src.Format() != dst.Format()) {
    throw std::runtime_error("MetalBackend::CopyTexture2D: size or format mismatch");
  }
  if (src.Bytes() == 0) {
    return;
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  MetalBackendImpl::EnsureBlitEncoder(*this, *gpu_, command_context);
  command_context.gpu_->blit->copyFromTexture(static_cast<MTL::Texture*>(src.Native()),
                                              static_cast<MTL::Texture*>(dst.Native()));
}

void MetalBackend::UploadR8TextureRect(Texture2D& texture, RectI rectangle,
                                       std::span<const std::byte> bytes,
                                       CommandContext&            command_context) {
  if (texture.Format() != TextureFormat::R8 || rectangle.x < 0 || rectangle.y < 0 ||
      rectangle.width <= 0 || rectangle.height <= 0 ||
      rectangle.X1() > static_cast<std::int32_t>(texture.Width()) ||
      rectangle.Y1() > static_cast<std::int32_t>(texture.Height()) ||
      bytes.size() != static_cast<std::size_t>(rectangle.width) * rectangle.height) {
    throw std::runtime_error("MetalBackend::UploadR8TextureRect: invalid rectangle");
  }
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("MetalBackend::UploadR8TextureRect: injected failure");
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  const auto width     = static_cast<std::uint32_t>(rectangle.width);
  const auto height    = static_cast<std::uint32_t>(rectangle.height);
  const auto row_bytes = AlignedRowBytes(width, TextureFormat::R8);
  const auto staging   = row_bytes * height;
  MetalBackendImpl::EnsureStaging(*this, *gpu_, staging);
  CopyPackedToAligned(bytes, width, height, TextureFormat::R8, gpu_->staging->contents());
  MetalBackendImpl::EnsureBlitEncoder(*this, *gpu_, command_context);
  command_context.gpu_->blit->copyFromBuffer(
      gpu_->staging.get(), 0, row_bytes, staging, MTL::Size{width, height, 1},
      static_cast<MTL::Texture*>(texture.Native()), 0, 0,
      MTL::Origin{static_cast<NS::UInteger>(rectangle.x), static_cast<NS::UInteger>(rectangle.y),
                  0});
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
  last_texture_rectangles_.push_back(rectangle);
}

void MetalBackend::DownloadTexture2D(const Texture2D& texture, std::span<std::byte> out,
                                     CommandContext& command_context) {
  if (texture.Native() == nullptr) {
    throw std::runtime_error("MetalBackend::DownloadTexture2D: empty texture");
  }
  if (out.size() != texture.Bytes()) {
    throw std::runtime_error("MetalBackend::DownloadTexture2D: size does not match texture");
  }
  if (out.empty()) {
    return;
  }
  if (HasInFlightSubmission()) {
    Wait(command_context);
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  const auto row_bytes = AlignedRowBytes(texture.Width(), texture.Format());
  const auto staging   = row_bytes * texture.Height();
  MetalBackendImpl::EnsureStaging(*this, *gpu_, staging);
  MetalBackendImpl::EnsureBlitEncoder(*this, *gpu_, command_context);
  command_context.gpu_->blit->copyFromTexture(
      static_cast<MTL::Texture*>(texture.Native()), 0, 0, MTL::Origin{0, 0, 0},
      MTL::Size{texture.Width(), texture.Height(), 1}, gpu_->staging.get(), 0, row_bytes, staging);
  command_context.gpu_->EndEncoders();
  if (command_context.gpu_->buffer) {
    command_context.gpu_->buffer->commit();
    command_context.gpu_->buffer->waitUntilCompleted();
    command_context.gpu_->buffer.reset();
  }
  CopyAlignedToPacked(gpu_->staging->contents(), texture.Width(), texture.Height(),
                      texture.Format(), out);
}

void MetalBackend::UploadDeviceMemory(void* dst, std::span<const std::byte> bytes,
                                      CommandContext& command_context) {
  if (fail_next_upload_) {
    fail_next_upload_ = false;
    throw std::runtime_error("MetalBackend::UploadDeviceMemory: injected failure");
  }
  if (bytes.empty()) {
    return;
  }
  if (dst == nullptr) {
    throw std::runtime_error("MetalBackend::UploadDeviceMemory: null destination");
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  const auto        address = reinterpret_cast<std::uint64_t>(dst);
  const LiveBuffer* found   = nullptr;
  for (const auto& entry : gpu_->live_buffers) {
    if (address >= entry.gpu_address && address + bytes.size() <= entry.gpu_address + entry.bytes) {
      found = &entry;
      break;
    }
  }
  if (found == nullptr) {
    throw std::runtime_error("MetalBackend::UploadDeviceMemory: destination is not a live buffer");
  }
  const auto offset = static_cast<std::uint32_t>(address - found->gpu_address);
  if (found->native->contents() != nullptr) {
    std::memcpy(static_cast<std::byte*>(found->native->contents()) + offset, bytes.data(),
                bytes.size());
    MetalBackendImpl::MarkDidModifyIfManaged(found->native, offset, bytes.size());
  } else {
    MetalBackendImpl::EnsureStaging(*this, *gpu_, bytes.size());
    std::memcpy(gpu_->staging->contents(), bytes.data(), bytes.size());
    MetalBackendImpl::EnsureBlitEncoder(*this, *gpu_, command_context);
    command_context.gpu_->blit->copyFromBuffer(gpu_->staging.get(), 0, found->native, offset,
                                               bytes.size());
  }
  ++h2d_copy_count_;
  h2d_bytes_ += bytes.size();
}

auto MetalBackend::ResolveDeviceMemory(void* device_pointer, std::size_t bytes) const
    -> std::pair<void*, std::uint32_t> {
  if (device_pointer == nullptr || bytes == 0 || !gpu_) {
    throw std::runtime_error("MetalBackend::ResolveDeviceMemory: invalid destination");
  }
  const auto address = reinterpret_cast<std::uint64_t>(device_pointer);
  for (const auto& entry : gpu_->live_buffers) {
    if (address >= entry.gpu_address && address + bytes <= entry.gpu_address + entry.bytes) {
      return {entry.native, static_cast<std::uint32_t>(address - entry.gpu_address)};
    }
  }
  throw std::runtime_error("MetalBackend::ResolveDeviceMemory: destination is not a live buffer");
}

void MetalBackend::FillDeviceMemory(void* dst, std::size_t bytes, std::uint8_t value,
                                    CommandContext& command_context) {
  if (bytes == 0) {
    return;
  }
  if (dst == nullptr) {
    throw std::runtime_error("MetalBackend::FillDeviceMemory: null destination");
  }
  MetalBackendImpl::AttachGpu(*gpu_);
  const auto        address = reinterpret_cast<std::uint64_t>(dst);
  const LiveBuffer* found   = nullptr;
  for (const auto& entry : gpu_->live_buffers) {
    if (address >= entry.gpu_address && address + bytes <= entry.gpu_address + entry.bytes) {
      found = &entry;
      break;
    }
  }
  if (found == nullptr) {
    throw std::runtime_error("MetalBackend::FillDeviceMemory: destination is not a live buffer");
  }
  const auto offset = static_cast<NS::UInteger>(address - found->gpu_address);
  MetalBackendImpl::EnsureBlitEncoder(*this, *gpu_, command_context);
  command_context.gpu_->blit->fillBuffer(found->native, NS::Range(offset, bytes), value);
}

void MetalBackend::CopyDeviceMemoryToBuffer(void* src, Buffer& dst, std::uint32_t dst_offset,
                                            std::size_t bytes, CommandContext& command_context) {
  if (bytes == 0) {
    return;
  }
  if (src == nullptr || dst.Empty() || dst.Native() == nullptr) {
    throw std::runtime_error("MetalBackend::CopyDeviceMemoryToBuffer: null source or destination");
  }
  if (static_cast<std::size_t>(dst_offset) + bytes > dst.Bytes()) {
    throw std::runtime_error("MetalBackend::CopyDeviceMemoryToBuffer: range exceeds destination");
  }
  const auto resolved = ResolveDeviceMemory(src, bytes);
  MetalBackendImpl::AttachGpu(*gpu_);
  MetalBackendImpl::EnsureBlitEncoder(*this, *gpu_, command_context);
  command_context.gpu_->blit->copyFromBuffer(
      static_cast<MTL::Buffer*>(resolved.first), resolved.second,
      static_cast<MTL::Buffer*>(dst.Native()), dst_offset, bytes);
}

auto MetalBackend::EnsureComputeCommandEncoder(CommandContext& command_context) -> void* {
  MetalBackendImpl::AttachGpu(*gpu_);
  MetalBackendImpl::EnsureComputeEncoder(*this, *gpu_, command_context);
  return command_context.gpu_->compute.get();
}

void MetalBackend::EndCommandEncoders(CommandContext& command_context) {
  if (command_context.gpu_) {
    command_context.gpu_->EndEncoders();
  }
}

void MetalBackend::Submit(CommandContext& command_context) {
  MetalBackendImpl::AttachGpu(*gpu_);
  MetalBackendImpl::EnsureCommandBuffer(*this, *gpu_, command_context);
  command_context.gpu_->EndEncoders();
  auto*      command_buffer = command_context.gpu_->buffer.get();
  const auto submission_id  = command_context.SubmissionId();
  command_buffer->addCompletedHandler(^(MTL::CommandBuffer*) {
    (void)submission_id;
  });
  command_buffer->commit();
  in_flight_submission_ = submission_id;
}

void MetalBackend::SynchronizeRecordedWork(CommandContext& command_context) {
  if (command_context.gpu_ == nullptr || command_context.gpu_->buffer.get() == nullptr) {
    return;
  }
  command_context.gpu_->EndEncoders();
  auto* command_buffer = command_context.gpu_->buffer.get();
  command_buffer->commit();
  command_buffer->waitUntilCompleted();
  if (command_buffer->status() == MTL::CommandBufferStatusError) {
    std::string message = "MetalBackend: Develop scratch wait failed";
    if (auto* error = command_buffer->error(); error != nullptr) {
      if (auto* description = error->localizedDescription(); description != nullptr) {
        message += ": ";
        message += description->utf8String();
      }
    }
    command_context.gpu_->Reset();
    throw std::runtime_error(message);
  }
  command_context.gpu_->Reset();
}

void MetalBackend::Wait(CommandContext& command_context) {
  if (in_flight_submission_ == 0) {
    if (command_context.gpu_) {
      command_context.gpu_->Reset();
    }
    return;
  }
  auto* command_buffer = command_context.gpu_ ? command_context.gpu_->buffer.get() : nullptr;
  if (command_buffer != nullptr) {
    command_buffer->waitUntilCompleted();
    if (command_buffer->status() == MTL::CommandBufferStatusError) {
      std::string message = "MetalBackend: command buffer failed";
      if (auto* error = command_buffer->error(); error != nullptr) {
        if (auto* description = error->localizedDescription(); description != nullptr) {
          message += ": ";
          message += description->utf8String();
        }
      }
      command_context.gpu_->Reset();
      in_flight_submission_ = 0;
      throw std::runtime_error(message);
    }
  }
  completed_submission_ = in_flight_submission_;
  in_flight_submission_ = 0;
  if (command_context.gpu_) {
    command_context.gpu_->Reset();
  }
}

void MetalBackend::WarmUpPipelines(std::span<const MetalPipelineWarmup> pipelines) {
  MetalBackendImpl::AttachGpu(*gpu_);
  auto& cache = metal::ComputePipelineCache::Instance();
  for (const auto& pipeline : pipelines) {
    const char* label = pipeline.debug_label != nullptr ? pipeline.debug_label : "MetalBackend";
    (void)cache.GetPipelineState(pipeline.metallib_path, pipeline.function_name, label);
  }
}

void MetalBackend::WarmUpPlan(const ExecutionPlan& plan) { WarmUpMetalDagPlan(*this, plan); }

auto MetalBackend::AcquireLut(ContentKey key, std::span<const std::byte> packed_rgba,
                              std::uint32_t edge, CommandContext&) -> MetalLutBinding {
  if (key.Empty() || edge <= 1 || packed_rgba.empty()) {
    return DummyLut();
  }
  for (auto& entry : lut_cache_) {
    if (entry.key == key && entry.edge_size == edge) {
      last_lut_resource_id_ = entry.buffer.ResourceId();
      return MetalLutBinding{entry.buffer.Native(), entry.buffer.ResourceId(), entry.edge_size};
    }
  }
  if (lut_byte_budget_ > 0 && lut_cache_bytes_ + packed_rgba.size() > lut_byte_budget_) {
    while (!lut_cache_.empty() && lut_cache_bytes_ + packed_rgba.size() > lut_byte_budget_) {
      lut_cache_bytes_ -= lut_cache_.front().bytes;
      lut_cache_.erase(lut_cache_.begin());
    }
  }
  auto buffer = CreateBuffer(packed_rgba.size());
  if (buffer.Empty() || buffer.DevicePointer() == nullptr) {
    throw std::runtime_error("MetalBackend::AcquireLut: failed to allocate LUT buffer");
  }
  std::memcpy(buffer.DevicePointer(), packed_rgba.data(), packed_rgba.size());
  MetalBackendImpl::MarkDidModifyIfManaged(static_cast<MTL::Buffer*>(buffer.Native()), 0,
                                           packed_rgba.size());
  lut_upload_bytes_ += packed_rgba.size();
  last_lut_resource_id_ = buffer.ResourceId();
  LutCacheEntry entry;
  entry.key       = key;
  entry.edge_size = edge;
  entry.bytes     = packed_rgba.size();
  entry.buffer    = std::move(buffer);
  lut_cache_bytes_ += entry.bytes;
  lut_cache_.push_back(std::move(entry));
  return MetalLutBinding{lut_cache_.back().buffer.Native(), last_lut_resource_id_, edge};
}

auto MetalBackend::DummyLut() -> MetalLutBinding {
  if (dummy_lut_.Empty()) {
    dummy_lut_ = CreateBuffer(16);
    if (dummy_lut_.DevicePointer() != nullptr) {
      std::memset(dummy_lut_.DevicePointer(), 0, dummy_lut_.Bytes());
    }
  }
  return MetalLutBinding{dummy_lut_.Native(), dummy_lut_.ResourceId(), 0};
}

void MetalBackend::SetLutByteBudget(std::size_t bytes) { lut_byte_budget_ = bytes; }

void MetalBackend::ResetCounters() {
  malloc_count_                = 0;
  free_count_                  = 0;
  buffer_create_count_         = 0;
  texture_create_count_        = 0;
  heap_create_count_           = 0;
  command_buffer_create_count_ = 0;
  h2d_copy_count_              = 0;
  h2d_bytes_                   = 0;
  compute_dispatch_count_      = 0;
  lut_upload_bytes_            = 0;
  last_h2d_ranges_.clear();
  last_texture_rectangles_.clear();
  if (gpu_) {
    const auto stats               = metal::ComputePipelineCache::Instance().GetStats();
    gpu_->pipeline_create_baseline = stats.creates;
    gpu_->pipeline_hit_baseline    = stats.hits;
  }
}

void MetalBackend::FailNextUpload() { fail_next_upload_ = true; }

auto MetalBackend::NativeDevice() const -> void* {
  return gpu_ ? static_cast<void*>(gpu_->device) : nullptr;
}
auto MetalBackend::NativeQueue() const -> void* {
  return gpu_ ? static_cast<void*>(gpu_->queue) : nullptr;
}
auto MetalBackend::WorkingSetBudgetBytes() const -> std::size_t {
  constexpr std::size_t kFloor = 256ull << 20;
  if (gpu_ == nullptr || gpu_->device == nullptr) {
    return kFloor;
  }
  const auto recommended = static_cast<std::size_t>(gpu_->device->recommendedMaxWorkingSetSize());
  const auto usable      = recommended - recommended / 4;
  return usable > kFloor ? usable : kFloor;
}

auto MetalBackend::QueryDeviceMemory() const -> GpuDeviceMemorySnapshot {
  GpuDeviceMemorySnapshot snapshot;
  if (gpu_ == nullptr || gpu_->device == nullptr) {
    return snapshot;
  }
  snapshot.total_bytes = static_cast<std::size_t>(gpu_->device->recommendedMaxWorkingSetSize());
  const auto allocated = static_cast<std::size_t>(gpu_->device->currentAllocatedSize());
  snapshot.free_bytes =
      snapshot.total_bytes > allocated ? snapshot.total_bytes - allocated : 0;
  snapshot.valid = snapshot.total_bytes > 0;
  return snapshot;
}
auto MetalBackend::PipelineCreateCount() const -> std::uint64_t {
  const auto creates  = metal::ComputePipelineCache::Instance().GetStats().creates;
  const auto baseline = gpu_ ? gpu_->pipeline_create_baseline : 0;
  return creates > baseline ? creates - baseline : 0;
}
auto MetalBackend::PipelineHitCount() const -> std::uint64_t {
  const auto hits     = metal::ComputePipelineCache::Instance().GetStats().hits;
  const auto baseline = gpu_ ? gpu_->pipeline_hit_baseline : 0;
  return hits > baseline ? hits - baseline : 0;
}

auto BindSystemDefaultMetalPresentationDevice() -> void* {
  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("BindSystemDefaultMetalPresentationDevice: no Metal device");
  }
  MetalContext::BindPresentationDevice(device);
  return device;
}

auto MetalPresentationDeviceHandle() -> void* { return MetalContext::Instance().Device(); }

}  // namespace alcedo
