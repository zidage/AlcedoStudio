//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include "edit/runtime/adjustment_runtime.hpp"

namespace alcedo::cuda_neighbor_grade {

__global__ void BlurHorizontal(const float4* src, float4* dst, int width, int height,
                               GradeNeighborParams params);
__global__ void ApplyVertical(const float4* original, const float4* blur_horizontal, float4* dst,
                              int width, int height, GradeNeighborParams params);

}  // namespace alcedo::cuda_neighbor_grade
