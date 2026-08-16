//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL
#include "image/metal_image.hpp"

#include "metal/metal_context.hpp"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace alcedo {
namespace metal {
namespace {

constexpr size_t kRowAlignmentBytes = 256;

auto ToMetalPixelFormat(PixelFormat format) -> MTL::PixelFormat {
  switch (format) {
    case PixelFormat::R16UINT:
      return MTL::PixelFormatR16Uint;
    case PixelFormat::RGBA16UINT:
      return MTL::PixelFormatRGBA16Uint;
    case PixelFormat::R16FLOAT:
      return MTL::PixelFormatR16Float;
    case PixelFormat::RGBA16FLOAT:
      return MTL::PixelFormatRGBA16Float;
    case PixelFormat::R32FLOAT:
      return MTL::PixelFormatR32Float;
    case PixelFormat::RGBA32FLOAT:
      return MTL::PixelFormatRGBA32Float;
  }

  throw std::invalid_argument("MetalImage: Unsupported pixel format.");
}

auto FromMetalPixelFormat(MTL::PixelFormat format) -> PixelFormat {
  switch (format) {
    case MTL::PixelFormatR16Uint:
      return PixelFormat::R16UINT;
    case MTL::PixelFormatRGBA16Uint:
      return PixelFormat::RGBA16UINT;
    case MTL::PixelFormatR16Float:
      return PixelFormat::R16FLOAT;
    case MTL::PixelFormatRGBA16Float:
      return PixelFormat::RGBA16FLOAT;
    case MTL::PixelFormatR32Float:
      return PixelFormat::R32FLOAT;
    case MTL::PixelFormatRGBA32Float:
      return PixelFormat::RGBA32FLOAT;
    default:
      throw std::invalid_argument("MetalImage: Unsupported Metal texture pixel format.");
  }
}

auto BuildUsageFlags(bool shader_read, bool shader_write, bool render_target) -> MTL::TextureUsage {
  uint32_t usage = static_cast<uint32_t>(MTL::TextureUsageUnknown);

  if (shader_read) {
    usage |= static_cast<uint32_t>(MTL::TextureUsageShaderRead);
  }
  if (shader_write) {
    usage |= static_cast<uint32_t>(MTL::TextureUsageShaderWrite);
  }
  if (render_target) {
    usage |= static_cast<uint32_t>(MTL::TextureUsageRenderTarget);
  }

  return static_cast<MTL::TextureUsage>(usage);
}

void Validate2DTexture(MTL::Texture* texture) {
  if (texture == nullptr) {
    throw std::invalid_argument("MetalImage: Cannot wrap a null Metal texture.");
  }
  if (texture->textureType() != MTL::TextureType2D) {
    throw std::invalid_argument("MetalImage: Only 2D textures are supported.");
  }
}

auto HasUsageFlag(uint32_t usage_flags, MTL::TextureUsage usage) -> bool {
  return (usage_flags & static_cast<uint32_t>(usage)) != 0U;
}

auto AlignedRowBytes(uint32_t width, PixelFormat format) -> size_t {
  const auto raw_row_bytes =
      static_cast<size_t>(width) * CV_ELEM_SIZE(MetalImage::CVTypeFromPixelFormat(format));
  return ((raw_row_bytes + kRowAlignmentBytes - 1) / kRowAlignmentBytes) * kRowAlignmentBytes;
}

auto StagingBufferSize(uint32_t width, uint32_t height, PixelFormat format) -> size_t {
  return AlignedRowBytes(width, format) * static_cast<size_t>(height);
}

auto MakeSharedBuffer(size_t length) -> NS::SharedPtr<MTL::Buffer> {
  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("MetalImage: Metal device is unavailable.");
  }

  auto buffer = NS::TransferPtr(
      device->newBuffer(static_cast<NS::UInteger>(length), MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error("MetalImage: Failed to allocate staging buffer.");
  }
  return buffer;
}

void SubmitAndWait(const NS::SharedPtr<MTL::CommandBuffer>& command_buffer) {
  command_buffer->commit();
  command_buffer->waitUntilCompleted();
}

void CopyRowsToBuffer(const cv::Mat& host_image, void* destination, size_t destination_row_bytes) {
  const auto source_row_bytes = static_cast<size_t>(host_image.cols) * host_image.elemSize();
  auto*      dst_bytes        = static_cast<std::byte*>(destination);
  for (int row = 0; row < host_image.rows; ++row) {
    std::memcpy(dst_bytes + static_cast<size_t>(row) * destination_row_bytes, host_image.ptr(row),
                source_row_bytes);
  }
}

void CopyRowsFromBuffer(const void* source, size_t source_row_bytes, cv::Mat& host_image) {
  const auto destination_row_bytes = static_cast<size_t>(host_image.cols) * host_image.elemSize();
  auto*      src_bytes             = static_cast<const std::byte*>(source);
  for (int row = 0; row < host_image.rows; ++row) {
    std::memcpy(host_image.ptr(row), src_bytes + static_cast<size_t>(row) * source_row_bytes,
                destination_row_bytes);
  }
}

}  // namespace

MetalImage::MetalImage(NS::SharedPtr<MTL::Texture>&& texture_owner, uint32_t width,
                       uint32_t height, PixelFormat format, MTL::TextureUsage usage) noexcept
    : width_(width),
      height_(height),
      usage_flags_(static_cast<uint32_t>(usage)),
      format_(format),
      texture_owner_(std::move(texture_owner)) {}

auto MetalImage::Wrap(MetalTextureHandle texture) -> MetalImage {
  Validate2DTexture(texture);
  return MetalImage(NS::RetainPtr(texture), static_cast<uint32_t>(texture->width()),
                    static_cast<uint32_t>(texture->height()),
                    FromMetalPixelFormat(texture->pixelFormat()), texture->usage());
}

void MetalImage::Create(uint32_t width, uint32_t height, PixelFormat format, bool shader_read,
                        bool shader_write, bool render_target) {
  if (width == 0 || height == 0) {
    throw std::invalid_argument("MetalImage: Texture dimensions must be non-zero.");
  }

  const auto usage = BuildUsageFlags(shader_read, shader_write, render_target);
  if (!Empty() && width_ == width && height_ == height && format_ == format &&
      usage_flags_ == static_cast<uint32_t>(usage)) {
    return;
  }

  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("MetalImage: Metal device is unavailable.");
  }

  auto descriptor = NS::TransferPtr(MTL::TextureDescriptor::alloc()->init());
  descriptor->setTextureType(MTL::TextureType2D);
  descriptor->setWidth(width);
  descriptor->setHeight(height);
  descriptor->setDepth(1);
  descriptor->setArrayLength(1);
  descriptor->setMipmapLevelCount(1);
  descriptor->setSampleCount(1);
  descriptor->setPixelFormat(ToMetalPixelFormat(format));
  descriptor->setStorageMode(MTL::StorageModePrivate);
  descriptor->setUsage(usage);

  auto texture = NS::TransferPtr(device->newTexture(descriptor.get()));
  if (!texture) {
    throw std::runtime_error("MetalImage: Failed to allocate Metal texture.");
  }

  width_         = width;
  height_        = height;
  usage_flags_   = static_cast<uint32_t>(usage);
  format_        = format;
  texture_owner_ = std::move(texture);
}

void MetalImage::Upload(const cv::Mat& host_image) {
  if (host_image.empty()) {
    throw std::invalid_argument("MetalImage: Cannot upload an empty host image.");
  }

  const auto format   = PixelFormatFromCVType(host_image.type());
  const auto row_bytes = AlignedRowBytes(static_cast<uint32_t>(host_image.cols), format);
  const auto buffer_size =
      StagingBufferSize(static_cast<uint32_t>(host_image.cols), static_cast<uint32_t>(host_image.rows),
                        format);

  Create(static_cast<uint32_t>(host_image.cols), static_cast<uint32_t>(host_image.rows), format);

  auto staging_buffer = MakeSharedBuffer(buffer_size);
  CopyRowsToBuffer(host_image, staging_buffer->contents(), row_bytes);

  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("MetalImage: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  auto blit           = NS::RetainPtr(command_buffer->blitCommandEncoder());
  blit->copyFromBuffer(staging_buffer.get(), 0, row_bytes, buffer_size,
                       MTL::Size{width_, height_, 1}, Texture(), 0, 0, MTL::Origin{0, 0, 0});
  blit->endEncoding();
  SubmitAndWait(command_buffer);
}

void MetalImage::Download(cv::Mat& host_image) const {
  if (Empty()) {
    throw std::runtime_error("MetalImage: Cannot download from an empty texture.");
  }

  const auto cv_type    = CVTypeFromPixelFormat(format_);
  const auto row_bytes  = AlignedRowBytes(width_, format_);
  const auto buffer_size = StagingBufferSize(width_, height_, format_);

  host_image.create(static_cast<int>(height_), static_cast<int>(width_), cv_type);
  auto staging_buffer = MakeSharedBuffer(buffer_size);

  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("MetalImage: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  auto blit           = NS::RetainPtr(command_buffer->blitCommandEncoder());
  blit->copyFromTexture(Texture(), 0, 0, MTL::Origin{0, 0, 0}, MTL::Size{width_, height_, 1},
                        staging_buffer.get(), 0, row_bytes, buffer_size);
  blit->endEncoding();
  SubmitAndWait(command_buffer);

  CopyRowsFromBuffer(staging_buffer->contents(), row_bytes, host_image);
}

void MetalImage::CopyTo(MetalImage& dst) const {
  if (Empty()) {
    throw std::runtime_error("MetalImage: Cannot copy an empty texture.");
  }

  dst.Create(width_, height_, format_, HasUsageFlag(usage_flags_, MTL::TextureUsageShaderRead),
             HasUsageFlag(usage_flags_, MTL::TextureUsageShaderWrite),
             HasUsageFlag(usage_flags_, MTL::TextureUsageRenderTarget));

  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("MetalImage: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  auto blit           = NS::RetainPtr(command_buffer->blitCommandEncoder());
  blit->copyFromTexture(Texture(), dst.Texture());
  blit->endEncoding();
  SubmitAndWait(command_buffer);
}

void MetalImage::Release() noexcept {
  texture_owner_.reset();
  width_       = 0;
  height_      = 0;
  usage_flags_ = static_cast<uint32_t>(MTL::TextureUsageUnknown);
  format_      = PixelFormat::RGBA32FLOAT;
}

void MetalImage::Swap(MetalImage& other) noexcept {
  using std::swap;

  swap(width_, other.width_);
  swap(height_, other.height_);
  swap(usage_flags_, other.usage_flags_);
  swap(format_, other.format_);
  swap(texture_owner_, other.texture_owner_);
}

}  // namespace metal
}  // namespace alcedo
#endif
