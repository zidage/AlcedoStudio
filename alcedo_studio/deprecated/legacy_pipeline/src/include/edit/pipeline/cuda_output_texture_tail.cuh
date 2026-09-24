//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <string_view>

#include "edit/operators/GPU_kernels/detail.cuh"
#include "edit/operators/GPU_kernels/film_grain.cuh"
#include "edit/operators/GPU_kernels/halation.cuh"
#include "edit/pipeline/kernel_stream_gpu.cuh"

namespace alcedo {
namespace CUDA {

inline constexpr std::array<std::string_view, 8> kCudaOutputTextureTailOrder = {
    "sharpen_blur_horizontal",    "sharpen_apply_vertical",    "clarity_blur_horizontal",
    "clarity_apply_vertical",     "halation_blur_horizontal",  "halation_apply_vertical",
    "film_grain_blur_horizontal", "film_grain_apply_vertical",
};

template <typename PreOutputChain, typename HighlightShadowStage, typename OutputChain>
auto MakeCudaPipelineKernelStream(PreOutputChain pre_output, HighlightShadowStage hs,
                                  OutputChain output) {
  auto sharp_h = GPU_SharpenBlurHorizontalKernel();
  auto sharp_v = GPU_SharpenApplyVerticalKernel();
  auto clar_h  = GPU_ClarityBlurHorizontalKernel();
  auto clar_v  = GPU_ClarityApplyVerticalKernel();
  auto hal_h   = GPU_HalationBlurHorizontalKernel();
  auto hal_v   = GPU_HalationApplyVerticalKernel();
  auto grain_h = GPU_FilmGrainBlurHorizontalKernel();
  auto grain_v = GPU_FilmGrainApplyVerticalKernel();

  return GPU_StaticKernelStream(pre_output, hs, output, sharp_h, sharp_v, clar_h, clar_v, hal_h,
                                hal_v, grain_h, grain_v);
}

}  // namespace CUDA
}  // namespace alcedo
