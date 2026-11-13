//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Resolution of the source color description of JPEG, PNG, TIFF and OpenEXR files
// (docs/roadmap/alcedo_studio/edit/raster_image_input_plan.md, sections 4 and 7.3). The
// fixtures come from tests/resources/raster/generate_raster_fixtures.py; each fixture name
// states the description that it carries.

#include "image/raster_color_description.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "utils/hash/sha256.hpp"

namespace alcedo {
namespace {

auto FixturePath(const std::string& name) -> std::filesystem::path {
  return std::filesystem::path(ALCEDO_RASTER_FIXTURE_DIR) / name;
}

auto ReadFixture(const std::string& name) -> std::vector<std::byte> {
  std::ifstream in(FixturePath(name), std::ios::binary);
  if (!in) {
    ADD_FAILURE() << "missing fixture " << FixturePath(name).string();
    return {};
  }
  const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  for (std::size_t i = 0; i < chars.size(); ++i) {
    bytes[i] = static_cast<std::byte>(chars[i]);
  }
  return bytes;
}

auto Resolve(const std::string& name, RasterFileKind kind) -> RasterColorDescription {
  const auto bytes = ReadFixture(name);
  return ResolveRasterColorDescription(bytes, kind);
}

void ExpectPrimariesNear(const RasterColorDescription& actual, const std::array<float, 8>& expected,
                         float tolerance = 2e-3f) {
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_NEAR(actual.primaries_xy_[i], expected[i], tolerance) << "chromaticity index " << i;
  }
}

void ExpectAllChannels(const RasterColorDescription& actual, RasterTransferKind kind) {
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_EQ(actual.transfer_[c].kind_, kind) << "channel " << c;
  }
}

void ExpectGamma(const RasterColorDescription& actual, float gamma, float tolerance = 5e-3f) {
  ExpectAllChannels(actual, RasterTransferKind::Gamma);
  for (std::size_t c = 0; c < 3; ++c) {
    EXPECT_NEAR(actual.transfer_[c].gamma_, gamma, tolerance) << "channel " << c;
  }
}

auto IsLowerHexSha256(const std::string& value) -> bool {
  if (value.size() != 64) {
    return false;
  }
  for (const char ch : value) {
    if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) {
      return false;
    }
  }
  return true;
}

// -------------------------------------------------------------------------------------------
// ICC
// -------------------------------------------------------------------------------------------

TEST(RasterColorDescriptionTest, IccMatrixShaperYieldsNativePrimariesAfterChadInverse) {
  const auto description = Resolve("display_p3_icc_8bit.jpg", RasterFileKind::Jpeg);
  EXPECT_EQ(description.origin_, RasterColorOrigin::IccMatrixShaper);
  EXPECT_EQ(description.referral_, RasterReferral::DisplayReferred);
  // The colorants are stored D50-adapted; the chad inverse restores the D65 native white.
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::P3D65));
  ExpectAllChannels(description, RasterTransferKind::SrgbPiecewise);
  EXPECT_EQ(description.profile_description_, "Display P3");
  EXPECT_TRUE(IsLowerHexSha256(description.icc_sha256_)) << description.icc_sha256_;
  EXPECT_FLOAT_EQ(description.peak_luminance_nits_, 100.0f);
  EXPECT_TRUE(description.default_reason_.empty());
}

TEST(RasterColorDescriptionTest, IccV2WithoutChadAdaptsFromMediaWhiteWithBradford) {
  const auto rec2020 = Resolve("rec2020_icc_16bit.tif", RasterFileKind::Tiff);
  EXPECT_EQ(rec2020.origin_, RasterColorOrigin::IccMatrixShaper);
  ExpectPrimariesNear(rec2020, color::GamutPrimariesXy(color::ColorGamutId::Rec2020));
  ExpectGamma(rec2020, 2.4f);

  const auto prophoto = Resolve("prophoto_icc_16bit.tif", RasterFileKind::Tiff);
  EXPECT_EQ(prophoto.referral_, RasterReferral::DisplayReferred);
  ExpectPrimariesNear(prophoto, color::GamutPrimariesXy(color::ColorGamutId::ProPhoto));
  ExpectGamma(prophoto, 1.8f);
}

TEST(RasterColorDescriptionTest, IccV44CicpTagOverridesTrc) {
  // The profile's TRC tags say gamma 2.2 with sRGB colorants; its cicp tag says Rec.2020 PQ.
  const auto description = Resolve("icc_v44_cicp_rec2020_pq_16bit.tif", RasterFileKind::Tiff);
  EXPECT_EQ(description.origin_, RasterColorOrigin::IccCicp);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec2020));
  ExpectAllChannels(description, RasterTransferKind::St2084);
  EXPECT_FLOAT_EQ(description.peak_luminance_nits_, 1000.0f);
  EXPECT_TRUE(IsLowerHexSha256(description.icc_sha256_));
}

TEST(RasterColorDescriptionTest, LutBasedRgbIccResolvesToLinearRec2020Converted) {
  const auto description = Resolve("lut_based_rgb_icc.tif", RasterFileKind::Tiff);
  EXPECT_EQ(description.origin_, RasterColorOrigin::IccLutConverted);
  EXPECT_EQ(description.referral_, RasterReferral::DisplayReferred);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec2020), 1e-6f);
  ExpectAllChannels(description, RasterTransferKind::Linear);
  EXPECT_EQ(description.profile_description_, "Synthetic LUT sRGB");
  // The decoder checks this hash before it runs the profile conversion.
  EXPECT_TRUE(IsLowerHexSha256(description.icc_sha256_));
}

TEST(RasterColorDescriptionTest, GrayIccOnSingleChannelImageUsesSrgbPrimariesAndGrayCurve) {
  const auto description = Resolve("gray_gamma22.png", RasterFileKind::Png);
  EXPECT_EQ(description.origin_, RasterColorOrigin::IccMatrixShaper);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec709));
  ExpectGamma(description, 2.2f);
}

TEST(RasterColorDescriptionTest, CmykIccIsRejected) {
  const auto bytes = ReadFixture("cmyk_icc.jpg");
  try {
    (void)ResolveRasterColorDescription(bytes, RasterFileKind::Jpeg);
    FAIL() << "CMYK JPEG resolved to a description";
  } catch (const RasterColorDescriptionError& error) {
    EXPECT_EQ(error.reason(), RasterColorDescriptionError::Reason::UnsupportedCmyk);
  }
}

TEST(RasterColorDescriptionTest, UnusableIccFallsBackToSrgbAndRecordsReason) {
  // The ICC colorants of red and green are equal, so the primaries form no triangle.
  const auto description = Resolve("degenerate_primaries_icc_8bit.jpg", RasterFileKind::Jpeg);
  EXPECT_EQ(description.origin_, RasterColorOrigin::DefaultSrgb);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec709), 1e-6f);
  ExpectAllChannels(description, RasterTransferKind::SrgbPiecewise);
  EXPECT_NE(description.default_reason_.find("degenerate"), std::string::npos)
      << description.default_reason_;
  EXPECT_TRUE(description.icc_sha256_.empty());
}

TEST(RasterColorDescriptionTest, MalformedPngIsReportedAsMalformedContainer) {
  auto bytes = ReadFixture("srgb_chunk.png");
  ASSERT_GT(bytes.size(), 40u);
  bytes.resize(20);  // Cut inside the IHDR chunk.
  try {
    (void)ResolveRasterColorDescription(bytes, RasterFileKind::Png);
    FAIL() << "truncated PNG resolved to a description";
  } catch (const RasterColorDescriptionError& error) {
    EXPECT_EQ(error.reason(), RasterColorDescriptionError::Reason::MalformedContainer);
  }
}

// -------------------------------------------------------------------------------------------
// PNG
// -------------------------------------------------------------------------------------------

TEST(RasterColorDescriptionTest, PngCicpOverridesIccpAndSrgbChunk) {
  const auto description = Resolve("cicp_rec2020_pq_16bit.png", RasterFileKind::Png);
  EXPECT_EQ(description.origin_, RasterColorOrigin::PngCicp);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec2020), 1e-6f);
  ExpectAllChannels(description, RasterTransferKind::St2084);
  // mDCv states a 4000-nit mastering display.
  EXPECT_FLOAT_EQ(description.peak_luminance_nits_, 4000.0f);
  EXPECT_TRUE(description.icc_sha256_.empty());
}

TEST(RasterColorDescriptionTest, PngIccpWinsOverMissingCicp) {
  const auto description = Resolve("iccp_display_p3.png", RasterFileKind::Png);
  EXPECT_EQ(description.origin_, RasterColorOrigin::IccMatrixShaper);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::P3D65));
  ExpectAllChannels(description, RasterTransferKind::SrgbPiecewise);
}

TEST(RasterColorDescriptionTest, PngSrgbChunkResolvesSrgb) {
  const auto description = Resolve("srgb_chunk.png", RasterFileKind::Png);
  EXPECT_EQ(description.origin_, RasterColorOrigin::PngSrgbChunk);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec709), 1e-6f);
  ExpectAllChannels(description, RasterTransferKind::SrgbPiecewise);
}

TEST(RasterColorDescriptionTest, PngGamaAndChrmGivePrimariesAndGamma) {
  const auto description = Resolve("gama_chrm_only.png", RasterFileKind::Png);
  EXPECT_EQ(description.origin_, RasterColorOrigin::PngGamaChrm);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec2020), 1e-5f);
  ExpectGamma(description, 1.8f, 1e-3f);
}

TEST(RasterColorDescriptionTest, PngGamaWithoutChrmUsesSrgbPrimaries) {
  const auto description = Resolve("gama_without_chrm.png", RasterFileKind::Png);
  EXPECT_EQ(description.origin_, RasterColorOrigin::PngGamaChrm);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec709), 1e-6f);
  ExpectGamma(description, 2.2f, 1e-3f);
}

TEST(RasterColorDescriptionTest, UntaggedPaletteAndAlphaPngUseTheirChunksOrSrgbDefault) {
  const auto palette = Resolve("palette_untagged.png", RasterFileKind::Png);
  EXPECT_EQ(palette.origin_, RasterColorOrigin::DefaultSrgb);
  EXPECT_FALSE(palette.default_reason_.empty());
  ExpectAllChannels(palette, RasterTransferKind::SrgbPiecewise);

  const auto rgba = Resolve("rgba_with_alpha.png", RasterFileKind::Png);
  EXPECT_EQ(rgba.origin_, RasterColorOrigin::PngSrgbChunk);
}

// -------------------------------------------------------------------------------------------
// JPEG
// -------------------------------------------------------------------------------------------

TEST(RasterColorDescriptionTest, JpegExifR03WithoutIccResolvesAdobeRgb) {
  const auto description = Resolve("adobe_rgb_exif_r03_no_icc.jpg", RasterFileKind::Jpeg);
  EXPECT_EQ(description.origin_, RasterColorOrigin::ExifInteropAdobeRgb);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::AdobeRgb), 1e-6f);
  ExpectGamma(description, CE_ADOBE_RGB_GAMMA, 1e-6f);
  EXPECT_EQ(description.referral_, RasterReferral::DisplayReferred);
}

TEST(RasterColorDescriptionTest, JpegIccAndUntaggedJpegResolve) {
  const auto srgb = Resolve("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  EXPECT_EQ(srgb.origin_, RasterColorOrigin::IccMatrixShaper);
  ExpectPrimariesNear(srgb, color::GamutPrimariesXy(color::ColorGamutId::Rec709));
  ExpectAllChannels(srgb, RasterTransferKind::SrgbPiecewise);

  const auto untagged = Resolve("untagged_8bit.jpg", RasterFileKind::Jpeg);
  EXPECT_EQ(untagged.origin_, RasterColorOrigin::DefaultSrgb);
  EXPECT_FALSE(untagged.default_reason_.empty());
  ExpectPrimariesNear(untagged, color::GamutPrimariesXy(color::ColorGamutId::Rec709), 1e-6f);
}

// -------------------------------------------------------------------------------------------
// TIFF and OpenEXR
// -------------------------------------------------------------------------------------------

TEST(RasterColorDescriptionTest, Float32TiffWithoutIccIsSceneLinear) {
  const auto description = Resolve("float32_no_icc.tif", RasterFileKind::Tiff);
  EXPECT_EQ(description.referral_, RasterReferral::SceneLinear);
  EXPECT_EQ(description.origin_, RasterColorOrigin::DefaultSrgb);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec709), 1e-6f);
  ExpectAllChannels(description, RasterTransferKind::Linear);
  EXPECT_FALSE(description.default_reason_.empty());
}

TEST(RasterColorDescriptionTest, Float32TiffReferralFollowsIccCurveLinearity) {
  const auto linear = Resolve("float32_linear_icc.tif", RasterFileKind::Tiff);
  EXPECT_EQ(linear.referral_, RasterReferral::SceneLinear);
  EXPECT_EQ(linear.origin_, RasterColorOrigin::IccMatrixShaper);
  ExpectAllChannels(linear, RasterTransferKind::Linear);

  const auto srgb = Resolve("float32_srgb_icc.tif", RasterFileKind::Tiff);
  EXPECT_EQ(srgb.referral_, RasterReferral::DisplayReferred);
  ExpectAllChannels(srgb, RasterTransferKind::SrgbPiecewise);
}

TEST(RasterColorDescriptionTest, IntegerTiffWithoutIccIsDisplayReferredSrgb) {
  const auto description = Resolve("uncompressed_rgb16_no_make.tif", RasterFileKind::Tiff);
  EXPECT_EQ(description.referral_, RasterReferral::DisplayReferred);
  EXPECT_EQ(description.origin_, RasterColorOrigin::DefaultSrgb);
  ExpectAllChannels(description, RasterTransferKind::SrgbPiecewise);
}

TEST(RasterColorDescriptionTest, ExrWithoutChromaticitiesResolvesRec709Linear) {
  const auto description = Resolve("no_chromaticities_float.exr", RasterFileKind::OpenExr);
  EXPECT_EQ(description.referral_, RasterReferral::SceneLinear);
  EXPECT_EQ(description.origin_, RasterColorOrigin::DefaultSrgb);
  ExpectPrimariesNear(description, color::GamutPrimariesXy(color::ColorGamutId::Rec709), 1e-6f);
  ExpectAllChannels(description, RasterTransferKind::Linear);
}

TEST(RasterColorDescriptionTest, ExrChromaticitiesAndAcesContainerFlagGivePrimaries) {
  const auto p3 = Resolve("chromaticities_p3_half.exr", RasterFileKind::OpenExr);
  EXPECT_EQ(p3.origin_, RasterColorOrigin::ExrChromaticities);
  EXPECT_EQ(p3.referral_, RasterReferral::SceneLinear);
  ExpectPrimariesNear(p3, color::GamutPrimariesXy(color::ColorGamutId::P3D65), 1e-6f);
  ExpectAllChannels(p3, RasterTransferKind::Linear);

  const auto aces = Resolve("aces_container_flag.exr", RasterFileKind::OpenExr);
  EXPECT_EQ(aces.origin_, RasterColorOrigin::ExrAcesContainer);
  ExpectPrimariesNear(aces, color::GamutPrimariesXy(color::ColorGamutId::Ap0), 1e-6f);
  ExpectAllChannels(aces, RasterTransferKind::Linear);
}

// -------------------------------------------------------------------------------------------
// Re-import of Alcedo exports
// -------------------------------------------------------------------------------------------

struct ExportProfileExpectation {
  const char*          profile_stem_;
  std::array<float, 8> primaries_;
  RasterTransferKind   transfer_;
  float                gamma_;
  float                peak_nits_;
};

constexpr std::array<float, 8> kP3D60            = {0.680f, 0.320f, 0.265f,   0.690f,
                                                    0.150f, 0.060f, 0.32168f, 0.33767f};
// The shipped xyz_gamma26.icc states these chromaticities (its `chrm` tag agrees) with the
// DCI white.
constexpr std::array<float, 8> kXyzExportProfile = {0.9642f, 0.0001f, 0.0001f, 0.8249f,
                                                    0.0001f, 0.0001f, 0.314f,  0.351f};

TEST(RasterColorDescriptionTest, ExportedIccProfilesReimportWithSamePrimariesAndTransfer) {
  const std::array<ExportProfileExpectation, 12> expectations = {{
      {"p3_d60_gamma26", kP3D60, RasterTransferKind::Gamma, 2.6f, 100.0f},
      {"p3_d65_gamma22", color::GamutPrimariesXy(color::ColorGamutId::P3D65),
       RasterTransferKind::Gamma, 2.2f, 100.0f},
      {"p3_d65_pq", color::GamutPrimariesXy(color::ColorGamutId::P3D65), RasterTransferKind::St2084,
       0.0f, 1000.0f},
      {"p3_dci_gamma26", color::GamutPrimariesXy(color::ColorGamutId::P3Dci),
       RasterTransferKind::Gamma, 2.6f, 100.0f},
      {"rec2020_hlg", color::GamutPrimariesXy(color::ColorGamutId::Rec2020),
       RasterTransferKind::Hlg, 0.0f, 1000.0f},
      {"rec2020_pq", color::GamutPrimariesXy(color::ColorGamutId::Rec2020),
       RasterTransferKind::St2084, 0.0f, 1000.0f},
      {"rec709_bt1886", color::GamutPrimariesXy(color::ColorGamutId::Rec709),
       RasterTransferKind::Gamma, 2.4f, 100.0f},
      {"rec709_gamma22", color::GamutPrimariesXy(color::ColorGamutId::Rec709),
       RasterTransferKind::Gamma, 2.2f, 100.0f},
      {"srgb_piecewise", color::GamutPrimariesXy(color::ColorGamutId::Rec709),
       RasterTransferKind::SrgbPiecewise, 0.0f, 100.0f},
      {"upstream_displayp3_compat_v4", color::GamutPrimariesXy(color::ColorGamutId::P3D65),
       RasterTransferKind::SrgbPiecewise, 0.0f, 100.0f},
      {"upstream_rec2020_v4", color::GamutPrimariesXy(color::ColorGamutId::Rec2020),
       RasterTransferKind::IccParametric, 0.0f, 100.0f},
      {"xyz_gamma26", kXyzExportProfile, RasterTransferKind::Gamma, 2.6f, 100.0f},
  }};
  for (const auto& expected : expectations) {
    SCOPED_TRACE(expected.profile_stem_);
    const auto description = Resolve(
        std::string("alcedo_export_") + expected.profile_stem_ + ".tif", RasterFileKind::Tiff);
    EXPECT_EQ(description.origin_, RasterColorOrigin::IccMatrixShaper);
    EXPECT_EQ(description.referral_, RasterReferral::DisplayReferred);
    ExpectPrimariesNear(description, expected.primaries_, 3e-3f);
    ExpectAllChannels(description, expected.transfer_);
    if (expected.transfer_ == RasterTransferKind::Gamma) {
      ExpectGamma(description, expected.gamma_);
    }
    EXPECT_FLOAT_EQ(description.peak_luminance_nits_, expected.peak_nits_);
  }
}

TEST(RasterColorDescriptionTest, IccParametricCurveKeepsTypeAndParameters) {
  // Rec.2020 v4 stores the BT.709 inverse OETF as ICC parametric function type 3.
  const auto  description = Resolve("alcedo_export_upstream_rec2020_v4.tif", RasterFileKind::Tiff);
  const auto& transfer    = description.transfer_[0];
  ASSERT_EQ(transfer.kind_, RasterTransferKind::IccParametric);
  EXPECT_EQ(transfer.icc_parametric_type_, 3);
  EXPECT_NEAR(transfer.icc_params_[0], 1.0f / 0.45f, 1e-3f);
  EXPECT_NEAR(transfer.icc_params_[1], 1.0f / 1.099f, 1e-3f);
  EXPECT_NEAR(transfer.icc_params_[3], 1.0f / 4.5f, 1e-3f);
  EXPECT_NEAR(EvaluateRasterTransfer(transfer, 0.5f),
              std::pow((0.5f + 0.099f) / 1.099f, 1.0f / 0.45f), 1e-3f);
}

// -------------------------------------------------------------------------------------------
// Transfer evaluation, usability and JSON
// -------------------------------------------------------------------------------------------

TEST(RasterColorDescriptionTest, TransferEvaluationMatchesReferenceCurves) {
  RasterTransfer srgb;
  srgb.kind_ = RasterTransferKind::SrgbPiecewise;
  EXPECT_NEAR(EvaluateRasterTransfer(srgb, 0.5f), 0.214041f, 1e-5f);
  EXPECT_NEAR(EvaluateRasterTransfer(srgb, 0.02f), 0.02f / 12.92f, 1e-7f);

  RasterTransfer pq;
  pq.kind_ = RasterTransferKind::St2084;
  // ST 2084: code value 0.5 is about 92.25 nits.
  EXPECT_NEAR(EvaluateRasterTransfer(pq, 0.5f) * 10000.0f, 92.25f, 0.05f);
  EXPECT_NEAR(EvaluateRasterTransfer(pq, 1.0f), 1.0f, 1e-6f);

  RasterTransfer hlg;
  hlg.kind_ = RasterTransferKind::Hlg;
  EXPECT_NEAR(EvaluateRasterTransfer(hlg, 0.5f), std::pow(0.25f / 3.0f, 1.2f), 1e-5f);
  EXPECT_NEAR(EvaluateRasterTransfer(hlg, 1.0f), 1.0f, 1e-4f);

  RasterTransfer bt1886;
  bt1886.kind_ = RasterTransferKind::Bt1886;
  EXPECT_NEAR(EvaluateRasterTransfer(bt1886, 0.5f), std::pow(0.5f, 2.4f), 1e-6f);
}

TEST(RasterColorDescriptionTest, DefectCheckRejectsWhiteOutsidePrimariesAndNonMonotonicCurve) {
  RasterColorDescription description;
  description.primaries_xy_ = color::GamutPrimariesXy(color::ColorGamutId::Rec709);
  EXPECT_FALSE(FindRasterColorDescriptionDefect(description).has_value());

  auto outside             = description;
  outside.primaries_xy_[6] = 0.70f;  // White x beyond the red primary.
  outside.primaries_xy_[7] = 0.10f;
  ASSERT_TRUE(FindRasterColorDescriptionDefect(outside).has_value());
  EXPECT_NE(FindRasterColorDescriptionDefect(outside)->find("white"), std::string::npos);

  auto non_monotonic               = description;
  non_monotonic.transfer_[1].kind_ = RasterTransferKind::IccSampled;
  non_monotonic.transfer_[1].sampled_.assign(kRasterSampledTransferEntries, 0.5f);
  non_monotonic.transfer_[1].sampled_[100] = 0.9f;
  ASSERT_TRUE(FindRasterColorDescriptionDefect(non_monotonic).has_value());
  EXPECT_NE(FindRasterColorDescriptionDefect(non_monotonic)->find("monotonic"), std::string::npos);
}

TEST(RasterColorDescriptionTest, DescriptionJsonRoundTripsAndOmitsAbsentFields) {
  RasterColorDescription defaulted;
  defaulted.primaries_xy_   = color::GamutPrimariesXy(color::ColorGamutId::Rec709);
  defaulted.origin_         = RasterColorOrigin::DefaultSrgb;
  defaulted.default_reason_ = "no color description in the file";
  const auto default_json   = RasterColorDescriptionToJson(defaulted);
  EXPECT_EQ(default_json.at("referral"), "display_referred");
  EXPECT_EQ(default_json.at("origin"), "default_srgb");
  ASSERT_EQ(default_json.at("transfer").size(), 1u);
  EXPECT_EQ(default_json.at("transfer")[0].at("kind"), "srgb_piecewise");
  EXPECT_FALSE(default_json.contains("icc_sha256"));
  EXPECT_FALSE(default_json.contains("profile_description"));
  EXPECT_EQ(default_json.at("default_reason"), "no color description in the file");
  EXPECT_EQ(RasterColorDescriptionFromJson(default_json), defaulted);

  RasterColorDescription icc;
  icc.primaries_xy_                     = color::GamutPrimariesXy(color::ColorGamutId::P3D65);
  icc.origin_                           = RasterColorOrigin::IccMatrixShaper;
  icc.profile_description_              = "Display P3";
  icc.icc_sha256_                       = std::string(64, 'a');
  icc.transfer_[0].kind_                = RasterTransferKind::Gamma;
  icc.transfer_[0].gamma_               = 2.4f;
  icc.transfer_[1].kind_                = RasterTransferKind::IccParametric;
  icc.transfer_[1].icc_parametric_type_ = 3;
  icc.transfer_[1].icc_params_          = {2.4f, 0.9f, 0.1f, 0.07f, 0.04f, 0.0f, 0.0f};
  icc.transfer_[2].kind_                = RasterTransferKind::IccSampled;
  icc.transfer_[2].sampled_.resize(kRasterSampledTransferEntries);
  for (std::size_t i = 0; i < kRasterSampledTransferEntries; ++i) {
    icc.transfer_[2].sampled_[i] =
        static_cast<float>(i) / static_cast<float>(kRasterSampledTransferEntries - 1);
  }
  const auto icc_json = RasterColorDescriptionToJson(icc);
  ASSERT_EQ(icc_json.at("transfer").size(), 3u);
  EXPECT_EQ(icc_json.at("transfer")[2].at("samples").size(), kRasterSampledTransferEntries);
  EXPECT_FALSE(icc_json.contains("default_reason"));
  EXPECT_EQ(icc_json.at("icc_sha256"), std::string(64, 'a'));
  EXPECT_EQ(RasterColorDescriptionFromJson(icc_json), icc);
  // The serialized form is stable, so a document that stores it keeps its hash.
  EXPECT_EQ(RasterColorDescriptionToJson(RasterColorDescriptionFromJson(icc_json)).dump(),
            icc_json.dump());

  auto bad_kind                   = default_json;
  bad_kind["transfer"][0]["kind"] = "log_c";
  EXPECT_THROW((void)RasterColorDescriptionFromJson(bad_kind), std::invalid_argument);
  auto missing_primaries = default_json;
  missing_primaries.erase("primaries_xy");
  EXPECT_THROW((void)RasterColorDescriptionFromJson(missing_primaries), std::invalid_argument);
}

TEST(Sha256Test, DigestMatchesFips180TestVectors) {
  EXPECT_EQ(ComputeSha256Hex({}),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  const std::string abc = "abc";
  EXPECT_EQ(ComputeSha256Hex(std::as_bytes(std::span(abc.data(), abc.size()))),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  EXPECT_EQ(ComputeSha256Hex(std::as_bytes(std::span(two_blocks.data(), two_blocks.size()))),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

}  // namespace
}  // namespace alcedo
