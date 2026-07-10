//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>

#include <opencv2/core/cuda.hpp>

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

struct NeuralEngineCfaPrep {
  bool          succeeded      = false;
  CfaAlignShift shift          = {};
  RawCfaPattern aligned_pattern = {};
  int           aligned_width  = 0;
  int           aligned_height = 0;
  std::string   error;
};

// Product-path sandwich for Neural Engine (not inside hard-coded Forward):
//   1) CFA phase-align crop to training origin + trim to period
//   2) clone ROI so the original linear CFA stays intact for Legacy fallback
//   3) gamma encode x^(1/2.2) without saturating to [0,1]
// White balance is already applied by ToLinearRef; do not re-WB here.
//
// On success, `aligned_cfa` is a private CV_32FC1 buffer ready for pack/forward,
// and `aligned_pattern` is the fixed training pattern (origin GRBG / X-Trans target).
auto PrepareNeuralEngineCfa(const cv::cuda::GpuMat& linear_cfa, const RawCfaPattern& camera_pattern,
                            cv::cuda::GpuMat& aligned_cfa, cv::cuda::Stream* stream = nullptr)
    -> NeuralEngineCfaPrep;

// Gamma decode y^2.2 on CV_32FC3 RGB after the network (sign-preserving, no hard clip).
void FinishNeuralEngineRgb(cv::cuda::GpuMat& rgb, cv::cuda::Stream* stream = nullptr);

// Elementwise signed power on GpuMat (CV_32FC1 or CV_32FC3). Does not clamp to [0,1].
void GammaEncodeGpuMat(cv::cuda::GpuMat& img, cv::cuda::Stream* stream = nullptr);
void GammaDecodeGpuMat(cv::cuda::GpuMat& img, cv::cuda::Stream* stream = nullptr);

}  // namespace alcedo
