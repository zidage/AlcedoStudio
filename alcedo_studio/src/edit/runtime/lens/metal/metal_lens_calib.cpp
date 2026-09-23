//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_METAL

#include "edit/runtime/lens/metal/metal_lens_calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <utility>

#include <opencv2/core/mat.hpp>

#include "image/metal_image.hpp"
#include "metal/compute_pipeline_cache.hpp"
#include "metal/metal_context.hpp"
#include "metal/metal_utils/geometry_utils.hpp"

namespace alcedo::metal {
namespace {

struct LensCalibDispatchParams {
  LensCalibGpuParams params;
  uint32_t           src_stride;
  uint32_t           dst_stride;
};

struct CropRectPx {
  float left   = 0.0f;
  float right  = 0.0f;
  float top    = 0.0f;
  float bottom = 0.0f;
};

enum class LensKernel : uint32_t {
  Vignetting,
  Warp,
};

constexpr uint32_t kRowAlignmentBytes = 256;

auto AlignRowBytes(size_t row_bytes) -> size_t {
  return ((row_bytes + kRowAlignmentBytes - 1) / kRowAlignmentBytes) * kRowAlignmentBytes;
}

auto RowBytesFor(uint32_t width, PixelFormat format) -> size_t {
  const auto cv_type = MetalImage::CVTypeFromPixelFormat(format);
  return AlignRowBytes(static_cast<size_t>(width) * CV_ELEM_SIZE(cv_type));
}

auto HasRenderTargetUsage(const MetalImage& image) -> bool {
  return (static_cast<uint32_t>(image.Usage()) &
          static_cast<uint32_t>(MTL::TextureUsageRenderTarget)) != 0U;
}

auto KernelNameFor(LensKernel kernel) -> const char* {
  switch (kernel) {
    case LensKernel::Vignetting:
      return "lens_vignetting_rgba32f";
    case LensKernel::Warp:
      return "lens_warp_rgba32f";
  }
  return nullptr;
}

auto GetLensPipelineState(LensKernel kernel) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_LENS_CALIB_METALLIB_PATH
  throw std::runtime_error("Metal lens calibration metallib path is not configured.");
#else
  return ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_LENS_CALIB_METALLIB_PATH, KernelNameFor(kernel), "Metal lens calibration");
#endif
}

auto MakeSharedBuffer(size_t length) -> NS::SharedPtr<MTL::Buffer> {
  auto* device = MetalContext::Instance().Device();
  if (device == nullptr) {
    throw std::runtime_error("Metal lens calibration: Metal device is unavailable.");
  }

  auto buffer = NS::TransferPtr(
      device->newBuffer(static_cast<NS::UInteger>(length), MTL::ResourceStorageModeShared));
  if (!buffer) {
    throw std::runtime_error("Metal lens calibration: failed to allocate staging buffer.");
  }
  return buffer;
}

auto MakeCommandBuffer() -> NS::SharedPtr<MTL::CommandBuffer> {
  auto* queue = MetalContext::Instance().Queue();
  if (queue == nullptr) {
    throw std::runtime_error("Metal lens calibration: Metal queue is unavailable.");
  }

  auto command_buffer = NS::RetainPtr(queue->commandBuffer());
  if (!command_buffer) {
    throw std::runtime_error("Metal lens calibration: failed to create command buffer.");
  }
  return command_buffer;
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

template <typename T>
void SwapValues(T& a, T& b) {
  const T tmp = a;
  a           = b;
  b           = tmp;
}

auto ResolveCropRectPx(const LensCalibGpuParams& params) -> CropRectPx {
  CropRectPx rect{};
  const float width  = static_cast<float>(params.dst_width);
  const float height = static_cast<float>(params.dst_height);
  if (width <= 0.0f || height <= 0.0f) {
    return rect;
  }

  if (params.dst_width >= params.dst_height) {
    rect.left   = params.crop_bounds[0] * width;
    rect.right  = params.crop_bounds[1] * width;
    rect.top    = params.crop_bounds[2] * height;
    rect.bottom = params.crop_bounds[3] * height;
  } else {
    rect.left   = params.crop_bounds[2] * width;
    rect.right  = params.crop_bounds[3] * width;
    rect.top    = params.crop_bounds[0] * height;
    rect.bottom = params.crop_bounds[1] * height;
  }

  if (rect.left > rect.right) {
    SwapValues(rect.left, rect.right);
  }
  if (rect.top > rect.bottom) {
    SwapValues(rect.top, rect.bottom);
  }
  return rect;
}

auto ComputeRectCropRoi(const LensCalibGpuParams& params) -> cv::Rect {
  const int width  = params.dst_width;
  const int height = params.dst_height;
  if (width <= 0 || height <= 0) {
    return cv::Rect();
  }

  const CropRectPx rect = ResolveCropRectPx(params);

  int x0 = static_cast<int>(std::lround(rect.left));
  int x1 = static_cast<int>(std::lround(rect.right));
  int y0 = static_cast<int>(std::lround(rect.top));
  int y1 = static_cast<int>(std::lround(rect.bottom));

  if (x0 > x1) {
    std::swap(x0, x1);
  }
  if (y0 > y1) {
    std::swap(y0, y1);
  }

  x0 = std::clamp(x0, 0, width - 1);
  y0 = std::clamp(y0, 0, height - 1);
  x1 = std::clamp(x1, x0 + 1, width);
  y1 = std::clamp(y1, y0 + 1, height);
  return cv::Rect(x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0));
}

void DispatchVignetting(MTL::CommandBuffer* command_buffer, MTL::Buffer* image_buffer,
                        const LensCalibDispatchParams& params, uint32_t width, uint32_t height) {
  auto pipeline = GetLensPipelineState(LensKernel::Vignetting);
  auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());

  compute->setComputePipelineState(pipeline.get());
  compute->setBuffer(image_buffer, 0, 0);
  compute->setBytes(&params, sizeof(params), 1);
  DispatchThreads(compute.get(), pipeline.get(), width, height);
  compute->endEncoding();
}

void DispatchWarp(MTL::CommandBuffer* command_buffer, MTL::Buffer* src_buffer, MTL::Buffer* dst_buffer,
                  const LensCalibDispatchParams& params, uint32_t width, uint32_t height) {
  auto pipeline = GetLensPipelineState(LensKernel::Warp);
  auto compute  = NS::RetainPtr(command_buffer->computeCommandEncoder());

  compute->setComputePipelineState(pipeline.get());
  compute->setBuffer(src_buffer, 0, 0);
  compute->setBuffer(dst_buffer, 0, 1);
  compute->setBytes(&params, sizeof(params), 2);
  DispatchThreads(compute.get(), pipeline.get(), width, height);
  compute->endEncoding();
}

}  // namespace

void ApplyLensCalibration(MetalImage& image, const LensCalibGpuParams& params) {
  if (image.Empty()) {
    return;
  }
  if (image.Format() != PixelFormat::RGBA32FLOAT) {
    throw std::runtime_error("Metal lens calibration expects RGBA32FLOAT input.");
  }

  LensCalibGpuParams launch = params;
  if (launch.src_width <= 0 || launch.src_height <= 0) {
    launch.src_width  = static_cast<std::int32_t>(image.Width());
    launch.src_height = static_cast<std::int32_t>(image.Height());
  }
  if (launch.dst_width <= 0 || launch.dst_height <= 0) {
    launch.dst_width  = static_cast<std::int32_t>(image.Width());
    launch.dst_height = static_cast<std::int32_t>(image.Height());
  }

  const bool has_vignetting = (launch.apply_vignetting != 0);
  const bool has_warp       = (launch.apply_distortion != 0 || launch.apply_tca != 0 ||
                               launch.apply_projection != 0 || launch.apply_crop_circle != 0);
  const bool has_rect_crop  = (launch.apply_crop != 0 &&
                              static_cast<LensCalibCropMode>(launch.crop_mode) ==
                                  LensCalibCropMode::RECTANGLE);
  if (!has_vignetting && !has_warp && !has_rect_crop) {
    return;
  }

  if (!has_vignetting && !has_warp && has_rect_crop) {
    const cv::Rect roi = ComputeRectCropRoi(launch);
    if (roi.width > 0 && roi.height > 0 &&
        (roi.width < static_cast<int>(image.Width()) || roi.height < static_cast<int>(image.Height()))) {
      MetalImage cropped;
      utils::CropResizeTexture(image, cropped, roi, roi.size());
      image = std::move(cropped);
    }
    return;
  }

  const auto row_bytes   = RowBytesFor(image.Width(), image.Format());
  const auto buffer_size = row_bytes * image.Height();
  const auto stride      = static_cast<uint32_t>(row_bytes / (sizeof(float) * 4U));

  auto src_buffer = MakeSharedBuffer(buffer_size);
  auto command_buffer = MakeCommandBuffer();
  EncodeTextureToBuffer(command_buffer.get(), image, src_buffer.get(), row_bytes, buffer_size);

  LensCalibDispatchParams dispatch_params{
      .params     = launch,
      .src_stride = stride,
      .dst_stride = stride,
  };

  if (has_vignetting) {
    DispatchVignetting(command_buffer.get(), src_buffer.get(), dispatch_params, image.Width(),
                       image.Height());
  }

  NS::SharedPtr<MTL::Buffer> final_buffer = src_buffer;
  if (has_warp) {
    auto dst_buffer = MakeSharedBuffer(buffer_size);
    DispatchWarp(command_buffer.get(), src_buffer.get(), dst_buffer.get(), dispatch_params,
                 image.Width(), image.Height());
    final_buffer = std::move(dst_buffer);
  }

  MetalImage working;
  working.Create(image.Width(), image.Height(), image.Format(), true, true,
                 HasRenderTargetUsage(image));
  EncodeBufferToTexture(command_buffer.get(), working, final_buffer.get(), row_bytes, buffer_size);
  command_buffer->commit();
  command_buffer->waitUntilCompleted();

  if (has_rect_crop) {
    const cv::Rect roi = ComputeRectCropRoi(launch);
    if (roi.width > 0 && roi.height > 0 &&
        (roi.width < static_cast<int>(working.Width()) ||
         roi.height < static_cast<int>(working.Height()))) {
      MetalImage cropped;
      utils::CropResizeTexture(working, cropped, roi, roi.size());
      working = std::move(cropped);
    }
  }

  image = std::move(working);
}

}  // namespace alcedo::metal

#endif
