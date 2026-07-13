//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>
#include <libraw/libraw.h>
#include <libraw/libraw_const.h>

#include <filesystem>
#include <memory>
#include <opencv2/core.hpp>

#include "decoders/processor/raw_processor_internal.hpp"

namespace alcedo::detail {
namespace {

auto MakeD800eSizes() -> libraw_image_sizes_t {
  libraw_image_sizes_t sizes{};
  sizes.raw_width   = 7424;
  sizes.raw_height  = 4924;
  sizes.width       = 7378;
  sizes.height      = 4924;
  sizes.left_margin = 0;
  sizes.top_margin  = 0;
  sizes.iwidth      = 7378;
  sizes.iheight     = 4924;
  return sizes;
}

auto MakeFullActiveSizes(const ushort width, const ushort height) -> libraw_image_sizes_t {
  libraw_image_sizes_t sizes{};
  sizes.raw_width   = width;
  sizes.raw_height  = height;
  sizes.width       = width;
  sizes.height      = height;
  sizes.left_margin = 0;
  sizes.top_margin  = 0;
  sizes.iwidth      = width;
  sizes.iheight     = height;
  return sizes;
}

TEST(RawProcessorCropTest, NormalDecodeCropUsesLibRawActiveAreaForNikonD800e) {
  const libraw_image_sizes_t sizes      = MakeD800eSizes();
  const ushort               no_crop[4] = {};

  const cv::Rect crop = BuildDecodeCropRect(sizes, no_crop, cv::Size(7424, 4924), DecodeRes::FULL);

  EXPECT_EQ(crop, cv::Rect(0, 0, 7378, 4924));
}

TEST(RawProcessorCropTest, RcdDecodeCropInsetsLibRawActiveAreaForNikonD800e) {
  const libraw_image_sizes_t sizes      = MakeD800eSizes();
  const ushort               no_crop[4] = {};

  const cv::Rect             crop =
      BuildRcdDecodeCropRect(sizes, no_crop, cv::Size(7416, 4916), DecodeRes::FULL);

  EXPECT_EQ(crop, cv::Rect(0, 0, 7370, 4916));
}

TEST(RawProcessorCropTest, NikonD800eFixtureExposesRightMarginThatRcdCropMustAvoid) {
  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  ASSERT_TRUE(std::filesystem::exists(raw_path)) << raw_path.string();

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  const ushort no_crop[4] = {};

  EXPECT_EQ(raw->imgdata.sizes.raw_width, 7424);
  EXPECT_EQ(raw->imgdata.sizes.raw_height, 4924);
  EXPECT_EQ(raw->imgdata.sizes.width, 7378);
  EXPECT_EQ(raw->imgdata.sizes.height, 4924);
  EXPECT_EQ(raw->imgdata.sizes.left_margin, 0);
  EXPECT_EQ(raw->imgdata.sizes.top_margin, 0);

  const cv::Rect normal_crop = BuildDecodeCropRect(
      raw->imgdata.sizes, no_crop,
      cv::Size(raw->imgdata.sizes.raw_width, raw->imgdata.sizes.raw_height), DecodeRes::FULL);
  EXPECT_EQ(normal_crop, cv::Rect(0, 0, 7378, 4924));

  const cv::Rect rcd_crop =
      BuildRcdDecodeCropRect(raw->imgdata.sizes, no_crop, cv::Size(7416, 4916), DecodeRes::FULL);
  EXPECT_EQ(rcd_crop, cv::Rect(0, 0, 7370, 4916));

  raw->recycle();
}

TEST(RawProcessorCropTest, RcdDecodeCropKeepsFullPostRcdOutputWhenActiveAreaIsFullRaw) {
  const libraw_image_sizes_t sizes      = MakeFullActiveSizes(100, 80);
  const ushort               no_crop[4] = {};

  const cv::Rect crop = BuildRcdDecodeCropRect(sizes, no_crop, cv::Size(92, 72), DecodeRes::FULL);

  EXPECT_EQ(crop, cv::Rect(0, 0, 92, 72));
}

TEST(RawProcessorCropTest, RcdDecodeCropPreservesScaledActiveOrigin) {
  libraw_image_sizes_t sizes{};
  sizes.raw_width           = 120;
  sizes.raw_height          = 90;
  sizes.left_margin         = 10;
  sizes.top_margin          = 6;
  sizes.width               = 80;
  sizes.height              = 60;
  const ushort   no_crop[4] = {};

  const cv::Rect crop = BuildRcdDecodeCropRect(sizes, no_crop, cv::Size(112, 82), DecodeRes::FULL);

  EXPECT_EQ(crop, cv::Rect(10, 6, 72, 52));
}

TEST(RawProcessorCropTest, RcdDecodeCropInsetsExplicitDefaultCrop) {
  const libraw_image_sizes_t sizes           = MakeD800eSizes();
  const ushort               default_crop[4] = {8, 6, 7344, 4900};

  const cv::Rect             crop =
      BuildRcdDecodeCropRect(sizes, default_crop, cv::Size(7416, 4916), DecodeRes::FULL);

  EXPECT_EQ(crop, cv::Rect(8, 6, 7336, 4892));
}

TEST(RawProcessorCropTest, RcdDecodeCropUsesDownsampledRcdRadius) {
  const libraw_image_sizes_t sizes      = MakeD800eSizes();
  const ushort               no_crop[4] = {};

  const cv::Rect             crop =
      BuildRcdDecodeCropRect(sizes, no_crop, cv::Size(3704, 2454), DecodeRes::HALF);

  EXPECT_EQ(crop, cv::Rect(0, 0, 3681, 2454));
}

TEST(RawProcessorCropTest, DefaultDemosaicUsesLegacyForBayerAndNeuralEngineForXTrans) {
  RawParams params;
  params.decode_res_      = DecodeRes::FULL;
  params.demosaic_method_ = RawDemosaicMethod::Default;

  EXPECT_EQ(ResolveRawDemosaicMethod(params, RawCfaKind::Bayer2x2), RawDemosaicMethod::Legacy);
  EXPECT_EQ(ResolveRawDemosaicMethod(params, RawCfaKind::XTrans6x6),
            RawDemosaicMethod::NeuralEngine);
}

TEST(RawProcessorCropTest, ReducedResolutionAlwaysUsesLegacyDemosaic) {
  RawParams params;
  params.demosaic_method_ = RawDemosaicMethod::NeuralEngine;

  for (const DecodeRes decode_res : {DecodeRes::HALF, DecodeRes::QUARTER, DecodeRes::EIGHTH}) {
    params.decode_res_ = decode_res;
    EXPECT_EQ(ResolveRawDemosaicMethod(params, RawCfaKind::Bayer2x2), RawDemosaicMethod::Legacy);
    EXPECT_EQ(ResolveRawDemosaicMethod(params, RawCfaKind::XTrans6x6), RawDemosaicMethod::Legacy);
  }
}

TEST(RawProcessorCropTest, ExplicitFullResolutionDemosaicChoiceIsHonored) {
  RawParams params;
  params.decode_res_      = DecodeRes::FULL;
  params.demosaic_method_ = RawDemosaicMethod::Legacy;
  EXPECT_EQ(ResolveRawDemosaicMethod(params, RawCfaKind::XTrans6x6), RawDemosaicMethod::Legacy);

  params.demosaic_method_ = RawDemosaicMethod::NeuralEngine;
  EXPECT_EQ(ResolveRawDemosaicMethod(params, RawCfaKind::Bayer2x2),
            RawDemosaicMethod::NeuralEngine);
}

TEST(RawProcessorCropTest, SelectCudaExecutionMode_NeuralEngineDefaultsToTiled) {
  RawParams params;
  params.gpu_backend_     = RawGpuBackend::CUDA;
  params.decode_res_      = DecodeRes::FULL;
  params.demosaic_method_ = RawDemosaicMethod::NeuralEngine;

  RawCfaPattern bayer;
  bayer.kind = RawCfaKind::Bayer2x2;
  EXPECT_EQ(SelectCudaExecutionMode(params, bayer, cv::Rect(0, 0, 128, 96)),
            CudaExecutionMode::Tiled);

  RawCfaPattern xtrans;
  xtrans.kind = RawCfaKind::XTrans6x6;
  EXPECT_EQ(SelectCudaExecutionMode(params, xtrans, cv::Rect(0, 0, 128, 96)),
            CudaExecutionMode::Tiled);
}

TEST(RawProcessorCropTest, SelectCudaExecutionMode_LegacyStillUsesLongEdgeThreshold) {
  RawParams params;
  params.gpu_backend_     = RawGpuBackend::CUDA;
  params.decode_res_      = DecodeRes::FULL;
  params.demosaic_method_ = RawDemosaicMethod::Legacy;
  RawCfaPattern bayer;
  bayer.kind = RawCfaKind::Bayer2x2;

  EXPECT_EQ(SelectCudaExecutionMode(params, bayer, cv::Rect(0, 0, 8999, 3000)),
            CudaExecutionMode::FullFrame);
  EXPECT_EQ(SelectCudaExecutionMode(params, bayer, cv::Rect(0, 0, 9001, 3000)),
            CudaExecutionMode::Tiled);
}

TEST(RawProcessorCropTest, NeuralBorderCropUsesValidConvolutionLoss) {
  const libraw_image_sizes_t sizes      = MakeFullActiveSizes(100, 80);
  const ushort               no_crop[4] = {};

  const cv::Rect             crop =
      BuildBorderLossDecodeCropRect(sizes, no_crop, cv::Size(38, 18), DecodeRes::FULL, 31);

  EXPECT_EQ(crop, cv::Rect(0, 0, 38, 18));
}

// Purpose: student tiled geometry does not re-subtract tile border; phase crop alone
// maps original CFA → same-size aligned RGB assembly.
TEST(RawProcessorCropTest, StudentTiledNeuralGeometryMapsCropWithoutBorderLoss) {
  const libraw_image_sizes_t sizes      = MakeFullActiveSizes(200, 160);
  const ushort               no_crop[4] = {};
  const cv::Size             original(200, 160);
  // Phase crop (2, 0) → aligned 198×160; virtual pad restores same-size RGB.
  const NeuralOutputGeometry geometry =
      MakeStudentTiledNeuralOutputGeometry(2, 0, cv::Size(198, 160));
  EXPECT_EQ(geometry.output_origin_in_aligned, cv::Point(0, 0));
  EXPECT_EQ(geometry.output_size, cv::Size(198, 160));

  const cv::Rect crop =
      BuildNeuralEngineDecodeCropRect(sizes, no_crop, original, DecodeRes::FULL, geometry);
  // Active area is full original; in RGB: x from 0-2 mapped → clamp to 0, width 200-2=198.
  EXPECT_EQ(crop, cv::Rect(0, 0, 198, 160));
}

// Purpose: natural-shrink full-frame geometry still subtracts the equal border once.
TEST(RawProcessorCropTest, NaturalShrinkNeuralGeometrySubtractsBorderOnce) {
  const libraw_image_sizes_t sizes      = MakeFullActiveSizes(100, 80);
  const ushort               no_crop[4] = {};
  const cv::Size             original(100, 80);
  const NeuralOutputGeometry geometry =
      MakeNaturalShrinkNeuralOutputGeometry(0, 0, cv::Size(100, 80), 17);
  EXPECT_EQ(geometry.output_size, cv::Size(66, 46));
  EXPECT_EQ(geometry.output_origin_in_aligned, cv::Point(17, 17));

  const cv::Rect crop =
      BuildNeuralEngineDecodeCropRect(sizes, no_crop, original, DecodeRes::FULL, geometry);
  // Full active (0,0,100,80) → RGB (0-17, 0-17) clamped → (0,0) to (66,46).
  EXPECT_EQ(crop, cv::Rect(0, 0, 66, 46));
}

}  // namespace
}  // namespace alcedo::detail
