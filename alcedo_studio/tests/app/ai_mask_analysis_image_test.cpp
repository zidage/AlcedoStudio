//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM4 plan S2 to S4: the render request of the AI Mask analysis image and the conversion of its
// display output to the RGB8 input of the segmentation session.

#include "app/ai_mask_analysis_image.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <opencv2/core.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

auto MakeRendered(int width, int height, const cv::Vec4f& fill) -> RenderedPipelineImage {
  RenderedPipelineImage rendered;
  cv::Mat pixels(height, width, CV_32FC4, cv::Scalar(fill[0], fill[1], fill[2], fill[3]));
  rendered.pixels                         = std::make_shared<ImageBuffer>(std::move(pixels));
  rendered.geometry.render_extent         = {static_cast<std::uint32_t>(width),
                                             static_cast<std::uint32_t>(height)};
  rendered.geometry.full_reference_extent = rendered.geometry.render_extent;
  return rendered;
}

auto ConversionError(const RenderedPipelineImage& rendered) -> std::string {
  try {
    (void)ConvertAiMaskAnalysisImage(rendered);
  } catch (const std::invalid_argument& ex) {
    return ex.what();
  }
  return {};
}

TEST(AiMaskAnalysisImageTest, RenderRequestIsTheWholeReferenceSourceFrameAt4096) {
  const auto request = MakeAiMaskAnalysisRenderRequest();
  EXPECT_EQ(request.document_geometry, DocumentGeometryUse::ReferenceSource);
  EXPECT_EQ(request.resolution.max_edge, kAiMaskAnalysisMaxLongEdge);
  EXPECT_EQ(request.resolution.quality, RenderQuality::Preview);
  EXPECT_TRUE(request.view.visible_rect_in_edit_space.IsFullFrame());
  EXPECT_EQ(request.view.viewport_extent, (Extent2D{}));
}

TEST(AiMaskAnalysisImageTest, ConversionRoundsToRgb8AndDropsAlpha) {
  auto  rendered             = MakeRendered(3, 2, {0.0f, 0.0f, 0.0f, 1.0f});
  auto& pixels               = rendered.pixels->GetCPUData();
  pixels.at<cv::Vec4f>(0, 0) = {1.0f, 0.5f, 0.25f, 0.0f};
  pixels.at<cv::Vec4f>(0, 2) = {-0.2f, 1.7f, 0.1f, 1.0f};
  pixels.at<cv::Vec4f>(1, 1) = {0.002f, 0.998f, 0.5019608f, 1.0f};

  const auto image           = ConvertAiMaskAnalysisImage(rendered);
  ASSERT_EQ(image.width, 3u);
  ASSERT_EQ(image.height, 2u);
  ASSERT_EQ(image.pixels.size(), 3u * 2u * 3u);
  const auto rgb = [&image](int row, int col) {
    const auto offset = (static_cast<std::size_t>(row) * image.width + col) * 3U;
    return std::vector<std::uint8_t>{image.pixels[offset], image.pixels[offset + 1],
                                     image.pixels[offset + 2]};
  };
  // 0.5 * 255 = 127.5 rounds to even (128); 0.25 * 255 = 63.75 rounds to 64.
  EXPECT_EQ(rgb(0, 0), (std::vector<std::uint8_t>{255, 128, 64}));
  EXPECT_EQ(rgb(0, 1), (std::vector<std::uint8_t>{0, 0, 0}));
  EXPECT_EQ(rgb(0, 2), (std::vector<std::uint8_t>{0, 255, 26})) << "values saturate";
  EXPECT_EQ(rgb(1, 1), (std::vector<std::uint8_t>{1, 254, 128}));
}

TEST(AiMaskAnalysisImageTest, ConversionRejectsHdrNonFiniteAndSizeErrors) {
  auto st2084                  = MakeRendered(4, 4, {0.5f, 0.5f, 0.5f, 1.0f});
  st2084.display.encoding_eotf = ColorUtils::EOTF::ST2084;
  EXPECT_NE(ConversionError(st2084).find("ST 2084"), std::string::npos);

  auto hlg                  = MakeRendered(4, 4, {0.5f, 0.5f, 0.5f, 1.0f});
  hlg.display.encoding_eotf = ColorUtils::EOTF::HLG;
  EXPECT_NE(ConversionError(hlg).find("HLG"), std::string::npos);

  auto nan_value = MakeRendered(4, 4, {0.5f, 0.5f, 0.5f, 1.0f});
  nan_value.pixels->GetCPUData().at<cv::Vec4f>(2, 3)[1] = std::nanf("");
  EXPECT_NE(ConversionError(nan_value).find("non-finite"), std::string::npos);

  auto size_mismatch                   = MakeRendered(4, 4, {0.5f, 0.5f, 0.5f, 1.0f});
  size_mismatch.geometry.render_extent = {4, 5};
  EXPECT_NE(ConversionError(size_mismatch).find("render geometry"), std::string::npos);

  const auto too_large =
      MakeRendered(static_cast<int>(kAiMaskAnalysisMaxLongEdge) + 1, 2, {0.5f, 0.5f, 0.5f, 1.0f});
  EXPECT_NE(ConversionError(too_large).find("long edge limit"), std::string::npos);

  RenderedPipelineImage no_pixels;
  EXPECT_NE(ConversionError(no_pixels).find("no host pixels"), std::string::npos);
}

TEST(AiMaskAnalysisImageTest, GeometryFingerprintTracksLensAndRawDecodeOnly) {
  const auto fingerprint_after = [](const std::function<void(DevelopParamsModel&)>& change) {
    auto document = CreateDefaultPipelineDocument();
    change(document.Develop()->Params());
    return MakeAiMaskGeometryFingerprint(document.Develop()->Params());
  };
  const auto base = fingerprint_after([](DevelopParamsModel&) {});
  ASSERT_EQ(base.size(), 32u);
  EXPECT_EQ(base.find_first_not_of("0123456789abcdef"), std::string::npos);
  EXPECT_EQ(fingerprint_after([](DevelopParamsModel&) {}), base);

  EXPECT_NE(fingerprint_after([](DevelopParamsModel& develop) {
              develop.ApplyLensCalibrationUpdate({.lens_enabled = true});
            }),
            base);
  EXPECT_NE(fingerprint_after([](DevelopParamsModel& develop) {
              develop.ApplyLensCalibrationUpdate({.apply_distortion = false});
            }),
            base);
  EXPECT_NE(
      fingerprint_after([](DevelopParamsModel& develop) {
        develop.ApplyLensCalibrationUpdate({.lens_maker = "Zeiss", .lens_model = "Touit 1.8/32"});
      }),
      base);
  EXPECT_NE(fingerprint_after([](DevelopParamsModel& develop) {
              develop.ApplyRawDecodeUpdate({.demosaic_method = "neural_engine"});
            }),
            base);
  EXPECT_NE(fingerprint_after([](DevelopParamsModel& develop) {
              develop.ApplyRawDecodeUpdate({.highlights_reconstruct = false});
            }),
            base);

  EXPECT_EQ(fingerprint_after([](DevelopParamsModel& develop) {
              develop.ApplyRawDecodeUpdate({.use_camera_wb = false, .user_wb = 4300.0f});
            }),
            base);
  EXPECT_EQ(fingerprint_after([](DevelopParamsModel& develop) {
              develop.ApplyColorTemperatureUpdate(
                  {.wb_mode = "custom", .custom_cct = 3200.0f, .custom_tint = 12.0f});
            }),
            base);
  EXPECT_EQ(fingerprint_after([](DevelopParamsModel& develop) {
              develop.ApplyLensCalibrationUpdate({.apply_vignetting = false});
            }),
            base);
}

}  // namespace
}  // namespace alcedo
