//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Backend-neutral CFA alignment, Reflect101, and signed-gamma constants.

#include <gtest/gtest.h>

#include <string>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/demosaicnet_specs.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {
namespace {

// Camera pattern such that cropping by (sy,sx) restores the training origin:
// camera(y,x) = training(y-sy, x-sx) with wrap.
auto MakeBayerCameraWithAlignShift(const int sy, const int sx) -> RawCfaPattern {
  const auto training = DemosaicNetTrainingBayerPattern();
  RawCfaPattern pattern = {};
  pattern.kind          = RawCfaKind::Bayer2x2;
  for (int y = 0; y < 2; ++y) {
    for (int x = 0; x < 2; ++x) {
      const int dst = BayerCellIndex(y, x);
      const int src_y = WrapPatternCoord(y - sy, 2);
      const int src_x = WrapPatternCoord(x - sx, 2);
      pattern.bayer_pattern.rgb_fc[dst] = RgbColorAt(training, src_y, src_x);
      pattern.bayer_pattern.raw_fc[dst] = pattern.bayer_pattern.rgb_fc[dst] == 1
                                              ? 1
                                              : pattern.bayer_pattern.rgb_fc[dst];
    }
  }
  return pattern;
}

auto MakeXTransCameraWithAlignShift(const int sy, const int sx) -> RawCfaPattern {
  const auto training = DemosaicNetTrainingXTransPattern();
  RawCfaPattern pattern = {};
  pattern.kind          = RawCfaKind::XTrans6x6;
  for (int y = 0; y < 6; ++y) {
    for (int x = 0; x < 6; ++x) {
      const int dst = XTransCellIndex(y, x);
      pattern.xtrans_pattern.rgb_fc[dst] =
          RgbColorAt(training, WrapPatternCoord(y - sy, 6), WrapPatternCoord(x - sx, 6));
      pattern.xtrans_pattern.raw_fc[dst] = pattern.xtrans_pattern.rgb_fc[dst];
    }
  }
  return pattern;
}

}  // namespace

// Purpose: every pure Bayer cyclic phase maps to a shift that yields GRBG at (0,0).
TEST(DemosaicNetPreprocessCommonTest, FindCfaAlignShiftMatchesGrbgOriginForCommonBayerPhases) {
  const struct {
    const char* name;
    int         rgb[4];
  } phases[] = {
      {"RGGB", {0, 1, 1, 2}},
      {"GRBG", {1, 0, 2, 1}},
      {"GBRG", {1, 2, 0, 1}},
      {"BGGR", {2, 1, 1, 0}},
  };

  for (const auto& phase : phases) {
    RawCfaPattern pattern;
    pattern.kind = RawCfaKind::Bayer2x2;
    for (int i = 0; i < 4; ++i) {
      pattern.bayer_pattern.rgb_fc[i] = phase.rgb[i];
      pattern.bayer_pattern.raw_fc[i] = phase.rgb[i] == 1 ? 1 : phase.rgb[i];
    }
    const auto shift = FindCfaAlignShift(pattern);
    ASSERT_TRUE(shift.has_value()) << phase.name;
    for (int i = 0; i < 2; ++i) {
      for (int j = 0; j < 2; ++j) {
        const int camera = RgbColorAt(pattern, i + shift->sy, j + shift->sx);
        const int target = kDemosaicNetBayerTargetRgb[BayerCellIndex(i, j)];
        EXPECT_EQ(camera, target) << phase.name << " at (" << i << "," << j << ")";
      }
    }
  }
}

// Purpose: all Bayer cyclic shifts (sy,sx) are recovered exactly.
TEST(DemosaicNetPreprocessCommonTest, FindCfaAlignShiftRecoversAllBayerCyclicShifts) {
  for (int sy = 0; sy < 2; ++sy) {
    for (int sx = 0; sx < 2; ++sx) {
      const auto pattern = MakeBayerCameraWithAlignShift(sy, sx);
      const auto shift   = FindCfaAlignShift(pattern);
      ASSERT_TRUE(shift.has_value()) << "sy=" << sy << " sx=" << sx;
      EXPECT_EQ(shift->sy, sy) << "sx=" << sx;
      EXPECT_EQ(shift->sx, sx) << "sy=" << sy;
    }
  }
}

// Purpose: every pure X-Trans cyclic camera phase admits a shift that yields the
// training origin. The training mask has internal symmetries, so the recovered
// (sy,sx) may be a smaller equivalent shift (first match in search order).
TEST(DemosaicNetPreprocessCommonTest, FindCfaAlignShiftAlignsAllXTransCyclicShiftsToTrainingOrigin) {
  for (int sy = 0; sy < 6; ++sy) {
    for (int sx = 0; sx < 6; ++sx) {
      const auto pattern = MakeXTransCameraWithAlignShift(sy, sx);
      const auto shift   = FindCfaAlignShift(pattern);
      ASSERT_TRUE(shift.has_value()) << "constructed sy=" << sy << " sx=" << sx;
      for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) {
          const int camera = RgbColorAt(pattern, i + shift->sy, j + shift->sx);
          const int target = kDemosaicNetXTransTargetRgb[i * 6 + j];
          EXPECT_EQ(camera, target)
              << "constructed (" << sy << "," << sx << ") recovered (" << shift->sy << ","
              << shift->sx << ") at (" << i << "," << j << ")";
        }
      }
    }
  }
}

// Purpose: a known non-ambiguous X-Trans cyclic offset is recovered exactly.
TEST(DemosaicNetPreprocessCommonTest, FindCfaAlignShiftRecoversKnownXTransOffsetExactly) {
  constexpr int kSy = 2;
  constexpr int kSx = 3;
  const auto pattern = MakeXTransCameraWithAlignShift(kSy, kSx);
  const auto shift   = FindCfaAlignShift(pattern);
  ASSERT_TRUE(shift.has_value());
  EXPECT_EQ(shift->sy, kSy);
  EXPECT_EQ(shift->sx, kSx);
}

// Purpose: non-cyclic CFA permutation is not phase-alignable.
TEST(DemosaicNetPreprocessCommonTest, FindCfaAlignShiftRejectsUnsupportedBayerPermutation) {
  RawCfaPattern pattern;
  pattern.kind                    = RawCfaKind::Bayer2x2;
  // Non-Bayer permutation: R R / B B (no greens) — not cyclic-equivalent to GRBG.
  pattern.bayer_pattern.rgb_fc[0] = 0;
  pattern.bayer_pattern.rgb_fc[1] = 0;
  pattern.bayer_pattern.rgb_fc[2] = 2;
  pattern.bayer_pattern.rgb_fc[3] = 2;
  for (int i = 0; i < 4; ++i) {
    pattern.bayer_pattern.raw_fc[i] = pattern.bayer_pattern.rgb_fc[i];
  }
  EXPECT_FALSE(FindCfaAlignShift(pattern).has_value());
}

// Purpose: Reflect101 matches OpenCV BORDER_REFLECT_101 at interior, edges, and outside.
TEST(DemosaicNetPreprocessCommonTest, Reflect101MapsInteriorEdgesAndOutsideCoordinates) {
  EXPECT_EQ(Reflect101(0, 5), 0);
  EXPECT_EQ(Reflect101(4, 5), 4);
  EXPECT_EQ(Reflect101(-1, 5), 1);
  EXPECT_EQ(Reflect101(5, 5), 3);
  EXPECT_EQ(Reflect101(-2, 5), 2);
  EXPECT_EQ(Reflect101(6, 5), 2);
  EXPECT_EQ(Reflect101(0, 1), 0);
  EXPECT_EQ(Reflect101(99, 1), 0);
  EXPECT_EQ(Reflect101(-3, 4), 3);
}

// Purpose: signed gamma encode/decode exponents and host PowSigned round-trip.
TEST(DemosaicNetPreprocessCommonTest, SignedGammaEncodeDecodeRoundTripOnNegativeAndOverRange) {
  EXPECT_FLOAT_EQ(kDemosaicNetGammaEncode, 1.0F / 2.2F);
  EXPECT_FLOAT_EQ(kDemosaicNetGammaDecode, 2.2F);

  const float samples[] = {-2.0F, -0.25F, 0.0F, 0.5F, 1.0F, 2.5F};
  for (const float x : samples) {
    const float encoded = PowSigned(x, kDemosaicNetGammaEncode);
    const float decoded = PowSigned(encoded, kDemosaicNetGammaDecode);
    if (x == 0.0F) {
      EXPECT_FLOAT_EQ(decoded, 0.0F);
    } else {
      EXPECT_NEAR(decoded, x, 1e-5F) << "x=" << x;
    }
    if (x < 0.0F) {
      EXPECT_LT(encoded, 0.0F);
    } else if (x > 0.0F) {
      EXPECT_GT(encoded, 0.0F);
    }
  }
}

// Purpose: ComputeNeuralAlignedGeometry phase-aligns and period-trims Bayer dimensions.
TEST(DemosaicNetPreprocessCommonTest, ComputeNeuralAlignedGeometryTrimsBayerToPeriod) {
  RawCfaPattern pattern = {};
  pattern.kind          = RawCfaKind::Bayer2x2;
  pattern.bayer_pattern = DemosaicNetTrainingBayerPattern();

  std::string error;
  const auto geo =
      ComputeNeuralAlignedGeometry(pattern, 101, 103, DemosaicNetBayerSpec::kMinSpatial, &error);
  ASSERT_TRUE(geo.has_value()) << error;
  EXPECT_EQ(geo->shift_sx, 0);
  EXPECT_EQ(geo->shift_sy, 0);
  EXPECT_EQ(geo->aligned_width, 100);
  EXPECT_EQ(geo->aligned_height, 102);
}

// Purpose: ComputeNeuralAlignedGeometry rejects frames smaller than min_spatial after align.
TEST(DemosaicNetPreprocessCommonTest, ComputeNeuralAlignedGeometryRejectsTooSmallFrame) {
  RawCfaPattern pattern = {};
  pattern.kind          = RawCfaKind::Bayer2x2;
  pattern.bayer_pattern = DemosaicNetTrainingBayerPattern();

  std::string error;
  const auto geo =
      ComputeNeuralAlignedGeometry(pattern, 20, 20, DemosaicNetBayerSpec::kMinSpatial, &error);
  EXPECT_FALSE(geo.has_value());
  EXPECT_FALSE(error.empty());
}

// Purpose: CfaPeriod matches Bayer period 2 and X-Trans period 6.
TEST(DemosaicNetPreprocessCommonTest, CfaPeriodMatchesVariantContract) {
  EXPECT_EQ(CfaPeriod(RawCfaKind::Bayer2x2), 2);
  EXPECT_EQ(CfaPeriod(RawCfaKind::XTrans6x6), 6);
}

}  // namespace alcedo
