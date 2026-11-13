//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <hwy/highway.h>

HWY_BEFORE_NAMESPACE();
namespace alcedo {
namespace HWY_NAMESPACE {
namespace {
namespace hn = hwy::HWY_NAMESPACE;
HWY_INLINE void PixelOffset(const float* HWY_RESTRICT in, float* HWY_RESTRICT out, size_t length,
                            float offset) {
  const hn::ScalableTag<float> d;  // tag for float vectors (scalable/portable)
  const auto                   voffset = Set(d, offset);  // vector filled with offset

  size_t                       i       = 0;
  const size_t                 N       = length;

  // Process full vector lanes
  const size_t                 lanes   = Lanes(d);
  for (; i + lanes <= N; i += lanes) {
    const auto v = Load(d, in + i);  // aligned or unaligned load handled by Load
    const auto r = Add(v, voffset);  // r = v + offset
    Store(r, d, out + i);            // store back
  }

  // Tail: scalar fallback
  for (; i < N; ++i) {
    out[i] = in[i] + offset;
  }
}
}  // namespace
}  // namespace HWY_NAMESPACE
}  // namespace alcedo
HWY_AFTER_NAMESPACE();

#ifdef HWY_ONCE
namespace alcedo {
HWY_INLINE void CallPixelOffset(const float* HWY_RESTRICT in, float* HWY_RESTRICT out,
                                size_t length, float offset) {
  HWY_NAMESPACE::PixelOffset(in, out, length, offset);
}
}  // namespace alcedo
#endif  // HWY_ONCE
