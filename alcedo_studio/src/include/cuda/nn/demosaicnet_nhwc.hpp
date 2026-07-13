// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

namespace alcedo::cuda::nn {

// One-time host prepack: OIHW [12,C,1,1] -> channel-major [C,12].
void TransformDemosaicNetResidualWeightsNhwc(const float* src_oihw, int channels, float* dst_cio);

// Fixed student residual projection: NHWC [N,H,W,C] -> NHWC [N,H,W,12].
// Weight layout is the prepacked [C,12] form above.
void DemosaicNetResidual1x1Nhwc(const float* input_nhwc, float* residual_nhwc,
                                const float* weight_cio, const float* bias, int batch, int height,
                                int width, int channels, cudaStream_t stream = nullptr);

// Fixed factor-2 grouped unpack, centered mosaic crop, and channel concat:
//   residual NHWC [N,H,W,12] -> unpacked NHWC [N,2H,2W,3]
//   mosaic NCHW [N,3,input_h,input_w] -> centered crop [N,2H,2W,3]
//   output NHWC [N,2H,2W,6] = concat(cropped_mosaic, unpacked_residual)
void DemosaicNetUnpackCropConcatNhwc(const float* mosaic_nchw, int input_h, int input_w,
                                     const float* residual_nhwc, int residual_h, int residual_w,
                                     float* cat_nhwc, int batch, cudaStream_t stream = nullptr);

}  // namespace alcedo::cuda::nn
