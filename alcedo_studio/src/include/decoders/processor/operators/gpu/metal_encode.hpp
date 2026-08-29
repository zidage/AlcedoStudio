//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstdint>

#include "decoders/dng_default_crop.hpp"
#include "decoders/processor/raw_linearization_params.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "edit/geometry/types.hpp"

namespace alcedo::metal {

/**
 * @brief Encode RAW Metal kernels onto an existing command buffer.
 *
 * Callers own textures, the command buffer, and any scratch. These entrypoints
 * do not create a queue, command buffer, static scratch, or wait.
 */
void EncodeToLinearRef(void* command_buffer, void* src_r16u, void* dst_r32f,
                       const RawLinearizationParams& linearization, const RawCfaPattern& pattern);

void EncodeCfaClamp01(void* command_buffer, void* r32f, std::uint32_t width, std::uint32_t height);

void EncodeBayerRcd(void* command_buffer, void* linear_cfa, void* r, void* g, void* b, void* vh,
                    void* pq, const BayerPattern2x2& pattern, std::uint32_t width,
                    std::uint32_t height);

void EncodeXTrans(void* command_buffer, void* linear_cfa, void* green, void* rgba,
                  const XTransPattern6x6& pattern, std::uint32_t width, std::uint32_t height,
                  int passes);

void EncodeHighlightReconstruct(void* command_buffer, void* src_rgba, void* dst_rgba,
                                void* stats_buffer, std::uint32_t stats_offset,
                                const float* cam_mul, std::uint32_t width, std::uint32_t height);

void EncodePackPlanesCropInverseOrient(void* command_buffer, void* r, void* g, void* b,
                                       void* dst_rgba, RectI crop, const float* cam_mul, int flip);

void EncodeCopyRgbaCropInverseOrient(void* command_buffer, void* src_rgba, void* dst_rgba,
                                     RectI crop, const float* cam_mul, int flip);

void EncodeWarpRectilinear(void* command_buffer, void* src_rgba, void* dst_rgba,
                           const dng::WarpRectilinear& warp, std::uint32_t width,
                           std::uint32_t height);

}  // namespace alcedo::metal

#endif
