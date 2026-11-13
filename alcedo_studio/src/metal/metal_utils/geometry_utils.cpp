//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "metal/metal_utils/geometry_utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "image/metal_image.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"

namespace alcedo::metal::utils {
namespace {

struct ResizeParams {
  uint32_t origin_x;
  uint32_t origin_y;
  uint32_t crop_width;
  uint32_t crop_height;
  uint32_t dst_width;
  uint32_t dst_height;
  float    scale_x;
  float    scale_y;
};

struct AffineParams {
  float    m00;
  float    m01;
  float    m02;
  float    m10;
  float    m11;
  float    m12;
  uint32_t src_width;
  uint32_t src_height;
  uint32_t dst_width;
  uint32_t dst_height;
  uint32_t src_stride;
  uint32_t dst_stride;
  float    border[4];
};

struct WarpRectilinearParams {
  uint32_t coefficient_set_count;
  uint32_t width;
  uint32_t height;
  uint32_t src_stride;
  uint32_t dst_stride;
  float    coefficient_sets[3][6];
  float    center_x;
  float    center_y;
};

enum class GeometryKernel : uint32_t {
  Linear,
  Area,
  WarpAffineLinear,
  WarpRectilinear,
  WarpRectilinearTex,
};

constexpr uint32_t kRowAlignmentBytes = 256;

auto AlignRowBytes(size_t row_bytes) -> size_t {
  return ((row_bytes + kRowAlignmentBytes - 1) / kRowAlignmentBytes) * kRowAlignmentBytes;
}

auto RowBytesFor(uint32_t width, PixelFormat format) -> size_t {
  const auto cv_type = MetalImage::CVTypeFromPixelFormat(format);
  return AlignRowBytes(static_cast<size_t>(width) * CV_ELEM_SIZE(cv_type));
}

auto SupportsGeometry(PixelFormat format) -> bool {
  switch (format) {
    case PixelFormat::R32FLOAT:
    case PixelFormat::RGBA32FLOAT:
      return true;
    default:
      return false;
  }
}

auto HasRenderTargetUsage(const MetalImage& image) -> bool {
  return (static_cast<uint32_t>(image.Usage()) &
          static_cast<uint32_t>(MTL::TextureUsageRenderTarget)) != 0U;
}

auto KernelNameFor(GeometryKernel kernel, PixelFormat format) -> const char* {
  switch (kernel) {
    case GeometryKernel::Linear:
      switch (format) {
        case PixelFormat::R32FLOAT:
          return "crop_resize_linear_r32f";
        case PixelFormat::RGBA32FLOAT:
          return "crop_resize_linear_rgba32f";
        default:
          return nullptr;
      }
    case GeometryKernel::Area:
      switch (format) {
        case PixelFormat::R32FLOAT:
          return "crop_resize_area_r32f";
        case PixelFormat::RGBA32FLOAT:
          return "crop_resize_area_rgba32f";
        default:
          return nullptr;
      }
    case GeometryKernel::WarpAffineLinear:
      switch (format) {
        case PixelFormat::R32FLOAT:
          return "warp_affine_linear_r32f";
        case PixelFormat::RGBA32FLOAT:
          return "warp_affine_linear_rgba32f";
        default:
          return nullptr;
      }
    case GeometryKernel::WarpRectilinear:
      switch (format) {
        case PixelFormat::RGBA32FLOAT:
          return "warp_rectilinear_rgba32f";
        default:
          return nullptr;
      }
    case GeometryKernel::WarpRectilinearTex:
      switch (format) {
        case PixelFormat::RGBA32FLOAT:
          return "warp_rectilinear_tex_rgba32f";
        default:
          return nullptr;
      }
  }

  return nullptr;
}

auto GetGeometryPipelineState(GeometryKernel kernel, PixelFormat format)
    -> NS::SharedPtr<MTL::ComputePipelineState> {
  const auto* kernel_name = KernelNameFor(kernel, format);
  if (kernel_name == nullptr) {
    throw std::runtime_error("Metal geometry utils: unsupported geometry format.");
  }

#ifndef ALCEDO_METAL_GEOMETRY_UTILS_METALLIB_PATH
  throw std::runtime_error("Metal geometry utils metallib path is not configured.");
#else
  return ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_GEOMETRY_UTILS_METALLIB_PATH, kernel_name, "Metal geometry utils");
#endif
}

auto MakeSharedBuffer(size_t length) -> NS::SharedPtr<MTL::Buffer> {
  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("Metal geometry utils: Metal device is unavailable.");
  }

  auto buffer = NS::TransferPtr(
      device->newBuffer(static_cast<NS::UInteger>(length), MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error("Metal geometry utils: failed to allocate staging buffer.");
  }
  return buffer;
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     uint32_t width, uint32_t height) {
  const auto thread_width  = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  const MTL::Size threads_per_group{thread_width, thread_height, 1};
  const MTL::Size threads_per_grid{width, height, 1};
  encoder->dispatchThreads(threads_per_grid, threads_per_group);
}

auto MakeCommandBuffer() -> NS::SharedPtr<MTL::CommandBuffer> {
  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("Metal geometry utils: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!command_buffer) {
    throw std::runtime_error("Metal geometry utils: failed to create command buffer.");
  }
  return command_buffer;
}

void EncodeTextureToBuffer(MTL::CommandBuffer* command_buffer, const MetalImage& src,
                           MTL::Buffer* buffer, size_t row_bytes, size_t buffer_size) {
  auto blit = NS::RetainPtr(command_buffer->blitCommandEncoder());
  blit->copyFromTexture(src.Texture(), 0, 0, MTL::Origin{0, 0, 0},
                        MTL::Size{src.Width(), src.Height(), 1}, buffer, 0, row_bytes, buffer_size);
  blit->endEncoding();
}

void EncodeBufferToTexture(MTL::CommandBuffer* command_buffer, const MetalImage& dst,
                           MTL::Buffer* buffer, size_t row_bytes, size_t buffer_size) {
  auto blit = NS::RetainPtr(command_buffer->blitCommandEncoder());
  blit->copyFromBuffer(buffer, 0, row_bytes, buffer_size, MTL::Size{dst.Width(), dst.Height(), 1},
                       dst.Texture(), 0, 0, MTL::Origin{0, 0, 0});
  blit->endEncoding();
}

void ValidateCropRect(const MetalImage& src, const cv::Rect& crop_rect) {
  if (crop_rect.width <= 0 || crop_rect.height <= 0) {
    throw std::runtime_error("Metal geometry utils: crop rectangle must be non-empty.");
  }
  if (crop_rect.x < 0 || crop_rect.y < 0 ||
      crop_rect.x + crop_rect.width > static_cast<int>(src.Width()) ||
      crop_rect.y + crop_rect.height > static_cast<int>(src.Height())) {
    throw std::runtime_error("Metal geometry utils: crop rectangle is out of bounds.");
  }
}

auto NormalizeAffineMatrix(const cv::Mat& matrix) -> cv::Mat {
  cv::Mat matrix_32f;
  if (matrix.type() == CV_64F) {
    matrix.convertTo(matrix_32f, CV_32F);
  } else if (matrix.type() == CV_32F) {
    matrix_32f = matrix;
  } else {
    throw std::runtime_error("Metal geometry utils: affine matrix type must be CV_32F or CV_64F.");
  }

  if (matrix_32f.rows != 2 || matrix_32f.cols != 3) {
    throw std::runtime_error("Metal geometry utils: affine matrix must be 2x3.");
  }
  return matrix_32f;
}

auto MakeBorderArray(const cv::Scalar& border_value) -> std::array<float, 4> {
  return {static_cast<float>(border_value[0]), static_cast<float>(border_value[1]),
          static_cast<float>(border_value[2]), static_cast<float>(border_value[3])};
}

void DispatchCropResize(const MetalImage& src, MetalImage& dst, const cv::Rect& crop_rect,
                        ResizeDownsampleAlgorithm downsample_algorithm) {
  auto command_buffer = MakeCommandBuffer();

  const ResizeParams params{
      .origin_x    = static_cast<uint32_t>(crop_rect.x),
      .origin_y    = static_cast<uint32_t>(crop_rect.y),
      .crop_width  = static_cast<uint32_t>(crop_rect.width),
      .crop_height = static_cast<uint32_t>(crop_rect.height),
      .dst_width   = dst.Width(),
      .dst_height  = dst.Height(),
      .scale_x     = static_cast<float>(crop_rect.width) / static_cast<float>(dst.Width()),
      .scale_y     = static_cast<float>(crop_rect.height) / static_cast<float>(dst.Height()),
  };

  const auto mode = (params.scale_x <= 1.0f || params.scale_y <= 1.0f ||
                     downsample_algorithm == ResizeDownsampleAlgorithm::Bilinear)
                        ? GeometryKernel::Linear
                        : GeometryKernel::Area;

  {
    auto pipeline = GetGeometryPipelineState(mode, src.Format());
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());

    compute->setComputePipelineState(pipeline.get());
    compute->setTexture(src.Texture(), 0);
    compute->setTexture(dst.Texture(), 1);
    compute->setBytes(&params, sizeof(params), 0);
    DispatchThreads(compute.get(), pipeline.get(), dst.Width(), dst.Height());
    compute->endEncoding();
  }

  command_buffer->commit();
  command_buffer->waitUntilCompleted();
}

void DispatchWarpAffine(const MetalImage& src, MetalImage& dst, const cv::Mat& matrix,
                        const cv::Scalar& border_value) {
  const auto src_row_bytes = RowBytesFor(src.Width(), src.Format());
  const auto dst_row_bytes = RowBytesFor(dst.Width(), dst.Format());
  const auto src_size      = src_row_bytes * src.Height();
  const auto dst_size      = dst_row_bytes * dst.Height();

  auto src_buffer = MakeSharedBuffer(src_size);
  auto dst_buffer = MakeSharedBuffer(dst_size);

  auto command_buffer = MakeCommandBuffer();
  const auto matrix_32f = NormalizeAffineMatrix(matrix);
  const auto border     = MakeBorderArray(border_value);

  const AffineParams params{
      .m00        = matrix_32f.at<float>(0, 0),
      .m01        = matrix_32f.at<float>(0, 1),
      .m02        = matrix_32f.at<float>(0, 2),
      .m10        = matrix_32f.at<float>(1, 0),
      .m11        = matrix_32f.at<float>(1, 1),
      .m12        = matrix_32f.at<float>(1, 2),
      .src_width  = src.Width(),
      .src_height = src.Height(),
      .dst_width  = dst.Width(),
      .dst_height = dst.Height(),
      .src_stride = static_cast<uint32_t>(
          src_row_bytes / CV_ELEM_SIZE(MetalImage::CVTypeFromPixelFormat(src.Format()))),
      .dst_stride = static_cast<uint32_t>(
          dst_row_bytes / CV_ELEM_SIZE(MetalImage::CVTypeFromPixelFormat(dst.Format()))),
      .border     = {border[0], border[1], border[2], border[3]},
  };

  EncodeTextureToBuffer(command_buffer.get(), src, src_buffer.get(), src_row_bytes, src_size);

  {
    auto pipeline = GetGeometryPipelineState(GeometryKernel::WarpAffineLinear, src.Format());
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());

    compute->setComputePipelineState(pipeline.get());
    compute->setBuffer(src_buffer.get(), 0, 0);
    compute->setBuffer(dst_buffer.get(), 0, 1);
    compute->setBytes(&params, sizeof(params), 2);
    DispatchThreads(compute.get(), pipeline.get(), dst.Width(), dst.Height());
    compute->endEncoding();
  }

  EncodeBufferToTexture(command_buffer.get(), dst, dst_buffer.get(), dst_row_bytes, dst_size);

  command_buffer->commit();
  command_buffer->waitUntilCompleted();
}

auto MakeWarpRectilinearParams(const dng::WarpRectilinear& warp, uint32_t width, uint32_t height)
    -> WarpRectilinearParams {
  WarpRectilinearParams params{
      .coefficient_set_count = warp.coefficient_set_count,
      .width                 = width,
      .height                = height,
      .src_stride            = width,
      .dst_stride            = width,
      .center_x              = static_cast<float>(warp.center_x),
      .center_y              = static_cast<float>(warp.center_y),
  };
  for (size_t set = 0; set < warp.coefficient_sets.size(); ++set) {
    for (size_t term = 0; term < warp.coefficient_sets[set].size(); ++term) {
      params.coefficient_sets[set][term] = static_cast<float>(warp.coefficient_sets[set][term]);
    }
  }
  return params;
}

void DispatchWarpRectilinear(const MetalImage& src, MetalImage& dst,
                             const dng::WarpRectilinear& warp) {
  const auto src_row_bytes = RowBytesFor(src.Width(), src.Format());
  const auto dst_row_bytes = RowBytesFor(dst.Width(), dst.Format());
  const auto src_size      = src_row_bytes * src.Height();
  const auto dst_size      = dst_row_bytes * dst.Height();

  auto src_buffer = MakeSharedBuffer(src_size);
  auto dst_buffer = MakeSharedBuffer(dst_size);

  auto command_buffer = MakeCommandBuffer();
  WarpRectilinearParams params{
      .coefficient_set_count = warp.coefficient_set_count,
      .width                 = src.Width(),
      .height                = src.Height(),
      .src_stride            = static_cast<uint32_t>(
          src_row_bytes / CV_ELEM_SIZE(MetalImage::CVTypeFromPixelFormat(src.Format()))),
      .dst_stride            = static_cast<uint32_t>(
          dst_row_bytes / CV_ELEM_SIZE(MetalImage::CVTypeFromPixelFormat(dst.Format()))),
      .center_x              = static_cast<float>(warp.center_x),
      .center_y              = static_cast<float>(warp.center_y),
  };
  for (size_t set = 0; set < warp.coefficient_sets.size(); ++set) {
    for (size_t term = 0; term < warp.coefficient_sets[set].size(); ++term) {
      params.coefficient_sets[set][term] = static_cast<float>(warp.coefficient_sets[set][term]);
    }
  }

  EncodeTextureToBuffer(command_buffer.get(), src, src_buffer.get(), src_row_bytes, src_size);

  {
    auto pipeline = GetGeometryPipelineState(GeometryKernel::WarpRectilinear, src.Format());
    auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());

    compute->setComputePipelineState(pipeline.get());
    compute->setBuffer(src_buffer.get(), 0, 0);
    compute->setBuffer(dst_buffer.get(), 0, 1);
    compute->setBytes(&params, sizeof(params), 2);
    DispatchThreads(compute.get(), pipeline.get(), dst.Width(), dst.Height());
    compute->endEncoding();
  }

  EncodeBufferToTexture(command_buffer.get(), dst, dst_buffer.get(), dst_row_bytes, dst_size);

  command_buffer->commit();
  command_buffer->waitUntilCompleted();
}

}  // namespace

void ResizeTexture(const MetalImage& src, MetalImage& dst, cv::Size dst_size,
                   ResizeDownsampleAlgorithm downsample_algorithm) {
  CropResizeTexture(src, dst,
                    cv::Rect(0, 0, static_cast<int>(src.Width()), static_cast<int>(src.Height())),
                    dst_size, downsample_algorithm);
}

void CropResizeTexture(const MetalImage& src, MetalImage& dst, const cv::Rect& crop_rect,
                       cv::Size dst_size, ResizeDownsampleAlgorithm downsample_algorithm) {
  if (src.Empty()) {
    throw std::runtime_error("Metal geometry utils: source texture is empty.");
  }
  if (!SupportsGeometry(src.Format())) {
    throw std::runtime_error("Metal geometry utils: unsupported image format.");
  }
  if (!dst.Empty() && src.Texture() == dst.Texture()) {
    throw std::runtime_error("Metal geometry utils: source and destination textures must differ.");
  }
  if (dst_size.width <= 0 || dst_size.height <= 0) {
    throw std::runtime_error("Metal geometry utils: destination size must be positive.");
  }

  ValidateCropRect(src, crop_rect);

  const bool is_full_source_crop =
      crop_rect.x == 0 && crop_rect.y == 0 && crop_rect.width == static_cast<int>(src.Width()) &&
      crop_rect.height == static_cast<int>(src.Height());
  if (is_full_source_crop && dst_size.width == static_cast<int>(src.Width()) &&
      dst_size.height == static_cast<int>(src.Height())) {
    src.CopyTo(dst);
    return;
  }

  dst.Create(static_cast<uint32_t>(dst_size.width), static_cast<uint32_t>(dst_size.height),
             src.Format(), true, true, HasRenderTargetUsage(src));
  DispatchCropResize(src, dst, crop_rect, downsample_algorithm);
}

void WarpAffineLinearTexture(const MetalImage& src, MetalImage& dst, const cv::Mat& matrix,
                             cv::Size out_size, const cv::Scalar& border_value) {
  if (src.Empty()) {
    throw std::runtime_error("Metal geometry utils: source texture is empty.");
  }
  if (!SupportsGeometry(src.Format())) {
    throw std::runtime_error("Metal geometry utils: unsupported image format.");
  }
  if (!dst.Empty() && src.Texture() == dst.Texture()) {
    throw std::runtime_error("Metal geometry utils: source and destination textures must differ.");
  }
  if (out_size.width <= 0 || out_size.height <= 0) {
    throw std::runtime_error("Metal geometry utils: output size must be positive.");
  }

  dst.Create(static_cast<uint32_t>(out_size.width), static_cast<uint32_t>(out_size.height),
             src.Format(), true, true, HasRenderTargetUsage(src));
  DispatchWarpAffine(src, dst, matrix, border_value);
}

void EncodeWarpRectilinearTexture(void* command_buffer, void* src_texture, void* dst_texture,
                                  const dng::WarpRectilinear& warp) {
  auto* buffer = static_cast<MTL::CommandBuffer*>(command_buffer);
  auto* src    = static_cast<MTL::Texture*>(src_texture);
  auto* dst    = static_cast<MTL::Texture*>(dst_texture);
  if (buffer == nullptr || src == nullptr || dst == nullptr) {
    throw std::runtime_error("Metal geometry utils: encode warp requires command buffer and textures.");
  }
  if (src->width() != dst->width() || src->height() != dst->height()) {
    throw std::runtime_error("Metal geometry utils: warp source and destination sizes must match.");
  }
  const auto params = MakeWarpRectilinearParams(warp, static_cast<uint32_t>(src->width()),
                                                static_cast<uint32_t>(src->height()));
  auto       pipeline = GetGeometryPipelineState(GeometryKernel::WarpRectilinearTex,
                                                 PixelFormat::RGBA32FLOAT);
  auto       compute  = NS::RetainPtr(buffer->computeCommandEncoder());
  if (!compute) {
    throw std::runtime_error("Metal geometry utils: failed to create compute encoder.");
  }
  compute->setComputePipelineState(pipeline.get());
  compute->setTexture(src, 0);
  compute->setTexture(dst, 1);
  compute->setBytes(&params, sizeof(params), 0);
  DispatchThreads(compute.get(), pipeline.get(), params.width, params.height);
  compute->endEncoding();
}

void WarpRectilinearTexture(const MetalImage& src, MetalImage& dst,
                            const dng::WarpRectilinear& warp) {
  if (src.Empty()) {
    throw std::runtime_error("Metal geometry utils: source texture is empty.");
  }
  if (src.Format() != PixelFormat::RGBA32FLOAT) {
    throw std::runtime_error("Metal geometry utils: DNG warp expects RGBA32FLOAT input.");
  }
  if (!dst.Empty() && src.Texture() == dst.Texture()) {
    throw std::runtime_error("Metal geometry utils: source and destination textures must differ.");
  }
  if (warp.coefficient_set_count == 0 || warp.coefficient_set_count > 3) {
    throw std::runtime_error("Metal geometry utils: invalid DNG warp coefficient set count.");
  }

  dst.Create(src.Width(), src.Height(), src.Format(), true, true, HasRenderTargetUsage(src));
  DispatchWarpRectilinear(src, dst, warp);
}

}  // namespace alcedo::metal::utils

#endif
