// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/dng_profile_gpu_math.h"
#include "image/dng_color_profile.hpp"
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

TEST(DngColorProfile, FingerprintIdentifiesContentAndIsPersistedAsSixteenHexDigits) {
  DngColorProfile profile;
  profile.baseline_exposure       = .28;
  profile.hue_sat_map_1.divisions = {1, 2, 1};
  profile.hue_sat_map_1.entries   = {0, 1, 1, 30, 1.5f, 1.1f};
  const auto original             = MakeDngColorProfile(profile);
  EXPECT_EQ(MakeDngColorProfile(profile)->fingerprint, original->fingerprint);
  EXPECT_TRUE(DngColorProfilesEqual(original, MakeDngColorProfile(profile)));
  profile.hue_sat_map_1.entries[4] = 1.6f;
  EXPECT_NE(original->fingerprint, MakeDngColorProfile(profile)->fingerprint);

  const auto text = DngColorProfileFingerprintToText(original->fingerprint);
  EXPECT_EQ(text.size(), 16u);
  EXPECT_EQ(DngColorProfileFingerprintFromText(text), original->fingerprint);
  EXPECT_EQ(DngColorProfileFingerprintToText(0x0123456789abcdefULL), "0123456789abcdef");
  EXPECT_THROW(DngColorProfileFingerprintFromText("0123"), std::runtime_error);
  EXPECT_THROW(DngColorProfileFingerprintFromText("0123456789ABCDEF"), std::runtime_error);

  // The persisted reference is the fingerprint text; reading it gives an unbound reference.
  const DngColorProfileRef bound(original);
  const auto               stored = DngColorProfileRefToJson(bound);
  EXPECT_EQ(stored, nlohmann::json(text));
  const auto restored = DngColorProfileRefFromJson(stored);
  EXPECT_TRUE(restored.IsReferenced());
  EXPECT_FALSE(restored.IsBound());
  EXPECT_EQ(restored, bound);
  EXPECT_THROW((void)restored.RequireBound(), std::runtime_error);
  EXPECT_EQ(bound.RequireBound(), original.get());
  EXPECT_TRUE(DngColorProfileRefToJson(DngColorProfileRef{}).is_null());
  EXPECT_FALSE(DngColorProfileRefFromJson(nullptr).IsReferenced());
  EXPECT_EQ(DngColorProfileRef{}.RequireBound(), nullptr);
}

TEST(DngColorProfile, UnboundReferenceFailsColorTransformAndGpuPackingInsteadOfDroppingProfile) {
  DngColorProfile dng;
  dng.hue_sat_map_1.divisions = {1, 2, 1};
  dng.hue_sat_map_1.entries   = {0, 1, 1, 10, 1.2f, 1.1f};
  const auto profile          = MakeDngColorProfile(dng);

  DevelopPayload payload;
  auto&          p        = payload.camera_profile;
  p.color_matrices_valid  = true;
  p.as_shot_neutral_valid = true;
  p.color_matrix_1 = p.color_matrix_2 = kDngIdentityMatrix;
  p.as_shot_neutral                   = {1, 1, 1};
  p.dng_profile                       = profile;
  ASSERT_TRUE(ResolveDevelopColorTransform(payload).ok);

  p.dng_profile         = DngColorProfileRef::Unbound(profile->fingerprint);
  const auto unresolved = ResolveDevelopColorTransform(payload);
  EXPECT_FALSE(unresolved.ok);
  EXPECT_EQ(unresolved.error, ColorTransformError::UnboundDngProfile);
  EXPECT_THROW((void)PackDngProfileGpuData(p, DevelopColorTransform{}), std::runtime_error);

  RawRuntimeColorContext imported;
  imported.color_matrices_valid_ = true;
  imported.dng_profile_          = DngColorProfileRef::Unbound(profile->fingerprint);
  DevelopPayload bind_target;
  EXPECT_THROW(BindDevelopCameraProfile(bind_target, imported), std::runtime_error);
  EXPECT_FALSE(bind_target.camera_profile.dng_profile.IsReferenced());
}

TEST(DngColorProfile, DevelopJsonStoresFingerprintAndLoadKeepsOnlyAMatchingBoundProfile) {
  DngColorProfile dng;
  dng.name                    = "Fixture";
  dng.hue_sat_map_1.divisions = {1, 2, 1};
  dng.hue_sat_map_1.entries   = {0, 1, 1, 10, 1.2f, 1.1f};
  const auto profile          = MakeDngColorProfile(dng);
  dng.name                    = "Other";
  const auto other            = MakeDngColorProfile(dng);

  auto document                      = CreateDefaultPipelineDocument();
  auto payload                       = document.Develop()->Params().Params();
  payload.camera_profile.dng_profile = profile;
  document.Develop()->Params().ReplaceParams(payload);

  const auto json = document.ToJson();
  const auto text = json.dump();
  EXPECT_EQ(text.find("hue_sat_map"), std::string::npos);
  EXPECT_EQ(text.find("\"dng_profile\""), std::string::npos);
  const auto develop_json = document.Develop()->Params().ToJson();
  EXPECT_EQ(develop_json.at("camera_profile").at("dng_profile_fingerprint"),
            nlohmann::json(DngColorProfileFingerprintToText(profile->fingerprint)));

  // A document read from JSON holds an unbound reference to the same fingerprint.
  const auto restored  = PipelineDocument::FromJson(json);
  const auto reference = restored.Develop()->Params().DngProfile();
  EXPECT_FALSE(reference.IsBound());
  EXPECT_EQ(reference, DngColorProfileRef(profile));
  EXPECT_EQ(restored.Develop()->Params().Params(), document.Develop()->Params().Params());

  // LoadJson of the same fingerprint keeps the bound profile; another fingerprint unbinds it.
  document.Develop()->Params().LoadJson(develop_json);
  EXPECT_EQ(document.Develop()->Params().DngProfile().Profile(), profile);
  auto changed = develop_json;
  changed["camera_profile"]["dng_profile_fingerprint"] =
      DngColorProfileFingerprintToText(other->fingerprint);
  document.Develop()->Params().LoadJson(changed);
  EXPECT_FALSE(document.Develop()->Params().DngProfile().IsBound());
  EXPECT_EQ(document.Develop()->Params().DngProfile(), DngColorProfileRef(other));

  // Binding makes the reference bound; a profile with a new fingerprint replaces the stored one.
  document.Develop()->Params().BindDngColorProfile(other);
  EXPECT_EQ(document.Develop()->Params().DngProfile().Profile(), other);
  document.Develop()->Params().BindDngColorProfile(profile);
  EXPECT_EQ(document.Develop()->Params().DngProfile(), DngColorProfileRef(profile));
  EXPECT_THROW(document.Develop()->Params().BindDngColorProfile(nullptr), std::invalid_argument);
}

TEST(DngColorProfile, ClonedDocumentSharesTheSourceBoundProfile) {
  DngColorProfile dng;
  dng.hue_sat_map_1.divisions = {1, 2, 1};
  dng.hue_sat_map_1.entries   = {0, 1, 1, 10, 1.2f, 1.1f};
  const auto profile          = MakeDngColorProfile(dng);
  auto       document         = CreateDefaultPipelineDocument();
  document.Develop()->Params().BindDngColorProfile(profile);

  const auto clone = ClonePipelineDocument(document);
  EXPECT_EQ(clone.Develop()->Params().DngProfile().Profile(), profile);
  const auto unbound_clone = ClonePipelineDocument(PipelineDocument::FromJson(document.ToJson()));
  EXPECT_FALSE(unbound_clone.Develop()->Params().DngProfile().IsBound());
  EXPECT_TRUE(unbound_clone.Develop()->Params().DngProfile().IsReferenced());
}

TEST(DngColorProfile, ImportedContextBindsProfileOnDocumentReadFromJson) {
  DngColorProfile dng;
  dng.hue_sat_map_1.divisions = {1, 2, 1};
  dng.hue_sat_map_1.entries   = {0, 1, 1, 10, 1.2f, 1.1f};
  RawRuntimeColorContext imported;
  imported.color_matrices_valid_  = true;
  imported.as_shot_neutral_valid_ = true;
  for (int i = 0; i < 3; ++i) {
    imported.color_matrix_1_[i * 4] = imported.color_matrix_2_[i * 4] = 1.0;
    imported.as_shot_neutral_[i]                                     = 1.0;
  }
  imported.dng_profile_ = MakeDngColorProfile(dng);

  auto document = CreateDefaultPipelineDocument();
  BindImportedCameraProfile(document, imported);
  // The checkpoint and replay paths bind the same context onto a document read from JSON.
  // Its payload equals the bound payload (same fingerprint), yet the profile must be installed.
  auto reloaded = PipelineDocument::FromJson(document.ToJson());
  ASSERT_FALSE(reloaded.Develop()->Params().DngProfile().IsBound());
  BindImportedCameraProfile(reloaded, imported);
  EXPECT_EQ(reloaded.Develop()->Params().DngProfile().Profile(), imported.dng_profile_.Profile());
  EXPECT_TRUE(ResolveDevelopColorTransform(reloaded.Develop()->Params().Params()).ok);

  auto replaced = PipelineDocument::FromJson(document.ToJson());
  replaced.Develop()->Params().ReplaceParams(document.Develop()->Params().Params());
  EXPECT_EQ(replaced.Develop()->Params().DngProfile().Profile(), imported.dng_profile_.Profile());
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

TEST(DngColorProfile, ReferenceGamutCompressionPreservesNeutralAndInGamutAp1Exactly) {
  for (const auto input : {AcesRgcMakeRgb(0.18f, 0.18f, 0.18f),
                           AcesRgcMakeRgb(1.0f, 0.25f, 0.15f)}) {
    const auto output = AcesReferenceGamutCompress(input.r, input.g, input.b);
    EXPECT_FLOAT_EQ(output.r, input.r);
    EXPECT_FLOAT_EQ(output.g, input.g);
    EXPECT_FLOAT_EQ(output.b, input.b);
  }
}

TEST(DngColorProfile, ReferenceGamutCompressionReducesExtremeAp1DistanceBeforeLogEncoding) {
  const auto input  = AcesRgcMakeRgb(1.0f, -1.0f, 0.0f);
  const auto output = AcesReferenceGamutCompress(input.r, input.g, input.b);
  EXPECT_FLOAT_EQ(output.r, input.r);
  EXPECT_GT(output.g, input.g);
  EXPECT_GT(output.b, input.b);
  EXPECT_TRUE(std::isfinite(output.r));
  EXPECT_TRUE(std::isfinite(output.g));
  EXPECT_TRUE(std::isfinite(output.b));
}

TEST(DngColorProfile, ProjectMetadataStoresFingerprintOnlyAndSourceReadGivesSameProfile) {
  const auto path = std::filesystem::path(TEST_IMG_PATH) /
                    "raw/camera/sony/a7cii/ycbcr_compressed/DSC04739_dng.dng";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "Private Sony fixture missing";
  Image source(1, path, ImageType::DNG);
  MetadataExtractor::ExtractEXIF_ToImage(path, source);
  const auto& imported = source.GetRawColorContext().dng_profile_;
  ASSERT_TRUE(imported.IsBound());

  const auto persisted = source.ExifToJson();
  EXPECT_EQ(persisted.find("HueSatMap"), std::string::npos);
  EXPECT_EQ(persisted.find("hue_sat_map"), std::string::npos);
  EXPECT_EQ(persisted.find("look_table"), std::string::npos);
  const auto encoded = nlohmann::json::parse(persisted);
  EXPECT_EQ(encoded.at("RawRuntimeColorContext").at("DngProfileFingerprint"),
            nlohmann::json(DngColorProfileFingerprintToText(imported->fingerprint)));

  Image restored(2, path, ImageType::DNG);
  restored.JsonToExif(persisted);
  const auto& reference = restored.GetRawColorContext().dng_profile_;
  EXPECT_FALSE(reference.IsBound());
  EXPECT_EQ(reference, imported);

  const auto from_source = MetadataExtractor::ReadDngColorProfileFromSource(path);
  ASSERT_NE(from_source, nullptr);
  EXPECT_EQ(from_source->fingerprint, imported->fingerprint);
  EXPECT_THROW((void)MetadataExtractor::ReadDngColorProfileFromSource("missing-dng-profile.dng"),
               std::exception);
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
  const auto from_source = MetadataExtractor::ReadDngColorProfileFromSource(path);
  ASSERT_NE(from_source, nullptr);
  EXPECT_EQ(from_source->fingerprint, ctx.dng_profile_->fingerprint);
  EXPECT_EQ(from_source->look_table, dng.look_table);
}
}  // namespace
}  // namespace alcedo
