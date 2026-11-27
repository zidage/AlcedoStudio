//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstddef>
#include <cstdint>
#include <optional>

#include "edit/geometry/types.hpp"
#include "edit/runtime/lens/lens_calib_runtime.hpp"
#include "edit/runtime/vulkan/vulkan_backend.hpp"
#include "edit/runtime/vulkan/vulkan_buffer_view.hpp"

/**
 * @file
 * @brief The lens calibration of the Develop pass on Vulkan (plan phase VK5): the Vulkan form of
 * `OpenCL::Geometry::ApplyLensCalibration` together with the resize back to the sensor size of
 * `ExecuteOpenClLensCalibration`.
 */
namespace alcedo::vulkan::lens {

/// Bytes of the parameter view of @ref EncodeLensCalibration.
inline constexpr std::size_t kLensCalibParameterBytes = sizeof(LensCalibGpuParams);

/// The stages of one lens calibration of an image (the rule of the OpenCL `ApplyLensCalibration`).
struct LensCalibrationSteps {
  bool                 vignetting = false;  ///< the vignetting gain, in place
  /// The warp (distortion, TCA, projection, or circle crop) into the warp texture.
  bool                 warp       = false;
  /// The rectangle crop when it is smaller than the image (the OpenCL `ComputeRectCropRoi`).
  std::optional<RectI> rectangle_crop;

  [[nodiscard]] auto   Any() const -> bool {
    return vignetting || warp || rectangle_crop.has_value();
  }
};

/**
 * @brief The stages of @p params for an image of @p width x @p height. A source or destination
 * size of zero in @p params means the image size (the OpenCL defaults).
 */
[[nodiscard]] auto ResolveLensCalibrationSteps(const LensCalibGpuParams& params,
                                               std::uint32_t width, std::uint32_t height)
    -> LensCalibrationSteps;

/// The textures that @ref EncodeLensCalibration writes besides the image. The caller allocates
/// them for the steps of @ref ResolveLensCalibrationSteps (a pass encoder does not allocate).
struct LensCalibrationScratch {
  /// RGBA32F of the image size, not the image; required when the steps have a warp.
  VulkanBackend::Texture2D* warped  = nullptr;
  /// RGBA32F of the rectangle crop size; required when the steps have a rectangle crop.
  VulkanBackend::Texture2D* cropped = nullptr;
};

/**
 * @brief Records the lens calibration of @p image in place.
 *
 * The stages of OpenCL: the vignetting gain in place; the warp into `scratch.warped`; the copy of
 * the rectangle crop into `scratch.cropped`; then the bilinear resize of the last result back to
 * the size of @p image (`EncodeCropResize` of the whole result, which reads 0 outside it, as the
 * OpenCL resize of the cropped image). Steps without an effect record nothing.
 *
 * Records the upload of the parameter record into @p parameters when a kernel runs, then the
 * dispatches and copies, into the open recording of @p command_context; does not submit or wait.
 *
 * @param parameters @ref kLensCalibParameterBytes bytes (16-byte aligned) that no other command
 *        of the recording reads.
 * @throws std::invalid_argument With the values when @p image is empty or not RGBA32F, the sizes
 *         of @p params are not the image size, a scratch texture that the steps need is missing,
 *         not RGBA32F, of another size, or the image itself, or the parameter view is too small or
 *         not aligned. Nothing is recorded.
 */
void EncodeLensCalibration(VulkanBackend& backend, VulkanCommandContext& command_context,
                           VulkanBackend::Texture2D& image, const LensCalibrationScratch& scratch,
                           VulkanBufferView parameters, const LensCalibGpuParams& params);

}  // namespace alcedo::vulkan::lens

#endif  // HAVE_VULKAN
