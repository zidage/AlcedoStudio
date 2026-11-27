//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/lens/vulkan/vulkan_lens_calib.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_geometry.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "vulkan/vulkan_lens_calib_kernels.hpp"

namespace alcedo::vulkan::lens {
namespace {

constexpr const char* kOperation       = "vulkan::lens::EncodeLensCalibration";
/// The slot alignment of `ParameterArena`.
constexpr std::size_t kRecordAlignment = 16;

[[noreturn]] void     ThrowRequestError(const std::string& message) {
  throw std::invalid_argument(std::string(kOperation) + ": " + message);
}

auto SizeText(std::uint32_t width, std::uint32_t height) -> std::string {
  return std::to_string(width) + " x " + std::to_string(height);
}

/// @p params with the OpenCL size defaults: a size of zero is the image size.
auto WithImageSize(LensCalibGpuParams params, std::uint32_t width, std::uint32_t height)
    -> LensCalibGpuParams {
  if (params.src_width <= 0 || params.src_height <= 0) {
    params.src_width  = static_cast<std::int32_t>(width);
    params.src_height = static_cast<std::int32_t>(height);
  }
  if (params.dst_width <= 0 || params.dst_height <= 0) {
    params.dst_width  = static_cast<std::int32_t>(width);
    params.dst_height = static_cast<std::int32_t>(height);
  }
  return params;
}

/// The crop rectangle in destination pixels (the OpenCL `ResolveCropRectPx`): the bounds are
/// left, right, top, bottom of the long edge first.
auto ResolveCropRectPx(const LensCalibGpuParams& params) -> std::array<float, 4> {
  const float width  = static_cast<float>(params.dst_width);
  const float height = static_cast<float>(params.dst_height);
  if (width <= 0.0F || height <= 0.0F) {
    return {};
  }
  float left   = 0.0F;
  float right  = 0.0F;
  float top    = 0.0F;
  float bottom = 0.0F;
  if (params.dst_width >= params.dst_height) {
    left   = params.crop_bounds[0] * width;
    right  = params.crop_bounds[1] * width;
    top    = params.crop_bounds[2] * height;
    bottom = params.crop_bounds[3] * height;
  } else {
    left   = params.crop_bounds[2] * width;
    right  = params.crop_bounds[3] * width;
    top    = params.crop_bounds[0] * height;
    bottom = params.crop_bounds[1] * height;
  }
  if (left > right) {
    std::swap(left, right);
  }
  if (top > bottom) {
    std::swap(top, bottom);
  }
  return {left, right, top, bottom};
}

/// The rectangle crop in pixels (the OpenCL `ComputeRectCropRoi`).
auto ComputeRectCropRoi(const LensCalibGpuParams& params) -> RectI {
  const int width  = params.dst_width;
  const int height = params.dst_height;
  if (width <= 0 || height <= 0) {
    return {};
  }
  const auto [left, right, top, bottom] = ResolveCropRectPx(params);
  int x0                                = static_cast<int>(std::lround(left));
  int x1                                = static_cast<int>(std::lround(right));
  int y0                                = static_cast<int>(std::lround(top));
  int y1                                = static_cast<int>(std::lround(bottom));
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
  return {x0, y0, std::max(1, x1 - x0), std::max(1, y1 - y0)};
}

void RequireTexture(const VulkanBackend::Texture2D* texture, const char* role, std::uint32_t width,
                    std::uint32_t height) {
  if (texture == nullptr || texture->Empty()) {
    ThrowRequestError(std::string("the steps need the ") + role + " texture; it is missing");
  }
  if (texture->Format() != TextureFormat::Rgba32f || texture->Width() != width ||
      texture->Height() != height) {
    ThrowRequestError(std::string("the ") + role + " texture must be RGBA32F of " +
                      SizeText(width, height) + "; it is " + TextureFormatName(texture->Format()) +
                      " of " + SizeText(texture->Width(), texture->Height()));
  }
}

auto Groups(std::uint32_t width, std::uint32_t height) -> VulkanWorkgroupCount {
  return WorkgroupsCovering(width, height, lens_calib_kernels::kGroupSize,
                            lens_calib_kernels::kGroupSize);
}

}  // namespace

auto ResolveLensCalibrationSteps(const LensCalibGpuParams& params, std::uint32_t width,
                                 std::uint32_t height) -> LensCalibrationSteps {
  const LensCalibGpuParams launch = WithImageSize(params, width, height);
  LensCalibrationSteps     steps;
  steps.vignetting = launch.apply_vignetting != 0;
  steps.warp       = launch.apply_distortion != 0 || launch.apply_tca != 0 ||
               launch.apply_projection != 0 || launch.apply_crop_circle != 0;
  const bool has_rect_crop =
      launch.apply_crop != 0 &&
      static_cast<LensCalibCropMode>(launch.crop_mode) == LensCalibCropMode::RECTANGLE;
  if (has_rect_crop) {
    const RectI roi = ComputeRectCropRoi(launch);
    if (roi.width > 0 && roi.height > 0 &&
        (static_cast<std::uint32_t>(roi.width) < width ||
         static_cast<std::uint32_t>(roi.height) < height)) {
      steps.rectangle_crop = roi;
    }
  }
  return steps;
}

void EncodeLensCalibration(VulkanBackend& backend, VulkanCommandContext& command_context,
                           VulkanBackend::Texture2D& image, const LensCalibrationScratch& scratch,
                           VulkanBufferView parameters, const LensCalibGpuParams& params) {
  if (image.Empty() || image.Format() != TextureFormat::Rgba32f) {
    ThrowRequestError("the image must be a non-empty RGBA32F texture");
  }
  const std::uint32_t      width  = image.Width();
  const std::uint32_t      height = image.Height();
  const LensCalibGpuParams launch = WithImageSize(params, width, height);
  if (launch.src_width != static_cast<std::int32_t>(width) ||
      launch.src_height != static_cast<std::int32_t>(height) ||
      launch.dst_width != static_cast<std::int32_t>(width) ||
      launch.dst_height != static_cast<std::int32_t>(height)) {
    ThrowRequestError("the parameters are for a source of " +
                      SizeText(static_cast<std::uint32_t>(launch.src_width),
                               static_cast<std::uint32_t>(launch.src_height)) +
                      " and a destination of " +
                      SizeText(static_cast<std::uint32_t>(launch.dst_width),
                               static_cast<std::uint32_t>(launch.dst_height)) +
                      "; the image is " + SizeText(width, height));
  }
  const LensCalibrationSteps steps = ResolveLensCalibrationSteps(launch, width, height);
  if (steps.warp) {
    RequireTexture(scratch.warped, "warp", width, height);
    if (scratch.warped->ResourceId() == image.ResourceId()) {
      ThrowRequestError("the warp texture is the image");
    }
  }
  if (steps.rectangle_crop.has_value()) {
    RequireTexture(scratch.cropped, "crop", static_cast<std::uint32_t>(steps.rectangle_crop->width),
                   static_cast<std::uint32_t>(steps.rectangle_crop->height));
  }
  lens_calib_kernels::LensCalibPushConstants push;
  if (steps.vignetting || steps.warp) {
    push.params = RequireBufferRange(parameters, kLensCalibParameterBytes, kRecordAlignment,
                                     kOperation, "parameters");
  }
  if (!steps.Any()) {
    return;
  }

  if (steps.vignetting || steps.warp) {
    backend.UploadBufferRange(*parameters.buffer, parameters.offset_bytes,
                              PushConstantBytes(launch), command_context);
  }
  if (steps.vignetting) {
    const VulkanImageBinding images[] = {{lens_calib_kernels::kImageBinding, &image}};
    RecordKernelDispatch(backend, command_context, lens_calib_kernels::VignettingDescriptor(),
                         images, PushConstantBytes(push), Groups(width, height));
  }
  const VulkanBackend::Texture2D* result = &image;
  if (steps.warp) {
    const VulkanImageBinding images[] = {{lens_calib_kernels::kImageBinding, &image},
                                         {lens_calib_kernels::kDestinationBinding, scratch.warped}};
    RecordKernelDispatch(backend, command_context, lens_calib_kernels::WarpDescriptor(), images,
                         PushConstantBytes(push), Groups(width, height));
    result = scratch.warped;
  }
  if (steps.rectangle_crop.has_value()) {
    backend.CopyTexture2DRegion(*result, *steps.rectangle_crop, *scratch.cropped, command_context);
    result = scratch.cropped;
  }
  if (result != &image) {
    // The OpenCL Develop pass resizes the corrected image back to the sensor size; a result of
    // the image size is a copy.
    geometry::EncodeCropResize(backend, command_context, *result, image,
                               RectI{0, 0, static_cast<std::int32_t>(result->Width()),
                                     static_cast<std::int32_t>(result->Height())});
  }
}

}  // namespace alcedo::vulkan::lens

#endif  // HAVE_VULKAN
