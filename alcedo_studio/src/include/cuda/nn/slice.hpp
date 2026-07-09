//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cuda_runtime.h>

#include <utility>

#include "cuda/nn/tensor.hpp"

namespace alcedo::cuda::nn {

// Channel-axis slice / split for rank-4 NCHW tensors (f32).
//
// Contiguous NCHW channel ranges are themselves contiguous sub-buffers, so
// SliceChannelsView / SplitChannelsView are zero-copy. Strided inputs require
// the materializing SliceChannels / SplitChannels overloads (copy into out).

// Zero-copy view of channels [channel_start, channel_start + channel_count).
// Requires contiguous rank-4 NCHW. The returned tensor aliases `input` storage.
[[nodiscard]] auto SliceChannelsView(const DeviceTensor& input, int channel_start,
                                     int channel_count) -> DeviceTensor;

// Materializing slice: copies the channel range into contiguous `out`.
// Works for contiguous and strided rank-4 NCHW. `out` shape must be
// [N, channel_count, H, W] and contiguous.
void SliceChannels(const DeviceTensor& input, DeviceTensor& out, int channel_start,
                   int channel_count, cudaStream_t stream = nullptr);

// Zero-copy split of contiguous rank-4 NCHW into [0, first_channels) and
// [first_channels, C). second_channels is implied as C - first_channels.
// Returns a pair of views that alias `input` storage.
[[nodiscard]] auto SplitChannelsView(const DeviceTensor& input, int first_channels)
    -> std::pair<DeviceTensor, DeviceTensor>;

// Materializing split (or view-assign when possible).
//
// If `input` is contiguous rank-4 NCHW and `prefer_view` is true (default),
// `first` and `second` are rewritten as views into `input` (their previous
// data pointers are ignored). Otherwise both must be pre-allocated contiguous
// tensors of shapes [N, first_channels, H, W] and [N, C-first_channels, H, W].
//
// Bayer path: SplitChannels(h, first=64) on conv15 output [N,128,h',w'].
void SplitChannels(const DeviceTensor& input, DeviceTensor& first, DeviceTensor& second,
                   int first_channels, cudaStream_t stream = nullptr,
                   bool prefer_view = true);

}  // namespace alcedo::cuda::nn
