//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <opencv2/core/cuda.hpp>

#include "cuda/nn/tensor.hpp"

namespace alcedo::cuda::nn {

// Layout conversion between pipeline HWC and internal CNN NCHW (f32 only).
//
// Pipeline boundary:
//   - Image edge: cv::cuda::GpuMat CV_32FC(C) as HWC (pitched rows OK).
//   - CNN internals: contiguous DeviceTensor NCHW [N, C, H, W].
// Convert once in (Pack) and once out (Unpack); never thrash layouts mid-graph.
//
// Pure tensor overloads treat HWC as rank-4 [N, H, W, C] or rank-3 [H, W, C]
// (N implied 1). NCHW is always rank-4 [N, C, H, W] (or rank-3 [C, H, W]).

// --- DeviceTensor (NCHW contiguous out / HWC contiguous out) ---

// hwc: [N,H,W,C] or [H,W,C]  →  nchw: [N,C,H,W] contiguous (pre-allocated).
void PackHwcToNchw(const DeviceTensor& hwc, DeviceTensor& nchw,
                   cudaStream_t stream = nullptr);

// nchw: [N,C,H,W] or [C,H,W]  →  hwc: [N,H,W,C] contiguous (pre-allocated).
void UnpackNchwToHwc(const DeviceTensor& nchw, DeviceTensor& hwc,
                     cudaStream_t stream = nullptr);

// --- GpuMat boundary (N=1) ---

// Pack CV_32F GpuMat (any channel count, pitched OK) → contiguous NCHW [1,C,H,W].
// `nchw` must be pre-allocated with shape [1, channels, rows, cols].
void PackHwcToNchw(const cv::cuda::GpuMat& hwc, DeviceTensor& nchw,
                   cv::cuda::Stream* stream = nullptr);
void PackHwcToNchw(const cv::cuda::GpuMat& hwc, DeviceTensor& nchw, cudaStream_t stream);

// Unpack contiguous NCHW [1,C,H,W] (or [C,H,W]) → CV_32FC(C) GpuMat.
// Allocates / resizes `hwc` when empty or mismatched.
void UnpackNchwToHwc(const DeviceTensor& nchw, cv::cuda::GpuMat& hwc,
                     cv::cuda::Stream* stream = nullptr);
void UnpackNchwToHwc(const DeviceTensor& nchw, cv::cuda::GpuMat& hwc, cudaStream_t stream);

}  // namespace alcedo::cuda::nn
