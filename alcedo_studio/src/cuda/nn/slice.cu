//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda/nn/slice.hpp"

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include "cuda/nn/common.hpp"

namespace alcedo::cuda::nn {
namespace {

constexpr int kBlockSize = 256;

void ValidateRank4Nchw(const DeviceTensor& t, const char* what) {
  if (t.rank != 4) {
    throw std::runtime_error(std::string(what) + ": input must be rank-4 NCHW");
  }
  if (t.data == nullptr) {
    throw std::runtime_error(std::string(what) + ": null data");
  }
}

void ValidateChannelRange(const DeviceTensor& input, int channel_start, int channel_count,
                          const char* what) {
  const int C = static_cast<int>(input.shape[1]);
  if (channel_start < 0 || channel_count < 0 || channel_start + channel_count > C) {
    throw std::runtime_error(std::string(what) + ": channel range out of bounds");
  }
}

// Contiguous NCHW channel slice is a contiguous sub-buffer at offset start * H * W (per N...
// actually offset = channel_start * H * W for the first batch, with full buffer layout).
// For multi-batch contiguous: layout is [N][C][H][W], so channels for batch n are not one
// contiguous block across N unless we take the full [N, count, H, W] which IS contiguous
// only when channel_start==0 and we take a prefix, OR when N==1.
//
// For general N and channel_start > 0: the memory layout interleaves batches as
// n0:c0..cC-1, n1:c0..cC-1, so a mid-channel slice is NOT a single contiguous region.
// Strides for a view: shape [N, count, H, W], strides [C*H*W, H*W, W, 1], data += start*H*W.
// IsContiguous() is true iff C == count (full channels) OR N==1 (single batch plane block).
//
// For demosaicnet N=1 always in practice. Views still work with correct strides for N>1.

auto MakeChannelSliceView(const DeviceTensor& input, int channel_start, int channel_count)
    -> DeviceTensor {
  ValidateRank4Nchw(input, "SliceChannelsView");
  if (!input.IsContiguous()) {
    throw std::runtime_error(
        "SliceChannelsView: input must be contiguous NCHW (use SliceChannels to copy)");
  }
  ValidateChannelRange(input, channel_start, channel_count, "SliceChannelsView");

  const std::int64_t N = input.shape[0];
  const std::int64_t H = input.shape[2];
  const std::int64_t W = input.shape[3];
  const std::int64_t plane = H * W;

  DeviceTensor view;
  view.data      = input.data + static_cast<std::int64_t>(channel_start) * plane;
  view.rank      = 4;
  view.shape[0]  = N;
  view.shape[1]  = channel_count;
  view.shape[2]  = H;
  view.shape[3]  = W;
  // Parent strides: [C*H*W, H*W, W, 1]
  view.strides[0] = input.strides[0];  // still full batch stride (C * H * W)
  view.strides[1] = input.strides[1];  // H * W
  view.strides[2] = input.strides[2];  // W
  view.strides[3] = input.strides[3];  // 1
  return view;
}

// Copy channels [start, start+count) from possibly strided input into contiguous out.
__global__ void SliceChannelsKernel(const float* __restrict__ in, float* __restrict__ out, int N,
                                    int C_in, int C_out, int H, int W, int channel_start,
                                    std::int64_t in_stride_n, std::int64_t in_stride_c,
                                    std::int64_t in_stride_h, std::int64_t in_stride_w) {
  (void)C_in;
  const std::int64_t numel = static_cast<std::int64_t>(N) * static_cast<std::int64_t>(C_out) *
                             static_cast<std::int64_t>(H) * static_cast<std::int64_t>(W);
  const std::int64_t grid_stride =
      static_cast<std::int64_t>(blockDim.x) * static_cast<std::int64_t>(gridDim.x);

  for (std::int64_t linear = static_cast<std::int64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
       linear < numel; linear += grid_stride) {
    std::int64_t rem = linear;
    const int    w   = static_cast<int>(rem % W);
    rem /= W;
    const int h = static_cast<int>(rem % H);
    rem /= H;
    const int c = static_cast<int>(rem % C_out);
    const int n = static_cast<int>(rem / C_out);

    const int src_c = c + channel_start;
    const std::int64_t in_off = static_cast<std::int64_t>(n) * in_stride_n +
                                static_cast<std::int64_t>(src_c) * in_stride_c +
                                static_cast<std::int64_t>(h) * in_stride_h +
                                static_cast<std::int64_t>(w) * in_stride_w;
    out[linear] = in[in_off];
  }
}

// Fast contiguous path: for N==1, a single contiguous memcpy of count*H*W floats.
// For N>1, still use the general kernel (or loop of memcpy per batch).
void LaunchSliceCopy(const DeviceTensor& input, DeviceTensor& out, int channel_start,
                     int channel_count, cudaStream_t stream) {
  const int N = static_cast<int>(input.shape[0]);
  const int H = static_cast<int>(input.shape[2]);
  const int W = static_cast<int>(input.shape[3]);
  const int C = static_cast<int>(input.shape[1]);

  const std::int64_t numel = out.Numel();
  if (numel == 0) {
    return;
  }

  // Contiguous N=1: pure D2D memcpy of the channel block.
  if (input.IsContiguous() && out.IsContiguous() && N == 1) {
    const std::int64_t plane  = static_cast<std::int64_t>(H) * static_cast<std::int64_t>(W);
    const float*       src    = input.data + static_cast<std::int64_t>(channel_start) * plane;
    const std::size_t  bytes  = static_cast<std::size_t>(channel_count) * static_cast<std::size_t>(plane) *
                               sizeof(float);
    if (stream == nullptr) {
      CheckCuda(cudaMemcpy(out.data, src, bytes, cudaMemcpyDeviceToDevice),
                "SliceChannels D2D memcpy");
    } else {
      CheckCuda(cudaMemcpyAsync(out.data, src, bytes, cudaMemcpyDeviceToDevice, stream),
                "SliceChannels D2D memcpyAsync");
    }
    return;
  }

  // Contiguous multi-batch: per-batch memcpy of channel_count planes.
  if (input.IsContiguous() && out.IsContiguous()) {
    const std::int64_t plane = static_cast<std::int64_t>(H) * static_cast<std::int64_t>(W);
    const std::int64_t src_batch_stride = static_cast<std::int64_t>(C) * plane;
    const std::int64_t dst_batch_stride = static_cast<std::int64_t>(channel_count) * plane;
    const std::size_t  chunk_bytes =
        static_cast<std::size_t>(channel_count) * static_cast<std::size_t>(plane) * sizeof(float);
    for (int n = 0; n < N; ++n) {
      const float* src = input.data + static_cast<std::int64_t>(n) * src_batch_stride +
                         static_cast<std::int64_t>(channel_start) * plane;
      float* dst = out.data + static_cast<std::int64_t>(n) * dst_batch_stride;
      if (stream == nullptr) {
        CheckCuda(cudaMemcpy(dst, src, chunk_bytes, cudaMemcpyDeviceToDevice),
                  "SliceChannels batch D2D");
      } else {
        CheckCuda(cudaMemcpyAsync(dst, src, chunk_bytes, cudaMemcpyDeviceToDevice, stream),
                  "SliceChannels batch D2D async");
      }
    }
    return;
  }

  const int grid = ChooseGridSize(numel, kBlockSize);
  SliceChannelsKernel<<<grid, kBlockSize, 0, stream>>>(
      input.data, out.data, N, C, channel_count, H, W, channel_start, input.strides[0],
      input.strides[1], input.strides[2], input.strides[3]);
  CheckCuda(cudaGetLastError(), "SliceChannelsKernel launch");
}

}  // namespace

auto SliceChannelsView(const DeviceTensor& input, int channel_start, int channel_count)
    -> DeviceTensor {
  return MakeChannelSliceView(input, channel_start, channel_count);
}

void SliceChannels(const DeviceTensor& input, DeviceTensor& out, int channel_start,
                   int channel_count, cudaStream_t stream) {
  ValidateRank4Nchw(input, "SliceChannels");
  ValidateChannelRange(input, channel_start, channel_count, "SliceChannels");
  if (out.data == nullptr || out.rank != 4) {
    throw std::runtime_error("SliceChannels: out must be rank-4 with valid data");
  }
  if (!out.IsContiguous()) {
    throw std::runtime_error("SliceChannels: out must be contiguous");
  }
  if (out.shape[0] != input.shape[0] || out.shape[1] != channel_count ||
      out.shape[2] != input.shape[2] || out.shape[3] != input.shape[3]) {
    throw std::runtime_error("SliceChannels: out shape must be [N, channel_count, H, W]");
  }

  LaunchSliceCopy(input, out, channel_start, channel_count, ResolveStream(stream));
}

auto SplitChannelsView(const DeviceTensor& input, int first_channels)
    -> std::pair<DeviceTensor, DeviceTensor> {
  ValidateRank4Nchw(input, "SplitChannelsView");
  const int C = static_cast<int>(input.shape[1]);
  if (first_channels < 0 || first_channels > C) {
    throw std::runtime_error("SplitChannelsView: first_channels out of range");
  }
  const int second_channels = C - first_channels;
  auto      first           = MakeChannelSliceView(input, 0, first_channels);
  auto      second          = MakeChannelSliceView(input, first_channels, second_channels);
  return {first, second};
}

void SplitChannels(const DeviceTensor& input, DeviceTensor& first, DeviceTensor& second,
                   int first_channels, cudaStream_t stream, bool prefer_view) {
  ValidateRank4Nchw(input, "SplitChannels");
  const int C = static_cast<int>(input.shape[1]);
  if (first_channels < 0 || first_channels > C) {
    throw std::runtime_error("SplitChannels: first_channels out of range");
  }
  const int second_channels = C - first_channels;

  if (prefer_view && input.IsContiguous()) {
    first  = MakeChannelSliceView(input, 0, first_channels);
    second = MakeChannelSliceView(input, first_channels, second_channels);
    return;
  }

  // Materialize into caller-provided buffers.
  if (first.data == nullptr || second.data == nullptr) {
    throw std::runtime_error(
        "SplitChannels: first/second must be pre-allocated when prefer_view is false "
        "or input is non-contiguous");
  }
  SliceChannels(input, first, 0, first_channels, stream);
  SliceChannels(input, second, first_channels, second_channels, stream);
}

}  // namespace alcedo::cuda::nn
