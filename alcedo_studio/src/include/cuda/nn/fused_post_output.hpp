//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstddef>
#include <cstdint>

namespace alcedo::cuda::nn {

// Fuse student post 3×3 (6 → width) + bias + ReLU + output 1×1 (width → 3)
// + optional bias + optional signed gamma decode, without materializing the
// width-channel post activation.
//
// Supported exact student tails only: width ∈ {24, 32}, Cin fixed at 6.
//
// Weight layouts (device, immutable after model load):
//   post_weight:  OIHW [width, 6, 3, 3]
//   post_bias:    [width] (required)
//   output_weight_cio: prepacked [width, 3] so each post channel's RGB weights
//                      are contiguous (host transforms once from OIHW [3,width,1,1])
//   output_bias:  [3] (required)
//
// Spatial: valid 3×3 on cat [N,H,W,6] yields natural (H-2)×(W-2). Callers may
// request a center crop by setting out_h/out_w smaller than natural and the
// kernel only evaluates those export pixels.

struct FusedPostOutputParams {
  int          post_channels = 0;  // 24 (Bayer) or 32 (X-Trans)
  const float* post_weight   = nullptr;
  const float* post_bias     = nullptr;
  const float* output_weight_cio = nullptr;
  const float* output_bias   = nullptr;
  bool         apply_gamma_decode = false;  // pow_signed(x, 2.2) after output bias
};

// Host-side one-time prepack: OIHW [3, width, 1, 1] → CIO [width, 3].
// `dst` must hold width * 3 floats.
void PrepackOutputWeightsCio(const float* src_oihw, int width, float* dst);

// Persistent channels-last tail. `cat_nhwc` is contiguous [N,H,W,6]; output is
// pitched HWC RGB. Post/output weights use the same OIHW/CIO layouts as above.
void FusedPostOutputNhwcToHwc(const float* cat_nhwc, int batch, int cat_h, int cat_w,
                              float* rgb_hwc, std::size_t step_bytes, int out_h, int out_w,
                              const FusedPostOutputParams& params,
                              cudaStream_t stream = nullptr);

}  // namespace alcedo::cuda::nn
