//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include "cuda/nn/tensor.hpp"

namespace alcedo::cuda::nn {

// Center-crop spatial dimensions of a rank-4 NCHW tensor to (target_h, target_w).
//
// Matches demosaicnet `_crop_like` / PyTorch center crop integer division:
//
//   crop_h = src_h - tgt_h;  crop_t = crop_h // 2;  crop_b = crop_h - crop_t
//   crop_w = src_w - tgt_w;  crop_l = crop_w // 2;  crop_r = crop_w - crop_l
//   return src[..., crop_t : src_h - crop_b, crop_l : src_w - crop_r]
//
// Requires target_h <= src_h and target_w <= src_w. N and C are preserved.

// Zero-copy strided view (not contiguous unless margins are zero on W).
// Aliases `input` storage. Useful when a later op can honor strides.
[[nodiscard]] auto CenterCropSpatialView(const DeviceTensor& input, int target_h,
                                         int target_w) -> DeviceTensor;

// Materializing crop into contiguous `out` of shape [N, C, target_h, target_w].
void CenterCropSpatial(const DeviceTensor& input, DeviceTensor& out, int target_h,
                       int target_w, cudaStream_t stream = nullptr);

// Crop `input` spatially to match the H,W of `spatial_ref` (both rank-4 NCHW).
void CenterCropLike(const DeviceTensor& input, const DeviceTensor& spatial_ref,
                    DeviceTensor& out, cudaStream_t stream = nullptr);

// Compute demosaicnet crop offsets (top, left) for the given sizes.
// Throws if target exceeds source.
struct CenterCropOffsets {
  int top  = 0;
  int left = 0;
  int src_h = 0;
  int src_w = 0;
  int tgt_h = 0;
  int tgt_w = 0;
};

[[nodiscard]] auto ComputeCenterCropOffsets(int src_h, int src_w, int tgt_h, int tgt_w)
    -> CenterCropOffsets;

}  // namespace alcedo::cuda::nn
