//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/cuda/cuda_drt_gpu_params.cuh"
#include "edit/runtime/cuda/drt/disp_enc_funcs.cuh"
#include "edit/runtime/cuda/drt/odt_funcs.cuh"
#include "edit/runtime/cuda/drt/open_drt_funcs.cuh"

namespace alcedo::CUDA {

/**
 * @brief The display transform of one pixel of linear AP1, with the encoding of the output.
 *
 * The entry has the ACES reference gamut compression, then ACES 2.0 or OpenDRT. `DrtKernel` calls
 * this function. It is a function of its own, so that another kernel can show linear AP1 with the
 * same arithmetic.
 */
GPU_FUNC float3 DrtDisplayPixel(float3 ap1, const CudaDrtGpuParams& runtime) {
  const AcesRgcRgb compressed = AcesReferenceGamutCompress(ap1.x, ap1.y, ap1.z);
  const float3     scene      = make_float3(compressed.r, compressed.g, compressed.b);
  float3           display_linear;
  if (runtime.method_ == CudaDrtMethod::ACES_2_0) {
    auto aces      = runtime.aces_params_;
    display_linear = OutputTransform_fwd(scene, aces);
  } else {
    display_linear = OpenDRTTransform_fwd(scene, runtime.open_drt_params_);
  }
  // DisplayEncoding reads the matrix only; its parameter is not const.
  return DisplayEncoding(display_linear, const_cast<float*>(runtime.limit_to_display_matx),
                         runtime.eotf, runtime.display_linear_scale_);
}

}  // namespace alcedo::CUDA
