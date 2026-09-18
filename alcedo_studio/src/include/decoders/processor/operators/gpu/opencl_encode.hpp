//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 120
#endif
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#include <CL/cl.h>

#include <cstdint>

#include "decoders/processor/raw_linearization_params.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "decoders/processor/raw_rgb_linearization_params.hpp"
#include "edit/geometry/types.hpp"

namespace alcedo::opencl {

/**
 * @brief Product-queue encode target for RAW OpenCL kernels.
 *
 * Does not own a queue. @p retain_event receives each enqueue event (refcount 1)
 * so the caller can track it on the current CommandContext. Encode functions do
 * not wait, finish, or allocate scratch.
 */
struct OpenClEncodeQueue {
  cl_command_queue queue        = nullptr;
  void (*retain_event)(cl_event event, void* context) = nullptr;
  void*            retain_ctx   = nullptr;
};

/**
 * @brief Byte-offset view into a workspace or transient OpenCL buffer.
 *
 * Kernels index `native` at @p offset_bytes. Offset 0 is a whole buffer.
 */
struct OpenClBufferView {
  cl_mem        native       = nullptr;
  std::uint32_t offset_bytes = 0;
};

}  // namespace alcedo::opencl

namespace alcedo::OpenCL {

/// Linearize uploaded RGBA in place before the normal RAW HLR/pack stages.
void EncodeLinearizeRgb(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView rgba,
                        std::uint32_t width, std::uint32_t height,
                        const RawRgbLinearizationParams& params);

/**
 * @brief Encode RAW OpenCL kernels onto the current product queue.
 *
 * Callers own buffers, images, the queue, and scratch. These entrypoints do not
 * create a queue, allocate long-lived scratch, or call clFinish.
 */
void EncodeToLinearRef(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView src_u16,
                       opencl::OpenClBufferView dst_f32, std::uint32_t width, std::uint32_t height,
                       const RawLinearizationParams& linearization, const RawCfaPattern& pattern);

void EncodeCfaClamp01(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView r32f,
                      std::uint32_t width, std::uint32_t height);

void EncodeBayerRcd(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView linear,
                    opencl::OpenClBufferView r, opencl::OpenClBufferView g,
                    opencl::OpenClBufferView b, opencl::OpenClBufferView vh,
                    opencl::OpenClBufferView pq, const BayerPattern2x2& pattern,
                    std::uint32_t width, std::uint32_t height);

void EncodeXTrans(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView linear,
                  opencl::OpenClBufferView green, opencl::OpenClBufferView rgba,
                  const XTransPattern6x6& pattern, std::uint32_t width, std::uint32_t height,
                  int passes);

void EncodeHighlightReconstruct(opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView src_rgba,
                                opencl::OpenClBufferView dst_rgba, opencl::OpenClBufferView mask,
                                opencl::OpenClBufferView dilated_mask, opencl::OpenClBufferView sums,
                                opencl::OpenClBufferView cnts, opencl::OpenClBufferView anyclipped,
                                const float* cam_mul, std::uint32_t width, std::uint32_t height);

void EncodeHighlightReconstructPlanarAndPack(
    opencl::OpenClEncodeQueue& stream, opencl::OpenClBufferView r, opencl::OpenClBufferView g,
    opencl::OpenClBufferView b, cl_mem dst_rgba, opencl::OpenClBufferView mask,
    opencl::OpenClBufferView dilated_mask, opencl::OpenClBufferView sums,
    opencl::OpenClBufferView cnts, opencl::OpenClBufferView anyclipped, const float* cam_mul,
    RectI crop, std::uint32_t plane_width, int flip);

void EncodePackPlanesCropInverseOrient(opencl::OpenClEncodeQueue& stream,
                                       opencl::OpenClBufferView r, opencl::OpenClBufferView g,
                                       opencl::OpenClBufferView b, cl_mem dst_rgba, RectI crop,
                                       std::uint32_t plane_width, const float* cam_mul, int flip);

void EncodeCopyRgbaCropInverseOrient(opencl::OpenClEncodeQueue& stream,
                                     opencl::OpenClBufferView src_rgba, cl_mem dst_rgba, RectI crop,
                                     std::uint32_t src_width, const float* cam_mul, int flip);

void EncodeCopyRgbCropInverseOrient(opencl::OpenClEncodeQueue& stream,
                                    opencl::OpenClBufferView src_rgb, cl_mem dst_rgba, RectI crop,
                                    std::uint32_t src_width, const float* cam_mul, int flip);

}  // namespace alcedo::OpenCL

#endif
