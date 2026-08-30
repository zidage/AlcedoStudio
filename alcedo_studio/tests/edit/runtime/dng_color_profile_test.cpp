// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <limits>

#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/dng_profile_gpu_math.h"
#include "image/dng_color_profile_import.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"

namespace alcedo {
namespace {
auto Table(unsigned h, unsigned s, unsigned v) -> std::vector<float> {
  std::vector<float> data(32 + h * s * v * 3, 0);
  data[2] = 32;
  data[3] = static_cast<float>(h);
  data[4] = static_cast<float>(s);
  data[5] = static_cast<float>(v);
  for (unsigned i = 32; i < data.size(); i += 3) {
    data[i + 1] = 1;
    data[i + 2] = 1;
  }
  return data;
}

TEST(DngColorProfile, HueInterpolationWrapsLastDivisionToRed) {
  auto data                = Table(6, 2, 1);
  data[32 + 5 * 2 * 3]     = 60;
  data[32 + 5 * 2 * 3 + 3] = 60;
  const auto result        = DngApplyHueSatMap(DngMakeRgb(1, 0, .5f), data.data(), 2);
  EXPECT_NEAR(result.r, 1, 1e-6);
  EXPECT_NEAR(result.g, 0, 1e-6);
  EXPECT_NEAR(result.b, 0, 1e-6);
}

TEST(DngColorProfile, TrilinearInterpolationUsesValueHueSaturationOrder) {
  auto data = Table(2, 2, 2);
  for (unsigned v = 0; v < 2; ++v)
    for (unsigned h = 0; h < 2; ++h)
      for (unsigned s = 0; s < 2; ++s) {
        data[32 + ((v * 2 + h) * 2 + s) * 3 + 1] = 1.0f + .2f * h + .4f * s + .2f * v;
      }
  // HSV=(90 degrees, .5, .5), halfway along each table axis, saturation scale=1.4.
  const auto result = DngApplyHueSatMap(DngMakeRgb(.375f, .5f, .25f), data.data(), 2);
  EXPECT_NEAR(result.r, .325, 1e-6);
  EXPECT_NEAR(result.g, .5, 1e-6);
  EXPECT_NEAR(result.b, .15, 1e-6);
}

TEST(DngColorProfile, SrgbValueScaleOperatesInEncodedValueSpace) {
  auto data = Table(1, 2, 2);
  data[6]   = 1;
  for (unsigned i = 32; i < data.size(); i += 3) data[i + 2] = .5f;
  const auto   result = DngApplyHueSatMap(DngMakeRgb(.5f, .25f, .25f), data.data(), 2);
  // Independent analytic sRGB EOTF of half the encoded value of linear 0.5.
  const double expected =
      std::pow(((1.055 * std::pow(.5, 1 / 2.4) - .055) * .5 + .055) / 1.055, 2.4);
  EXPECT_NEAR(result.r, expected, 2e-6);
  EXPECT_NEAR(result.g, expected * .5, 2e-6);
}

TEST(DngColorProfile, TableBoundaryRetainsPositiveSceneHeadroomAndNeutralAxis) {
  auto data = Table(2, 2, 2);
  for (unsigned i = 32; i < data.size(); i += 3) data[i + 2] = 1.5f;
  const auto bright = DngApplyHueSatMap(DngMakeRgb(2, 1, .5f), data.data(), 2);
  EXPECT_NEAR(bright.r, 3, 1e-6);
  EXPECT_NEAR(bright.g, 1.5, 1e-6);
  EXPECT_NEAR(bright.b, .75, 1e-6);
  const auto gray = DngApplyHueSatMap(DngMakeRgb(.2f, .2f, .2f), data.data(), 2);
  EXPECT_NEAR(gray.r, .3, 1e-6);
  EXPECT_NEAR(gray.g, .3, 1e-6);
  EXPECT_NEAR(gray.b, .3, 1e-6);
}

TEST(DngColorProfile, MalformedTablesAndUnsupportedEncodingsAreRejected) {
  DngColorProfile profile;
  profile.hue_sat_map_1.divisions = {90, 30, 1};
  EXPECT_THROW(MakeDngColorProfile(profile), std::runtime_error);
  profile.hue_sat_map_1.divisions = {1, 2, 1};
  profile.hue_sat_map_1.entries   = {0, 1, 1, 0, 1, 1};
  profile.hue_sat_map_1.encoding  = 2;
  EXPECT_THROW(MakeDngColorProfile(profile), std::runtime_error);
  profile.hue_sat_map_1.encoding   = 0;
  profile.hue_sat_map_1.entries[1] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW(MakeDngColorProfile(profile), std::runtime_error);
  profile.hue_sat_map_1.entries[1] = 1;
  profile.hue_sat_map_1.entries[2] = .5f;
  EXPECT_THROW(MakeDngColorProfile(profile), std::runtime_error);
}

TEST(DngColorProfile, SerializationPreservesTablesAndRecomputesContentFingerprint) {
  DngColorProfile profile;
  profile.baseline_exposure       = .28;
  profile.hue_sat_map_1.divisions = {1, 2, 1};
  profile.hue_sat_map_1.entries   = {0, 1, 1, 30, 1.5f, 1.1f};
  const auto original             = MakeDngColorProfile(profile);
  const auto restored             = DngColorProfileFromJson(DngColorProfileToJson(original));
  EXPECT_TRUE(DngColorProfilesEqual(original, restored));
  profile.hue_sat_map_1.entries[4] = 1.6f;
  EXPECT_NE(original->fingerprint, MakeDngColorProfile(profile)->fingerprint);
  EXPECT_THROW(DngColorProfileFromJson({{"version", 2}}), std::runtime_error);
}

TEST(DngColorProfile, ImportExpandsOmittedNeutralRowsAndTwoDimensionalTables) {
  Exiv2::ExifData exif;
  exif["Exif.Image.ProfileHueSatMapDims"]  = "2 2";
  exif["Exif.Image.ProfileHueSatMapData1"] = "10 1.2 1.1 -20 1.4 1.3";
  const auto profile                       = ReadDngColorProfile(exif);
  EXPECT_EQ(profile->hue_sat_map_1.divisions, (std::array<std::uint32_t, 3>{2, 2, 1}));
  EXPECT_EQ(profile->hue_sat_map_1.entries,
            (std::vector<float>{10, 1.2f, 1, 10, 1.2f, 1.1f, -20, 1.4f, 1, -20, 1.4f, 1.3f}));
  exif["Exif.Image.ProfileHueSatMapData1"] = "10 1.2 1.1";
  EXPECT_THROW(ReadDngColorProfile(exif), std::runtime_error);
}

TEST(DngColorProfile, DualIlluminantTablesInterpolateInReciprocalTemperatureAndClampEndpoints) {
  DngColorProfile dng;
  dng.hue_sat_map_1.divisions = dng.hue_sat_map_2.divisions = {1, 2, 1};
  dng.hue_sat_map_1.entries                                 = {0, 1, 1, 10, 1.2f, 1.1f};
  dng.hue_sat_map_2.entries                                 = {0, 1, 1, 30, 1.6f, 1.3f};
  DevelopCameraProfile profile;
  profile.dng_profile                   = MakeDngColorProfile(dng);
  profile.calibration_illuminants_valid = true;
  profile.color_matrix_1_cct            = 3000;
  profile.color_matrix_2_cct            = 6000;
  DevelopColorTransform transform;
  transform.xyz_d50_to_ap1 = {1, 0, 0, 0, 1, 0, 0, 0, 1};
  for (const auto [cct, hue] : {std::pair{2000.f, 10.f}, {4000.f, 20.f}, {8000.f, 30.f}}) {
    transform.resolved_cct = cct;
    const auto packed      = PackDngProfileGpuData(profile, transform);
    EXPECT_NEAR(packed[static_cast<unsigned>(packed[2]) + 3], hue, 1e-5);
  }
}

TEST(DngColorProfile, MissingProfileLeavesNonDngPixelsExactlyUnchanged) {
  DevelopColorTransform transform;
  const auto            data   = PackDngProfileGpuData(DevelopCameraProfile{}, transform);
  const auto            actual = DngApplyColorProfile(DngMakeRgb(-.25f, .5f, 4), data.data());
  EXPECT_FLOAT_EQ(actual.r, -.25f);
  EXPECT_FLOAT_EQ(actual.g, .5f);
  EXPECT_FLOAT_EQ(actual.b, 4);
}

TEST(DngColorProfile, LegacyProjectMetadataReloadsCompleteProfileWithoutMutatingSharedImage) {
  const auto path = std::filesystem::path(TEST_IMG_PATH) /
                    "raw/camera/sony/a7cii/ycbcr_compressed/DSC04739_dng.dng";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "Private Sony fixture missing";
  Image source(1, path, ImageType::DNG);
  MetadataExtractor::ExtractEXIF_ToImage(path, source);
  auto  encoded = nlohmann::json::parse(source.ExifToJson());
  auto& legacy  = encoded["RawRuntimeColorContext"];
  legacy.erase("DngColorProfile");
  // Older projects stored AnalogBalance and CameraCalibration baked into ColorMatrix.
  legacy["ColorMatrix1"][0] = 2.12;
  Image restored(2, path, ImageType::DNG);
  restored.JsonToExif(encoded.dump());
  const auto resolved = MetadataExtractor::ReadRawColorContextForRender(restored);
  EXPECT_TRUE(
      DngColorProfilesEqual(source.GetRawColorContext().dng_profile_, resolved.dng_profile_));
  EXPECT_NEAR(resolved.color_matrix_1_[0], .8784, 1e-6);
  EXPECT_FALSE(restored.GetRawColorContext().dng_profile_);
  EXPECT_DOUBLE_EQ(restored.GetRawColorContext().color_matrix_1_[0], 2.12);
  restored.image_path_ = "missing-dng-profile.dng";
  EXPECT_THROW(MetadataExtractor::ReadRawColorContextForRender(restored), std::exception);
  source.image_path_ = restored.image_path_;
  EXPECT_NO_THROW(MetadataExtractor::ReadRawColorContextForRender(source));
}

TEST(DngColorProfile, FullCalibrationMapsTaggedNeutralToD50WithOffDiagonalCameraCalibration) {
  DevelopPayload payload;
  auto&          p         = payload.camera_profile;
  p.color_matrices_valid   = true;
  p.forward_matrices_valid = true;
  p.as_shot_neutral_valid  = true;
  p.color_matrix_1 = p.color_matrix_2 = kDngIdentityMatrix;
  const double x = .34567 / .35850, z = (1 - .34567 - .35850) / .35850;
  p.forward_matrix_1 = p.forward_matrix_2 = {x, 0, 0, 0, 1, 0, 0, 0, z};
  DngColorProfile dng;
  dng.analog_balance       = {2, 1, 1.5};
  dng.camera_calibration_1 = dng.camera_calibration_2 = {1, .1, 0, 0, 1, 0, 0, .05, 1};
  const double maximum                                = 2 * (x + .1);
  p.as_shot_neutral                                   = {1, 1 / maximum, 1.5 * (z + .05) / maximum};
  p.dng_profile                                       = MakeDngColorProfile(dng);
  const auto transform                                = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(transform.ok);
  for (int r = 0; r < 3; ++r) {
    double out = 0;
    for (int c = 0; c < 3; ++c)
      out += transform.transform.camera_to_xyz_d50[r * 3 + c] * p.as_shot_neutral[c];
    EXPECT_NEAR(out, r == 0 ? x : (r == 1 ? 1 : z), 2e-5);
  }
}

TEST(DngColorProfile, CanonR6iiiImportsFullAdobeTablesAndChangesColorBeyondAMatrix) {
  const auto path =
      std::filesystem::path(TEST_IMG_PATH) / "raw/camera/canon/r6iii/9327411796_dng.dng";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "Private Canon fixture missing";
  Image image(1, path, ImageType::DNG);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  const auto& ctx = image.GetRawColorContext();
  ASSERT_TRUE(ctx.dng_profile_);
  const auto& dng = *ctx.dng_profile_;
  EXPECT_EQ(dng.name, "Adobe Standard");
  EXPECT_EQ(dng.hue_sat_map_1.entries.size(), 8100);
  EXPECT_EQ(dng.hue_sat_map_2.entries.size(), 8100);
  EXPECT_EQ(dng.look_table.entries.size(), 13824);
  EXPECT_NEAR(dng.baseline_exposure, .28, 1e-6);
  EXPECT_EQ(dng.analog_balance, (std::array<double, 3>{1, 1, 1}));
  DevelopPayload payload;
  BindDevelopCameraProfile(payload, ctx);
  const auto transform = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(transform.ok);
  const auto data = PackDngProfileGpuData(payload.camera_profile, transform.transform);
  EXPECT_NEAR(data[1], std::exp2(.28), 1e-6);
  // Test a warm camera-space patch; compare the matrix output with actual profile evaluation.
  float       c[3]   = {};
  const float raw[3] = {.12f, .18f, .08f};
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) c[r] += transform.transform.camera_to_ap1[r * 3 + k] * raw[k];
  const auto corrected = DngApplyColorProfile(DngMakeRgb(c[0], c[1], c[2]), data.data());
  EXPECT_TRUE(std::isfinite(corrected.r) && std::isfinite(corrected.g) &&
              std::isfinite(corrected.b));
  EXPECT_GT(
      std::abs(corrected.r - c[0]) + std::abs(corrected.g - c[1]) + std::abs(corrected.b - c[2]),
      .01f);
  const auto restored = DngColorProfileFromJson(DngColorProfileToJson(ctx.dng_profile_));
  EXPECT_TRUE(DngColorProfilesEqual(ctx.dng_profile_, restored));
}
}  // namespace
}  // namespace alcedo
