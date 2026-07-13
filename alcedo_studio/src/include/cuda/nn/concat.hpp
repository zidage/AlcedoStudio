//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <cstdint>

#include "cuda/nn/tensor.hpp"

namespace alcedo::cuda::nn {

// Concatenate two rank-4 NCHW tensors along the channel axis.
//
//   a:   [N, Ca, H, W]
//   b:   [N, Cb, H, W]   (N, H, W must match a)
//   out: [N, Ca+Cb, H, W]  must be contiguous; storage pre-allocated by caller
//
// Used by demosaicnet full-res branch: Bayer (3+3→6), XTrans (3+64→67).
// Output is always written as contiguous NCHW for the next Conv2d.

void ConcatChannels(const DeviceTensor& a, const DeviceTensor& b, DeviceTensor& out,
                    cudaStream_t stream = nullptr);

// Expected output channel count: Ca + Cb. Helper for workspace sizing.
[[nodiscard]] inline auto ConcatChannelsOutChannels(const DeviceTensor& a,
                                                    const DeviceTensor& b) -> std::int64_t {
  if (a.rank != 4 || b.rank != 4) {
    return -1;
  }
  return a.shape[1] + b.shape[1];
}

}  // namespace alcedo::cuda::nn
