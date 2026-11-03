//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/drt/drt_output_resolver.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

#include "edit/graph/drt_node_model.hpp"

namespace alcedo {
namespace {

// A resolved value that no valid resolution produces, so an untouched output is visible.
auto SentinelOutput() -> ColorUtils::TO_OUTPUT_Params {
  ColorUtils::TO_OUTPUT_Params output;
  output.peak_luminance_       = -7.0f;
  output.display_linear_scale_ = -7.0f;
  return output;
}

void ExpectUnchanged(const ColorUtils::TO_OUTPUT_Params& output) {
  EXPECT_EQ(output.peak_luminance_, -7.0f);
  EXPECT_EQ(output.display_linear_scale_, -7.0f);
  EXPECT_EQ(output.aces_params_.table_hues_, nullptr);
}

TEST(DrtOutputResolver, DrtOutputResolverRejectsUnknownEncodingEotf) {
  DrtPayload payload;
  payload.encoding_eotf = static_cast<DrtEotf>(42);
  auto        output    = SentinelOutput();
  std::string error;
  EXPECT_FALSE(DrtOutputResolver::Resolve(payload, nullptr, &output, &error));
  EXPECT_EQ(error, "DrtOutputResolver: unsupported DRT encoding EOTF 42.");
  ExpectUnchanged(output);
}

TEST(DrtOutputResolver, DrtOutputResolverRejectsUnknownMethodAndColorSpaces) {
  auto        output = SentinelOutput();
  std::string error;

  DrtPayload  method;
  method.method = static_cast<DrtMethod>(9);
  EXPECT_FALSE(DrtOutputResolver::Resolve(method, nullptr, &output, &error));
  EXPECT_EQ(error, "DrtOutputResolver: unsupported DRT method 9.");

  DrtPayload encoding;
  encoding.encoding_space = static_cast<DrtColorSpace>(7);
  EXPECT_FALSE(DrtOutputResolver::Resolve(encoding, nullptr, &output, &error));
  EXPECT_EQ(error, "DrtOutputResolver: unsupported DRT encoding space 7.");

  DrtPayload limiting;
  limiting.method         = DrtMethod::Aces20;
  limiting.limiting_space = static_cast<DrtColorSpace>(5);
  EXPECT_FALSE(DrtOutputResolver::Resolve(limiting, nullptr, &output, &error));
  EXPECT_EQ(error, "DrtOutputResolver: unsupported DRT limiting space 5.");
  ExpectUnchanged(output);
}

TEST(DrtOutputResolver, DrtOutputResolverRejectsNonPositivePeakLuminance) {
  DrtPayload payload;
  payload.peak_luminance = 0.0f;
  auto        output     = SentinelOutput();
  std::string error;
  EXPECT_FALSE(DrtOutputResolver::Resolve(payload, nullptr, &output, &error));
  EXPECT_EQ(error, "DrtOutputResolver: peak_luminance must be positive.");
  ExpectUnchanged(output);

  ExportColorProfileConfig export_encoding;
  export_encoding.peak_luminance = -1.0f;
  payload.peak_luminance         = 100.0f;
  EXPECT_FALSE(DrtOutputResolver::Resolve(payload, &export_encoding, &output, &error));
  ExpectUnchanged(output);
}

TEST(DrtOutputResolver, DrtOutputResolverRejectsUnsupportedOpenDrtEncodingPair) {
  DrtPayload payload;
  payload.method         = DrtMethod::OpenDrt;
  payload.encoding_space = DrtColorSpace::Rec709;
  payload.encoding_eotf  = DrtEotf::Hlg;
  auto        output     = SentinelOutput();
  std::string error;
  EXPECT_FALSE(DrtOutputResolver::Resolve(payload, nullptr, &output, &error));
  EXPECT_EQ(error,
            "DrtOutputResolver: ResolveOpenDRTRuntime: unsupported OpenDRT output combination for "
            "encoding_space=\"rec709\" and encoding_eotf=\"hlg\".");
  ExpectUnchanged(output);
}

TEST(DrtOutputResolver, OpenDrtResolvesSrgbPiecewiseForRec709AndP3D65) {
  for (const auto [space, encoding_space, display_gamut] :
       {std::tuple{DrtColorSpace::Rec709, ColorUtils::ColorSpace::REC709, 0},
        std::tuple{DrtColorSpace::P3D65, ColorUtils::ColorSpace::P3_D65, 1}}) {
    DrtPayload payload;
    payload.method         = DrtMethod::OpenDrt;
    payload.encoding_space = space;
    payload.encoding_eotf  = DrtEotf::SrgbPiecewise;
    ColorUtils::TO_OUTPUT_Params output;
    std::string                  error;
    ASSERT_TRUE(DrtOutputResolver::Resolve(payload, nullptr, &output, &error)) << error;
    EXPECT_EQ(output.method_, ColorUtils::ODTMethod::OPEN_DRT);
    EXPECT_EQ(output.encoding_space_, encoding_space);
    EXPECT_EQ(output.eotf_, ColorUtils::EOTF::SRGB_PIECEWISE);
    EXPECT_EQ(output.display_linear_scale_, 1.0f);
    EXPECT_EQ(output.open_drt_params_.display_gamut_, display_gamut);
    EXPECT_EQ(output.open_drt_params_.surround_, 2);
    EXPECT_EQ(output.open_drt_params_.display_eotf_, 1);

    // The sRGB display shares the gamma 2.2 tonescale; only the encoding curve differs.
    DrtPayload gamma_payload    = payload;
    gamma_payload.encoding_eotf = DrtEotf::Gamma22;
    ColorUtils::TO_OUTPUT_Params gamma_output;
    ASSERT_TRUE(DrtOutputResolver::Resolve(gamma_payload, nullptr, &gamma_output, &error)) << error;
    EXPECT_EQ(output.open_drt_params_.ts_s1_, gamma_output.open_drt_params_.ts_s1_);
    EXPECT_EQ(output.open_drt_params_.ts_dsc_, gamma_output.open_drt_params_.ts_dsc_);
  }

  // Export to an sRGB file with OpenDRT resolves through the same display encoding.
  DrtPayload payload;
  payload.method = DrtMethod::OpenDrt;
  ExportColorProfileConfig     export_encoding{ColorUtils::ColorSpace::REC709,
                                           ColorUtils::EOTF::SRGB_PIECEWISE, 100.0f};
  ColorUtils::TO_OUTPUT_Params output;
  std::string                  error;
  ASSERT_TRUE(DrtOutputResolver::Resolve(payload, &export_encoding, &output, &error)) << error;
  EXPECT_EQ(output.eotf_, ColorUtils::EOTF::SRGB_PIECEWISE);
}

TEST(DrtOutputResolver, AcesResolutionFillsTablesAndPqDisplayScale) {
  DrtPayload payload;
  payload.method         = DrtMethod::Aces20;
  payload.encoding_space = DrtColorSpace::Rec2020;
  payload.encoding_eotf  = DrtEotf::St2084;
  payload.limiting_space = DrtColorSpace::Rec2020;
  payload.peak_luminance = 1000.0f;
  ColorUtils::TO_OUTPUT_Params output;
  std::string                  error;
  ASSERT_TRUE(DrtOutputResolver::Resolve(payload, nullptr, &output, &error)) << error;
  EXPECT_EQ(output.method_, ColorUtils::ODTMethod::ACES_2_0);
  EXPECT_EQ(output.encoding_space_, ColorUtils::ColorSpace::REC2020);
  EXPECT_EQ(output.eotf_, ColorUtils::EOTF::ST2084);
  EXPECT_EQ(output.peak_luminance_, 1000.0f);
  EXPECT_EQ(output.display_linear_scale_, ColorUtils::ref_lum);
  EXPECT_NE(output.aces_params_.table_reach_M_, nullptr);
  EXPECT_NE(output.aces_params_.table_hues_, nullptr);
  EXPECT_NE(output.aces_params_.table_upper_hull_gammas_, nullptr);
  EXPECT_NE(output.aces_params_.table_gamut_cusps_, nullptr);
}

TEST(DrtOutputResolver, ExportEncodingReplacesPayloadEncodingButNotLimitingSpace) {
  DrtPayload payload;
  payload.method         = DrtMethod::Aces20;
  payload.limiting_space = DrtColorSpace::P3D65;
  ExportColorProfileConfig export_encoding{ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::HLG,
                                           1000.0f};
  ColorUtils::TO_OUTPUT_Params output;
  std::string                  error;
  ASSERT_TRUE(DrtOutputResolver::Resolve(payload, &export_encoding, &output, &error)) << error;
  EXPECT_EQ(output.encoding_space_, ColorUtils::ColorSpace::REC2020);
  EXPECT_EQ(output.eotf_, ColorUtils::EOTF::HLG);
  EXPECT_EQ(output.peak_luminance_, 1000.0f);
  const cv::Matx33f p3_to_2020 = ColorUtils::RGB_TO_XYZ_f33(ColorUtils::ColorSpace::P3_D65) *
                                 ColorUtils::XYZ_TO_RGB_f33(ColorUtils::ColorSpace::REC2020);
  for (int i = 0; i < 9; ++i) {
    EXPECT_EQ(output.limit_to_display_matx_.val[i], p3_to_2020.val[i]) << i;
  }

  // Export spaces outside the DRT encoding domain encode as Rec.709, as the export path did
  // before G10.5.
  export_encoding.encoding_space = ColorUtils::ColorSpace::PROPHOTO;
  ASSERT_TRUE(DrtOutputResolver::Resolve(payload, &export_encoding, &output, &error)) << error;
  EXPECT_EQ(output.encoding_space_, ColorUtils::ColorSpace::REC709);
}

auto FilmPrintPayload() -> DrtPayload {
  DrtPayload payload;
  payload.method         = DrtMethod::FilmPrint;
  payload.encoding_space = DrtColorSpace::Rec709;
  payload.encoding_eotf  = DrtEotf::Gamma22;
  return payload;
}

TEST(DrtOutputResolver, FilmPrintResolvesRec709SdrEncodings) {
  for (const auto [eotf, resolved_eotf] :
       {std::tuple{DrtEotf::Gamma22, ColorUtils::EOTF::GAMMA_2_2},
        std::tuple{DrtEotf::SrgbPiecewise, ColorUtils::EOTF::SRGB_PIECEWISE},
        std::tuple{DrtEotf::Bt1886, ColorUtils::EOTF::BT1886},
        std::tuple{DrtEotf::Gamma26, ColorUtils::EOTF::GAMMA_2_6},
        std::tuple{DrtEotf::Gamma18, ColorUtils::EOTF::GAMMA_1_8}}) {
    auto payload          = FilmPrintPayload();
    payload.encoding_eotf = eotf;
    ColorUtils::TO_OUTPUT_Params output;
    std::string                  error;
    ASSERT_TRUE(DrtOutputResolver::Resolve(payload, nullptr, &output, &error)) << error;
    EXPECT_EQ(output.method_, ColorUtils::ODTMethod::FILM_PRINT);
    EXPECT_EQ(output.encoding_space_, ColorUtils::ColorSpace::REC709);
    EXPECT_EQ(output.eotf_, resolved_eotf);
    EXPECT_EQ(output.display_linear_scale_, 1.0f);
  }
}

TEST(DrtOutputResolver, FilmPrintRejectsHdrAndLinearEncodings) {
  for (const auto [eotf, name] :
       {std::tuple{DrtEotf::St2084, "st2084"}, std::tuple{DrtEotf::Hlg, "hlg"},
        std::tuple{DrtEotf::Linear, "linear"}}) {
    auto payload          = FilmPrintPayload();
    payload.encoding_eotf = eotf;
    auto        output    = SentinelOutput();
    std::string error;
    EXPECT_FALSE(DrtOutputResolver::Resolve(payload, nullptr, &output, &error));
    EXPECT_EQ(error, std::string{"DrtOutputResolver: the film print encodes only SDR display "
                                 "curves, not "} +
                         name + ".");
    ExpectUnchanged(output);
  }
}

TEST(DrtOutputResolver, FilmPrintRejectsAnEncodingSpaceOtherThanRec709) {
  for (const auto [space, name] : {std::tuple{DrtColorSpace::Rec2020, "rec2020"},
                                   std::tuple{DrtColorSpace::P3D65, "p3_d65"}}) {
    auto payload           = FilmPrintPayload();
    payload.encoding_space = space;
    auto        output     = SentinelOutput();
    std::string error;
    EXPECT_FALSE(DrtOutputResolver::Resolve(payload, nullptr, &output, &error));
    EXPECT_EQ(error, std::string{"DrtOutputResolver: the film print encodes only Rec. 709, not "} +
                         name + ".");
    ExpectUnchanged(output);
  }
}

TEST(DrtOutputResolver, FilmPrintExportOverrideFollowsTheSameRule) {
  const auto                   payload = FilmPrintPayload();
  std::string                  error;

  ExportColorProfileConfig     sdr{ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::SRGB_PIECEWISE,
                               100.0f};
  ColorUtils::TO_OUTPUT_Params output;
  ASSERT_TRUE(DrtOutputResolver::Resolve(payload, &sdr, &output, &error)) << error;
  EXPECT_EQ(output.eotf_, ColorUtils::EOTF::SRGB_PIECEWISE);

  const ExportColorProfileConfig rejected[] = {
      {ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::ST2084, 1000.0f},
      {ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::HLG, 1000.0f},
      {ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::LINEAR, 100.0f},
      {ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::GAMMA_2_2, 100.0f},
      {ColorUtils::ColorSpace::P3_D65, ColorUtils::EOTF::GAMMA_2_2, 100.0f},
      // Other methods encode this space as Rec. 709. The film print stops.
      {ColorUtils::ColorSpace::PROPHOTO, ColorUtils::EOTF::GAMMA_2_2, 100.0f},
  };
  for (const auto& config : rejected) {
    auto refused = SentinelOutput();
    EXPECT_FALSE(DrtOutputResolver::Resolve(payload, &config, &refused, &error));
    EXPECT_NE(error.find("the film print encodes only"), std::string::npos) << error;
    ExpectUnchanged(refused);
  }

  // The override replaces the stored encoding: a stored HDR encoding does not stop an SDR export.
  auto stored_hdr           = payload;
  stored_hdr.encoding_space = DrtColorSpace::Rec2020;
  stored_hdr.encoding_eotf  = DrtEotf::St2084;
  EXPECT_TRUE(DrtOutputResolver::Resolve(stored_hdr, &sdr, &output, &error)) << error;
}

TEST(DrtOutputResolver, FilmPrintHdrThrowsFromResolveNode) {
  auto node             = DrtNodeModel::MakeDefault(NodeId{"drt"});
  auto payload          = FilmPrintPayload();
  payload.encoding_eotf = DrtEotf::St2084;
  node->Params().ReplaceParams(payload);
  try {
    (void)DrtOutputResolver::ResolveNode(*node, std::nullopt, "ExecuteTestDrt");
    FAIL() << "ResolveNode accepted a film print with HDR";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()),
              "ExecuteTestDrt: DrtOutputResolver: the film print encodes only SDR display curves, "
              "not st2084.");
  }
}

TEST(DrtOutputResolver, ResolveNodeThrowsWithCallerPrefixAndReadsNodeParameters) {
  auto node             = DrtNodeModel::MakeDefault(NodeId{"drt"});
  auto payload          = node->Params().Params();
  payload.encoding_eotf = static_cast<DrtEotf>(42);
  node->Params().ReplaceParams(payload);
  try {
    (void)DrtOutputResolver::ResolveNode(*node, std::nullopt, "ExecuteTestDrt");
    FAIL() << "ResolveNode accepted an unknown EOTF";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()),
              "ExecuteTestDrt: DrtOutputResolver: unsupported DRT encoding EOTF 42.");
  }

  payload.encoding_space = DrtColorSpace::Rec2020;
  payload.encoding_eotf  = DrtEotf::Hlg;
  payload.peak_luminance = 400.0f;
  node->Params().ReplaceParams(payload);
  const auto resolved = DrtOutputResolver::ResolveNode(*node, std::nullopt, "ExecuteTestDrt");
  EXPECT_EQ(resolved.method_, ColorUtils::ODTMethod::OPEN_DRT);
  EXPECT_EQ(resolved.eotf_, ColorUtils::EOTF::HLG);
  EXPECT_EQ(resolved.peak_luminance_, 400.0f);
}

}  // namespace
}  // namespace alcedo
