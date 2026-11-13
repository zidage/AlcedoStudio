//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstdint>

#include "edit/input/prepared_raw_input.hpp"

namespace alcedo {

/**
 * @brief LinearizeRaster on CUDA: read tightly packed RGBA host-format pixels at @p source and
 * write linear RGB (3 floats per pixel, rows of `width * 12` bytes) to @p linear_rgb with the
 * raster_linearize_math.h parameters at @p params.
 * @throws std::runtime_error for a CFA format or a launch error.
 */
void LaunchCudaLinearizeRaster(const void* source, HostPixelFormat format, std::uint32_t width,
                               std::uint32_t height, const float* params, float* linear_rgb,
                               cudaStream_t stream);

}  // namespace alcedo
