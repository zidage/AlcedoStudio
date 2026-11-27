//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_VULKAN

#include <cstddef>
#include <cstdint>
#include <span>

#include "decoders/processor/raw_linearization_params.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "decoders/processor/raw_rgb_linearization_params.hpp"
#include "edit/geometry/types.hpp"
#include "edit/runtime/vulkan/vulkan_backend.hpp"
#include "edit/runtime/vulkan/vulkan_buffer_view.hpp"
#include "vulkan/vulkan_highlight_kernels.hpp"
#include "vulkan/vulkan_raw_core_kernels.hpp"
#include "vulkan/vulkan_xtrans_kernels.hpp"

/**
 * @file
 * @brief The Vulkan RAW encode functions of the Develop pass (plan phases VK4 and VK5).
 *
 * Each function is the Vulkan form of the `OpenCL::Encode*` function of the same name in
 * `opencl_encode.hpp`, with the same parameters and the same result. The RAW intermediate data
 * are buffers (device addresses); the pack functions write the RGBA32F image of the graph.
 *
 * Common rules of every function:
 * - It records into the open recording of @p command_context (it opens one when none is open). It
 *   does not submit and does not wait (plan section 4.2). The caller submits, and keeps each
 *   buffer and texture alive until the batch is complete (the `VulkanBackend` lifetime rule).
 * - Each view is checked for its size (width * height elements from its offset) and the alignment
 *   of its element before anything is recorded. A failed check throws `std::invalid_argument`
 *   with the function, the view, and the values; then nothing is recorded and no pipeline is
 *   requested.
 * - A buffer that a function writes is not read by another view of the same call, except where
 *   the OpenCL function reads and writes the same buffer (in place).
 * - A width or a height of zero records nothing (the views are checked first). The pack functions
 *   reject an empty crop.
 * - Not thread-safe: the command context belongs to one render thread.
 */
namespace alcedo::vulkan::raw {

/// Bytes of the parameter view of @ref EncodeToLinearRef.
inline constexpr std::size_t kToLinearRefParameterBytes =
    sizeof(raw_core_kernels::ToLinearRefParameters);
/// Bytes of the parameter view of @ref EncodeXTrans.
inline constexpr std::size_t kXTransParameterBytes = sizeof(xtrans_kernels::XTransParams);

/**
 * @brief Linearizes uploaded RGBA in place: black, clamp at 0 for integer codes, scale; alpha 1.
 * @param rgba width * height RGBA float values (16-byte aligned).
 */
void EncodeLinearizeRgb(VulkanBackend& backend, VulkanCommandContext& command_context,
                        VulkanBufferView rgba, std::uint32_t width, std::uint32_t height,
                        const RawRgbLinearizationParams& params);

/**
 * @brief U16 CFA samples to linear F32 samples: black (with the pattern black), white, and the
 * relative as-shot white balance of the color of each sample.
 *
 * @param source_u16 width * height U16 samples (2-byte aligned).
 * @param destination_f32 width * height float values (4-byte aligned).
 * @param parameters @ref kToLinearRefParameterBytes bytes (16-byte aligned) that no other command
 *        of the recording reads; the function records the upload of its parameter record into
 *        them before the dispatch.
 */
void EncodeToLinearRef(VulkanBackend& backend, VulkanCommandContext& command_context,
                       VulkanBufferView source_u16, VulkanBufferView destination_f32,
                       VulkanBufferView parameters, std::uint32_t width, std::uint32_t height,
                       const RawLinearizationParams& linearization, const RawCfaPattern& pattern);

/// @brief Clamps width * height F32 CFA values to [0, 1] in place (4-byte aligned).
void EncodeCfaClamp01(VulkanBackend& backend, VulkanCommandContext& command_context,
                      VulkanBufferView r32f, std::uint32_t width, std::uint32_t height);

/// The scratch planes of @ref EncodeBayerRcd: width * height float values each.
struct RcdPlanes {
  VulkanBufferView r;   ///< output: red
  VulkanBufferView g;   ///< output: green
  VulkanBufferView b;   ///< output: blue
  VulkanBufferView vh;  ///< scratch: vertical and horizontal weight
  VulkanBufferView pq;  ///< scratch: diagonal weight
};

/**
 * @brief RCD demosaic of a linear Bayer CFA into R, G, B planes: five dispatches in the order of
 * `OpenCL::EncodeBayerRcd`. In the 4-texel border each plane has the CFA value at the samples
 * of its color and 0 at the other samples.
 * @param linear width * height float values; read only.
 */
void EncodeBayerRcd(VulkanBackend& backend, VulkanCommandContext& command_context,
                    VulkanBufferView linear, const RcdPlanes& planes,
                    const BayerPattern2x2& pattern, std::uint32_t width, std::uint32_t height);

/**
 * @brief X-Trans demosaic of a linear CFA into a green plane and RGBA: two dispatches.
 *
 * @param green width * height float values: the green plane.
 * @param rgba width * height RGBA float values (16-byte aligned): the result.
 * @param parameters @ref kXTransParameterBytes bytes (16-byte aligned) that no other command of
 *        the recording reads; the function records the upload of its parameter record into them.
 * @param passes The OpenCL value: 3 at full size, 1 for a downsampled CFA; values below 1 are 1.
 *        More than one pass widens the red and blue radius from 3 to 4.
 */
void EncodeXTrans(VulkanBackend& backend, VulkanCommandContext& command_context,
                  VulkanBufferView linear, VulkanBufferView green, VulkanBufferView rgba,
                  VulkanBufferView parameters, const XTransPattern6x6& pattern, std::uint32_t width,
                  std::uint32_t height, int passes);

/**
 * @brief Packs the @p crop of R, G, B planes into @p destination with the inverse white balance
 * (green / channel gain of @p cam_mul) and the orientation @p flip.
 *
 * @param plane_width, plane_height The size of each plane; @p crop is inside it.
 * @param destination RGBA32F, the size of the crop, with the width and the height exchanged for
 *        flip 5 and 6.
 * @param flip 3: 180 degrees; 5 and 6: 90 degrees; every other value keeps the orientation (the
 *        rule of `cvt_ref_space.cl`).
 */
void EncodePackPlanesCropInverseOrient(VulkanBackend&        backend,
                                       VulkanCommandContext& command_context, VulkanBufferView r,
                                       VulkanBufferView g, VulkanBufferView b,
                                       VulkanBackend::Texture2D& destination, RectI crop,
                                       std::uint32_t plane_width, std::uint32_t plane_height,
                                       std::span<const float, 4> cam_mul, int flip);

/**
 * @brief Copies the @p crop of an RGBA buffer into @p destination with the inverse white balance
 * of @p cam_mul and the orientation @p flip. Alpha is copied.
 *
 * @param source source_width * source_height RGBA float values (16-byte aligned).
 * @param destination See @ref EncodePackPlanesCropInverseOrient.
 */
void EncodeCopyRgbaCropInverseOrient(VulkanBackend& backend, VulkanCommandContext& command_context,
                                     VulkanBufferView source, VulkanBackend::Texture2D& destination,
                                     RectI crop, std::uint32_t source_width,
                                     std::uint32_t source_height, std::span<const float, 4> cam_mul,
                                     int flip);

// ---- Highlight reconstruction (phase VK5) ---------------------------------------------------

/// Bytes of the statistics view of @ref HighlightScratch: the float sums and counts of R, G, B.
inline constexpr std::size_t kHighlightStatisticsBytes =
    highlight_kernels::kStatisticsWords * sizeof(std::uint32_t);

/// Bytes of each mask view of @ref HighlightScratch for a region of @p width x @p height: one
/// 32-bit word for each texel with one bit for each of R, G, B.
[[nodiscard]] constexpr auto HighlightMaskBytes(std::uint32_t width, std::uint32_t height)
    -> std::size_t {
  return static_cast<std::size_t>(width) * height * sizeof(std::uint32_t);
}

/**
 * @brief The scratch of one highlight reconstruction: the Vulkan form of the five OpenCL scratch
 * views (the OpenCL `anyclipped` count has no reader and is not kept).
 *
 * The caller allocates the views; the encode function writes all of them, so their content
 * before the call does not matter. No other command of the recording uses them until the
 * reconstruction is complete.
 */
struct HighlightScratch {
  VulkanBufferView mask;        ///< @ref HighlightMaskBytes of the region (4-byte aligned)
  VulkanBufferView dilated;     ///< @ref HighlightMaskBytes of the region (4-byte aligned)
  VulkanBufferView statistics;  ///< @ref kHighlightStatisticsBytes (4-byte aligned)
};

/**
 * @brief Reconstructs the clipped highlights of an RGBA buffer into another RGBA buffer (the
 * opposed algorithm: clip mask, 7 x 7 dilation, chrominance of the ring around the clipped
 * texels, then the reconstruction of each texel near its clip level).
 *
 * The clip level of each channel is 0.987 times the ratio of its @p cam_mul gain to green (the
 * OpenCL `MakeHighlightParams`). Records the clear of the statistics and four dispatches.
 *
 * @param source, destination width * height RGBA float values each (16-byte aligned), two
 *        different ranges. Alpha is copied.
 */
void EncodeHighlightReconstruct(VulkanBackend& backend, VulkanCommandContext& command_context,
                                VulkanBufferView source, VulkanBufferView destination,
                                const HighlightScratch& scratch, std::span<const float, 4> cam_mul,
                                std::uint32_t width, std::uint32_t height);

/**
 * @brief Reconstructs the clipped highlights of the @p crop of R, G, B planes and packs the result
 * into @p destination with the inverse white balance and the orientation (the rule of
 * @ref EncodePackPlanesCropInverseOrient). The statistics and the masks cover the crop only.
 *
 * @param scratch The masks of the crop size.
 * @param destination RGBA32F of the oriented crop size; alpha 1.
 */
void EncodeHighlightReconstructPlanarAndPack(
    VulkanBackend& backend, VulkanCommandContext& command_context, VulkanBufferView r,
    VulkanBufferView g, VulkanBufferView b, VulkanBackend::Texture2D& destination,
    const HighlightScratch& scratch, std::span<const float, 4> cam_mul, RectI crop,
    std::uint32_t plane_width, std::uint32_t plane_height, int flip);

}  // namespace alcedo::vulkan::raw

#endif  // HAVE_VULKAN
