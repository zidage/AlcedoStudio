//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstddef>
#include <cstdint>
#include <stdexcept>

#include "opencl/nn/common.hpp"

namespace alcedo::opencl::nn {

// Non-owning view of a buffer-backed NHWC4 activation tensor.
//
// Storage is float4 channel-blocks:
//   index = (((n * height + y) * width + x) * channel_blocks + cb)
// Logical channels may be less than channel_blocks * 4; padded lanes must be
// zero and ignored by kernels via logical_channels masking.
struct Nhwc4TensorView {
  cl_mem buffer            = nullptr;
  int    batch             = 0;
  int    height            = 0;
  int    width             = 0;
  int    logical_channels  = 0;
  int    channel_blocks    = 0;  // physical float4 blocks; typically ChannelBlocks(logical)
  // Optional byte offset into `buffer` for sub-allocations (workspace slices).
  std::size_t byte_offset  = 0;

  [[nodiscard]] auto empty() const noexcept -> bool {
    return buffer == nullptr || batch <= 0 || height <= 0 || width <= 0 || channel_blocks <= 0;
  }

  // Number of float4 elements in the logical volume.
  [[nodiscard]] auto Float4Count() const -> std::int64_t {
    if (batch < 0 || height < 0 || width < 0 || channel_blocks < 0) {
      throw std::runtime_error("Nhwc4TensorView: negative dimension");
    }
    return static_cast<std::int64_t>(batch) * height * width * channel_blocks;
  }

  // Number of scalar floats in the physical allocation (4 per channel block).
  [[nodiscard]] auto FloatCount() const -> std::int64_t { return Float4Count() * 4; }

  [[nodiscard]] auto ByteSize() const -> std::size_t {
    return static_cast<std::size_t>(FloatCount()) * sizeof(float);
  }

  // Contiguous NHWC4 view over an entire buffer base pointer.
  static auto Contiguous(cl_mem ptr, int n, int h, int w, int logical_c) -> Nhwc4TensorView {
    Nhwc4TensorView view;
    view.buffer           = ptr;
    view.batch            = n;
    view.height           = h;
    view.width            = w;
    view.logical_channels = logical_c;
    view.channel_blocks   = ChannelBlocks(logical_c);
    view.byte_offset      = 0;
    return view;
  }

  // Contiguous view with an explicit physical block count (for padded tensors
  // such as post-network C6 logical in two float4 blocks).
  static auto ContiguousBlocked(cl_mem ptr, int n, int h, int w, int logical_c, int blocks)
      -> Nhwc4TensorView {
    if (blocks < ChannelBlocks(logical_c)) {
      throw std::runtime_error(
          "Nhwc4TensorView::ContiguousBlocked: blocks smaller than logical requirement");
    }
    Nhwc4TensorView view;
    view.buffer           = ptr;
    view.batch            = n;
    view.height           = h;
    view.width            = w;
    view.logical_channels = logical_c;
    view.channel_blocks   = blocks;
    view.byte_offset      = 0;
    return view;
  }
};

// Host-side NHWC4 scalar indexing (tests / packing helpers).
[[nodiscard]] inline auto Nhwc4ScalarIndex(int n, int y, int x, int channel, int height, int width,
                                           int channel_blocks) -> std::size_t {
  const int cb   = channel / 4;
  const int lane = channel % 4;
  return static_cast<std::size_t>((((n * height + y) * width + x) * channel_blocks + cb) * 4 +
                                  lane);
}

}  // namespace alcedo::opencl::nn

#endif  // HAVE_OPENCL
