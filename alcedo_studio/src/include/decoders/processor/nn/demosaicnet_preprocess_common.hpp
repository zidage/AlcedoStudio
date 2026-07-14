//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cmath>
#include <optional>
#include <string>

#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {

// Training CFA origins for bundled demosaicnet weights (raw_pipeline.py /
// BayerMosaickLayer / XTransMosaickLayer). Pack/forward assume these patterns at (0,0).
// Bayer GRBG: G R / B G (rgb indices 1,0 / 2,1).
// X-Trans: fixed 6x6 rgb mask from demosaicnet XTransMosaickLayer.

struct CfaAlignShift {
  int sy = 0;  // crop rows from top
  int sx = 0;  // crop columns from left
};

// Signed gamma encode / decode exponents used by Neural Engine product path.
// Encode: x^(1/2.2); decode: y^2.2. Sign-preserving; no [0,1] clamp.
inline constexpr float kDemosaicNetGammaEncode = 1.0F / 2.2F;
inline constexpr float kDemosaicNetGammaDecode = 2.2F;

// Bayer training origin: GRBG as rgb_fc layout (period 2).
inline constexpr int kDemosaicNetBayerTargetRgb[4] = {1, 0, 2, 1};

// X-Trans training origin as rgb colors, row-major 6x6 (period 6).
inline constexpr int kDemosaicNetXTransTargetRgb[36] = {
    1, 2, 1, 1, 0, 1,  //
    0, 1, 0, 2, 1, 2,  //
    1, 2, 1, 1, 0, 1,  //
    1, 0, 1, 1, 2, 1,  //
    2, 1, 2, 0, 1, 0,  //
    1, 0, 1, 1, 2, 1,  //
};

inline auto DemosaicNetTrainingBayerPattern() -> BayerPattern2x2 {
  // GRBG: G1 R / B G2
  BayerPattern2x2 pattern = {};
  pattern.raw_fc[0]       = 1;
  pattern.raw_fc[1]       = 0;
  pattern.raw_fc[2]       = 2;
  pattern.raw_fc[3]       = 3;
  pattern.rgb_fc[0]       = 1;
  pattern.rgb_fc[1]       = 0;
  pattern.rgb_fc[2]       = 2;
  pattern.rgb_fc[3]       = 1;
  return pattern;
}

inline auto DemosaicNetTrainingXTransPattern() -> XTransPattern6x6 {
  XTransPattern6x6 pattern = {};
  for (int i = 0; i < 36; ++i) {
    pattern.rgb_fc[i] = kDemosaicNetXTransTargetRgb[i];
    // Dual-green not distinguished in the training mask; use green=1 for all greens.
    pattern.raw_fc[i] = kDemosaicNetXTransTargetRgb[i];
  }
  return pattern;
}

inline auto DemosaicNetTrainingPattern(const RawCfaKind kind) -> RawCfaPattern {
  RawCfaPattern pattern = {};
  if (kind == RawCfaKind::XTrans6x6) {
    pattern.kind           = RawCfaKind::XTrans6x6;
    pattern.xtrans_pattern = DemosaicNetTrainingXTransPattern();
    return pattern;
  }
  pattern.kind          = RawCfaKind::Bayer2x2;
  pattern.bayer_pattern = DemosaicNetTrainingBayerPattern();
  return pattern;
}

inline auto CfaPeriod(const RawCfaKind kind) -> int {
  return kind == RawCfaKind::XTrans6x6 ? 6 : 2;
}

// Search cyclic shifts (sy,sx) in [0, period) so cropping the camera CFA by that
// shift yields the training target at the origin. Compares R/G/B only (dual green
// collapsed). Returns nullopt when no pure cyclic shift matches (rotated/reflected).
inline auto FindCfaAlignShift(const RawCfaPattern& camera_pattern)
    -> std::optional<CfaAlignShift> {
  const int period = CfaPeriod(camera_pattern.kind);
  for (int sy = 0; sy < period; ++sy) {
    for (int sx = 0; sx < period; ++sx) {
      bool ok = true;
      for (int i = 0; i < period && ok; ++i) {
        for (int j = 0; j < period; ++j) {
          const int camera_rgb = RgbColorAt(camera_pattern, i + sy, j + sx);
          const int target_rgb =
              camera_pattern.kind == RawCfaKind::XTrans6x6
                  ? kDemosaicNetXTransTargetRgb[i * 6 + j]
                  : kDemosaicNetBayerTargetRgb[BayerCellIndex(i, j)];
          if (camera_rgb != target_rgb) {
            ok = false;
            break;
          }
        }
      }
      if (ok) {
        return CfaAlignShift{sy, sx};
      }
    }
  }
  return std::nullopt;
}

// OpenCV BORDER_REFLECT_101 mapping (used by Neural tile packing).
// Backend-neutral host form; GPU kernels may reimplement the same arithmetic.
[[nodiscard]] inline auto Reflect101(const int coordinate, const int limit) -> int {
  if (limit <= 1) {
    return 0;
  }
  int reflected = coordinate;
  while (reflected < 0 || reflected >= limit) {
    reflected = reflected < 0 ? -reflected : 2 * limit - reflected - 2;
  }
  return reflected;
}

// Sign-preserving power used by gamma encode/decode (host reference / CPU paths).
[[nodiscard]] inline auto PowSigned(const float x, const float gamma) -> float {
  if (x == 0.0F) {
    return 0.0F;
  }
  return std::copysign(std::pow(std::fabs(x), gamma), x);
}

// Phase-align crop dimensions: after shifting by (sy,sx), trim to CFA period.
struct NeuralAlignedGeometry {
  int shift_sy       = 0;
  int shift_sx       = 0;
  int aligned_width  = 0;
  int aligned_height = 0;
  int min_spatial    = 0;
};

// Compute phase-align + period-trim geometry without touching image data.
// Returns nullopt-style failure via out_error when geometry is invalid.
[[nodiscard]] inline auto ComputeNeuralAlignedGeometry(
    const RawCfaPattern& camera_pattern, const int image_width, const int image_height,
    const int min_spatial, std::string* out_error = nullptr) -> std::optional<NeuralAlignedGeometry> {
  const auto shift = FindCfaAlignShift(camera_pattern);
  if (!shift.has_value()) {
    if (out_error != nullptr) {
      *out_error =
          "CFA cannot be aligned to DemosaicNet training origin by a cyclic shift "
          "(rotated/reflected CFA is unsupported)";
    }
    return std::nullopt;
  }

  const int period  = CfaPeriod(camera_pattern.kind);
  const int avail_h = image_height - shift->sy;
  const int avail_w = image_width - shift->sx;
  if (avail_h < period || avail_w < period) {
    if (out_error != nullptr) {
      *out_error = "CFA is too small after phase-align crop";
    }
    return std::nullopt;
  }

  const int aligned_h = avail_h - (avail_h % period);
  const int aligned_w = avail_w - (avail_w % period);
  if (aligned_h < min_spatial || aligned_w < min_spatial) {
    if (out_error != nullptr) {
      *out_error = "CFA is smaller than the Neural Engine minimum after phase-align";
    }
    return std::nullopt;
  }

  NeuralAlignedGeometry geo;
  geo.shift_sy       = shift->sy;
  geo.shift_sx       = shift->sx;
  geo.aligned_width  = aligned_w;
  geo.aligned_height = aligned_h;
  geo.min_spatial    = min_spatial;
  return geo;
}

}  // namespace alcedo
