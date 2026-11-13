//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda_neighbor_grade.hpp"
#include "cuda_neighbor_grade.cuh"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace alcedo::cuda_neighbor_grade {
namespace {

/// Film Grain has its own passes (RecordCudaFilmGrainPost), so these kernels do not take it.
void RequireNeighborhoodKernelBehavior(const GradeNeighborParams& params, const char* caller) {
  if (static_cast<AdjustmentBehavior>(params.behavior) == AdjustmentBehavior::FilmGrain) {
    throw std::invalid_argument(std::string(caller) +
                                ": Film Grain runs RecordCudaFilmGrainPost, not this kernel");
  }
}

}  // namespace

void LaunchBlurHorizontal(cudaStream_t stream, const float4* src, float4* dst, int width,
                          int height, const GradeNeighborParams& params) {
  RequireNeighborhoodKernelBehavior(params, "LaunchBlurHorizontal");
  const dim3 block{16, 16};
  const dim3 grid{(static_cast<unsigned>(width) + 15U) / 16U,
                  (static_cast<unsigned>(height) + 15U) / 16U};
  BlurHorizontal<<<grid, block, 0, stream>>>(src, dst, width, height, params);
}

void LaunchApplyVertical(cudaStream_t stream, const float4* original,
                         const float4* blur_horizontal, float4* dst, const float4* mix_original,
                         float mix, const std::uint8_t* mask, int width, int height,
                         const GradeNeighborParams& params) {
  RequireNeighborhoodKernelBehavior(params, "LaunchApplyVertical");
  const dim3 block{16, 16};
  const dim3 grid{(static_cast<unsigned>(width) + 15U) / 16U,
                  (static_cast<unsigned>(height) + 15U) / 16U};
  const auto radius = NeighborhoodVerticalRadius(params);
  const auto shared =
      static_cast<std::size_t>(block.x) * (block.y + 2U * radius) * sizeof(float4);
  ApplyVertical<<<grid, block, shared, stream>>>(original, blur_horizontal, dst, mix_original, mix,
                                                 mask, width, height, params);
}

}  // namespace alcedo::cuda_neighbor_grade
