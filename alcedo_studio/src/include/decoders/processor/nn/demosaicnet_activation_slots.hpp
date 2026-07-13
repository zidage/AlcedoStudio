//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "cuda/nn/workspace.hpp"

namespace alcedo::demosaicnet_slots {
namespace detail {

[[nodiscard]] inline auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

}  // namespace detail

// Aligned activation tensor size (float NCHW) for WorkspacePool bump slots.
[[nodiscard]] inline auto TensorBytes(std::int64_t n, std::int64_t c, std::int64_t h,
                                      std::int64_t w) -> std::size_t {
  if (n <= 0 || c <= 0 || h <= 0 || w <= 0) {
    return 0;
  }
  const std::size_t elems = static_cast<std::size_t>(n) * static_cast<std::size_t>(c) *
                            static_cast<std::size_t>(h) * static_cast<std::size_t>(w);
  return detail::AlignUp(elems * sizeof(float), cuda::nn::WorkspacePool::kDefaultAlignment);
}

// Peak-live activation layout for student DemosaicNet forwards (P1):
//
//   slot A / B  — ping-pong pack + trunk; inactive side reused for residual,
//                 unpack, and concat once stream order makes prior values dead
//   structural  — cropped mosaic, then natural RGB (export-crop path)
//   post        — full-width post-convolution activation
//
// Host-side reuse is valid only because all consumers and later overwrites are
// ordered on the same CUDA stream. Do not share the arena across streams.
struct PeakLiveSlots {
  std::size_t trunk_slot_bytes       = 0;  // size of each of A and B
  std::size_t structural_slot_bytes  = 0;
  std::size_t post_slot_bytes        = 0;
  std::size_t peak_live_bytes        = 0;  // 2*trunk + structural + post
  std::size_t estimate_bytes         = 0;  // peak + scratch headroom

  // Geometry retained for Forward slot views (packed / mosaic / natural).
  std::int64_t pack_h     = 0;
  std::int64_t pack_w     = 0;
  std::int64_t mosaic_h   = 0;  // after trunk
  std::int64_t mosaic_w   = 0;
  std::int64_t unpack_h   = 0;
  std::int64_t unpack_w   = 0;
  std::int64_t natural_h  = 0;
  std::int64_t natural_w  = 0;
};

// Scratch headroom reserved after the live activation set (future op scratch).
inline constexpr std::size_t kScratchHeadroomBytes = 256 * 1024;

// Compute peak-live slot sizes for a student topology.
// pack_out_ch / width / residual_ch / depth / pack_factor match the hard-coded net.
[[nodiscard]] inline auto ComputePeakLiveSlots(int input_h, int input_w, int batch, int pack_out_ch,
                                               int width, int residual_ch, int depth,
                                               int pack_factor) -> PeakLiveSlots {
  PeakLiveSlots out;
  if (batch < 1 || input_h < 1 || input_w < 1 || pack_factor < 1 || depth < 1) {
    return out;
  }
  if ((input_h % pack_factor) != 0 || (input_w % pack_factor) != 0) {
    return out;
  }

  const std::int64_t N  = batch;
  const std::int64_t ph = input_h / pack_factor;
  const std::int64_t pw = input_w / pack_factor;
  if (ph <= 2 * static_cast<std::int64_t>(depth) || pw <= 2 * static_cast<std::int64_t>(depth)) {
    return out;
  }

  const std::int64_t mh = ph - 2 * static_cast<std::int64_t>(depth);
  const std::int64_t mw = pw - 2 * static_cast<std::int64_t>(depth);
  const std::int64_t uh = mh * pack_factor;
  const std::int64_t uw = mw * pack_factor;
  const std::int64_t nh = uh - 2;
  const std::int64_t nw = uw - 2;
  if (mh < 1 || mw < 1 || nh < 1 || nw < 1) {
    return out;
  }

  out.pack_h    = ph;
  out.pack_w    = pw;
  out.mosaic_h  = mh;
  out.mosaic_w  = mw;
  out.unpack_h  = uh;
  out.unpack_w  = uw;
  out.natural_h = nh;
  out.natural_w = nw;

  // Largest pack / first trunk output dominate the ping-pong slots; residual,
  // unpack, and concat reuse the same physical slabs after earlier values die.
  std::size_t trunk = TensorBytes(N, pack_out_ch, ph, pw);
  trunk = std::max(trunk, TensorBytes(N, width, ph - 2, pw - 2));
  trunk = std::max(trunk, TensorBytes(N, residual_ch, mh, mw));
  trunk = std::max(trunk, TensorBytes(N, 3, uh, uw));
  trunk = std::max(trunk, TensorBytes(N, 6, uh, uw));  // concat on inactive trunk slot

  // Cropped mosaic (3, uh, uw) and natural RGB (3, nh, nw) share structural.
  const std::size_t structural =
      std::max(TensorBytes(N, 3, uh, uw), TensorBytes(N, 3, nh, nw));
  const std::size_t post = TensorBytes(N, width, nh, nw);

  out.trunk_slot_bytes      = trunk;
  out.structural_slot_bytes = structural;
  out.post_slot_bytes       = post;
  out.peak_live_bytes       = 2 * trunk + structural + post;
  out.estimate_bytes        = out.peak_live_bytes + kScratchHeadroomBytes;
  return out;
}

}  // namespace alcedo::demosaicnet_slots
