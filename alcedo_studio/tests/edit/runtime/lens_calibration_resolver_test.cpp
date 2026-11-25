//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/lens/lens_calibration_resolver.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/raw_color_context.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/lens/lens_calib_runtime.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

auto LensDatabasePath() -> std::string {
  return (std::filesystem::path(CONFIG_PATH) / "lens_calib").string();
}

auto TouitPayload(bool enabled) -> DevelopPayload {
  DevelopPayload payload;
  payload.lens_enabled         = enabled;
  payload.apply_vignetting     = true;
  payload.apply_distortion     = true;
  payload.apply_tca            = false;
  payload.apply_crop           = false;
  payload.projection_enabled   = false;
  payload.lens_maker           = "Zeiss";
  payload.lens_model           = "Touit 1.8/32";
  payload.lens_profile_db_path = LensDatabasePath();
  return payload;
}

auto FocalOnlyContext() -> RawRuntimeColorContext {
  RawRuntimeColorContext context;
  context.valid_               = true;
  context.lens_metadata_valid_ = false;
  context.focal_length_mm_     = 32.0f;
  context.aperture_f_number_   = 1.8f;
  context.focus_distance_m_    = 10.0f;
  context.crop_factor_hint_    = 1.534f;
  return context;
}

auto ReadBytes(const std::filesystem::path& path) -> std::vector<std::byte> {
  std::ifstream           file(path, std::ios::binary);
  const std::vector<char> chars((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

auto ReadJson(const std::filesystem::path& path) -> nlohmann::json {
  std::ifstream file(path);
  EXPECT_TRUE(file.is_open()) << path.string();
  return nlohmann::json::parse(file);
}

/**
 * @brief One stored lens fixture.
 *
 * The expected file was written by LensCalibOp::ResolveRuntimeForImage at fcfbb2ef, before G10.6
 * moved the resolution into LensCalibrationResolver. It names the CI RAW file and the lens EXIF
 * name that the resolver received. The unmatched fixture replaces a real lens EXIF name with one
 * that Lensfun does not know, because every CI RAW lens has a profile.
 *
 * The user catalog file is the exception. LensCalibOp ranked a lens whose focal and aperture
 * ranges fit the shot above the selected lens name, so the file from fcfbb2ef held the FE 28mm f/2
 * profile of the RAW lens instead of the selected FE 20-70mm f/4 G. The file now holds the
 * resolver output for the FE 20-70mm f/4 G profile at the RAW's 28 mm focal length.
 */
struct LensFixture {
  const char* expected_file;
  const char* catalog_lens_maker;
  const char* catalog_lens_model;
};

// Float fields compare within 4 ULP: the Lensfun auto-scale search rounds differently on arm64 and
// x64 (resolved_scale 1.03895819 and 1.03895831 for the DNG warp fixture).
void ExpectFloatField(const nlohmann::json& expected, const char* name, float actual,
                      const std::string& context) {
  ASSERT_TRUE(expected.contains(name)) << context << " missing " << name;
  EXPECT_FLOAT_EQ(expected.at(name).get<float>(), actual) << context << " field " << name;
}

void ExpectIntField(const nlohmann::json& expected, const char* name, std::int32_t actual,
                    const std::string& context) {
  ASSERT_TRUE(expected.contains(name)) << context << " missing " << name;
  EXPECT_EQ(expected.at(name).get<std::int32_t>(), actual) << context << " field " << name;
}

template <std::size_t N>
void ExpectFloatArray(const nlohmann::json& expected, const char* name, const float (&actual)[N],
                      const std::string& context) {
  ASSERT_TRUE(expected.contains(name)) << context << " missing " << name;
  const auto& values = expected.at(name);
  ASSERT_EQ(values.size(), N) << context << " field " << name;
  for (std::size_t i = 0; i < N; ++i) {
    EXPECT_FLOAT_EQ(values.at(i).get<float>(), actual[i])
        << context << " field " << name << "[" << i << "]";
  }
}

void ExpectParamsEqual(const nlohmann::json& expected, const LensCalibGpuParams& actual,
                       const std::string& context) {
  ExpectIntField(expected, "version", actual.version, context);
  ExpectIntField(expected, "src_width", actual.src_width, context);
  ExpectIntField(expected, "src_height", actual.src_height, context);
  ExpectIntField(expected, "dst_width", actual.dst_width, context);
  ExpectIntField(expected, "dst_height", actual.dst_height, context);
  ExpectFloatField(expected, "norm_scale", actual.norm_scale, context);
  ExpectFloatField(expected, "norm_unscale", actual.norm_unscale, context);
  ExpectFloatField(expected, "center_x", actual.center_x, context);
  ExpectFloatField(expected, "center_y", actual.center_y, context);
  ExpectFloatField(expected, "camera_crop_factor", actual.camera_crop_factor, context);
  ExpectFloatField(expected, "nominal_focal_mm", actual.nominal_focal_mm, context);
  ExpectFloatField(expected, "real_focal_mm", actual.real_focal_mm, context);
  ExpectFloatField(expected, "lens_center_x", actual.lens_center_x, context);
  ExpectFloatField(expected, "lens_center_y", actual.lens_center_y, context);
  ExpectIntField(expected, "source_projection", actual.source_projection, context);
  ExpectIntField(expected, "target_projection", actual.target_projection, context);
  ExpectIntField(expected, "distortion_model", actual.distortion_model, context);
  ExpectFloatArray(expected, "distortion_terms", actual.distortion_terms, context);
  ExpectIntField(expected, "tca_model", actual.tca_model, context);
  ExpectFloatArray(expected, "tca_terms", actual.tca_terms, context);
  ExpectIntField(expected, "vignetting_model", actual.vignetting_model, context);
  ExpectFloatArray(expected, "vignetting_terms", actual.vignetting_terms, context);
  ExpectIntField(expected, "crop_mode", actual.crop_mode, context);
  ExpectFloatArray(expected, "crop_bounds", actual.crop_bounds, context);
  ExpectIntField(expected, "interpolation", actual.interpolation, context);
  ExpectIntField(expected, "apply_vignetting", actual.apply_vignetting, context);
  ExpectIntField(expected, "apply_distortion", actual.apply_distortion, context);
  ExpectIntField(expected, "apply_tca", actual.apply_tca, context);
  ExpectIntField(expected, "apply_projection", actual.apply_projection, context);
  ExpectIntField(expected, "apply_crop", actual.apply_crop, context);
  ExpectIntField(expected, "apply_crop_circle", actual.apply_crop_circle, context);
  ExpectIntField(expected, "use_user_scale", actual.use_user_scale, context);
  ExpectIntField(expected, "use_auto_scale", actual.use_auto_scale, context);
  ExpectFloatField(expected, "user_scale", actual.user_scale, context);
  ExpectFloatField(expected, "resolved_scale", actual.resolved_scale, context);
  ExpectIntField(expected, "perspective_mode", actual.perspective_mode, context);
  ExpectFloatArray(expected, "perspective_terms", actual.perspective_terms, context);
  ExpectIntField(expected, "fast_path_distortion_only", actual.fast_path_distortion_only, context);
  ExpectIntField(expected, "fast_path_vignetting_only", actual.fast_path_vignetting_only, context);
  ExpectIntField(expected, "low_precision_preview", actual.low_precision_preview, context);
}

}  // namespace

TEST(LensCalibDevelopResolve, LensResolverMatchesStoredLensParametersForFixtureSet) {
  const LensFixture fixtures[] = {
      {"dng_embedded_warp_expected_lens_params.json", "", ""},
      {"lensfun_matched_raw_expected_lens_params.json", "", ""},
      {"lensfun_unmatched_raw_expected_lens_params.json", "", ""},
      {"user_catalog_lens_expected_lens_params.json", "Sony", "FE 20-70mm F4 G"},
  };
  const auto  expected_dir   = std::filesystem::path(ALCEDO_LENS_EXPECTED_PARAMETER_DIR);
  const auto  raw_root       = std::filesystem::path(ALCEDO_CI_RAW_FIXTURE_ROOT);
  std::size_t resolved_count = 0;
  for (const auto& fixture : fixtures) {
    const auto expected = ReadJson(expected_dir / fixture.expected_file);
    const auto raw_file = expected.at("raw_file").get<std::string>();
    auto prepared = RawInputLoader::LoadEncoded(ReadBytes(raw_root / raw_file), DecodeRes::EIGHTH);
    prepared.color_context.lens_make_  = expected.at("exif_lens_maker").get<std::string>();
    prepared.color_context.lens_model_ = expected.at("exif_lens_model").get<std::string>();
    const auto extent                  = prepared.CompileSource().develop_output_extent;
    const bool warp                    = prepared.dng_warp_rectilinear.has_value();
    const auto warp_corrections        = dng::CorrectionsOf(prepared.dng_warp_rectilinear);
    ASSERT_EQ(expected.at("develop_extent").at(0).get<std::uint32_t>(), extent.width)
        << fixture.expected_file;
    ASSERT_EQ(expected.at("develop_extent").at(1).get<std::uint32_t>(), extent.height)
        << fixture.expected_file;
    ASSERT_EQ(expected.at("dng_geometry_applied").get<bool>(), warp) << fixture.expected_file;

    DevelopPayload payload;
    payload.lens_enabled         = true;
    payload.lens_profile_db_path = LensDatabasePath();
    payload.lens_maker           = fixture.catalog_lens_maker;
    payload.lens_model           = fixture.catalog_lens_model;
    const auto runtime =
        LensCalibrationResolver::Resolve(payload, prepared.color_context, extent, warp_corrections);

    ASSERT_EQ(expected.at("resolved").get<bool>(), runtime.has_value()) << fixture.expected_file;
    if (runtime.has_value()) {
      ++resolved_count;
      ExpectParamsEqual(expected.at("params"), *runtime, fixture.expected_file);
    }
  }
  // The set covers a DNG warp, a matched RAW, and a catalog selection that resolve, and one
  // unmatched RAW that does not.
  EXPECT_EQ(resolved_count, 3U);
}

TEST(LensCalibDevelopResolve, UserCatalogIdentityResolvesWhenRawLensNameIsEmpty) {
  const auto runtime = LensCalibrationResolver::Resolve(TouitPayload(true), FocalOnlyContext(),
                                                        Extent2D{96, 64}, {});
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(runtime->apply_vignetting, 1);
  EXPECT_EQ(runtime->vignetting_model, static_cast<std::int32_t>(LensCalibVignettingModel::PA));
  EXPECT_EQ(runtime->src_width, 96);
  EXPECT_EQ(runtime->src_height, 64);
  EXPECT_EQ(runtime->dst_width, 96);
  EXPECT_EQ(runtime->dst_height, 64);
}

TEST(LensCalibDevelopResolve, DisabledPayloadReturnsNoRuntime) {
  const auto runtime = LensCalibrationResolver::Resolve(TouitPayload(false), FocalOnlyContext(),
                                                        Extent2D{96, 64}, {});
  EXPECT_FALSE(runtime.has_value());
}

TEST(LensCalibDevelopResolve, EmptyCatalogIdentityWithoutRawLensNameReturnsNoRuntime) {
  auto payload       = TouitPayload(true);
  payload.lens_maker = {};
  payload.lens_model = {};
  const auto runtime =
      LensCalibrationResolver::Resolve(payload, FocalOnlyContext(), Extent2D{96, 64}, {});
  EXPECT_FALSE(runtime.has_value());
}

TEST(LensCalibDevelopResolve, CatalogIdentityWithoutFocalLengthReturnsNoRuntime) {
  auto context                 = FocalOnlyContext();
  context.focal_length_mm_     = 0.0f;
  context.aperture_f_number_   = 0.0f;
  context.lens_metadata_valid_ = false;
  const auto runtime =
      LensCalibrationResolver::Resolve(TouitPayload(true), context, Extent2D{96, 64}, {});
  EXPECT_FALSE(runtime.has_value());
}

TEST(LensCalibDevelopResolve, EnabledPayloadWithEmptyExtentThrows) {
  EXPECT_THROW((void)LensCalibrationResolver::Resolve(TouitPayload(true), FocalOnlyContext(),
                                                      Extent2D{0, 0}, {}),
               std::runtime_error);
}

TEST(LensCalibDevelopResolve, ProfileStatusNamesMissingIdentityAndUnknownLens) {
  LensCorrectionSettings settings;
  settings.database_path_ = LensDatabasePath();

  LensInputMeta no_focal;
  no_focal.lens_model_ = "Touit 1.8/32";
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(no_focal, settings, {}).status_,
            LensProfileStatus::MissingLensIdentity);

  LensInputMeta unknown;
  unknown.lens_maker_      = "Nonexistent Optics";
  unknown.lens_model_      = "Imaginary 7.3/1234 Mk IX";
  unknown.focal_length_mm_ = 1234.0f;
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(unknown, settings, {}).status_,
            LensProfileStatus::LensNotInDatabase);
}

namespace {

/// Warp of three planes with kr0..kr3 per plane and no tangential terms.
auto ThreePlaneWarp(const std::array<std::array<double, 4>, 3>& radial) -> dng::WarpRectilinear {
  dng::WarpRectilinear warp;
  warp.coefficient_set_count = 3;
  for (std::size_t plane = 0; plane < radial.size(); ++plane) {
    for (std::size_t term = 0; term < radial[plane].size(); ++term) {
      warp.coefficient_sets[plane][term] = radial[plane][term];
    }
  }
  return warp;
}

/// OpcodeList3 of an Adobe Camera Raw Enhanced DNG of a Sony ILCE-7RM3 with the Tamron
/// 28-200mm A071: red and blue move, green is the identity (issue #1144).
auto CameraRawLateralCaWarp() -> dng::WarpRectilinear {
  return ThreePlaneWarp(
      {{{0.9998897514917477, 0.0005055739768886625, -0.0011587295601298564, 0.0010821022163653993},
        {1.0, 0.0, 0.0, 0.0},
        {1.0003718791627394, 0.0006991645819134362, -0.0009717152039527832,
         2.4391552233993252e-05}}});
}

/// A warp whose planes all correct the same barrel distortion.
auto DistortionWarp() -> dng::WarpRectilinear {
  return ThreePlaneWarp(
      {{{1.0, -0.02, 0.003, 0.0}, {1.0, -0.02, 0.003, 0.0}, {1.0, -0.02, 0.003, 0.0}}});
}

}  // namespace

TEST(LensCalibDevelopResolve, DngWarpCorrectionsNameWhatTheWarpMoves) {
  EXPECT_FALSE(dng::CorrectionsOf(std::optional<dng::WarpRectilinear>{}).distortion);

  const auto identity = dng::CorrectionsOf(
      ThreePlaneWarp({{{1.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}}}));
  EXPECT_FALSE(identity.distortion);
  EXPECT_FALSE(identity.lateral_ca);

  const auto lateral_ca = dng::CorrectionsOf(CameraRawLateralCaWarp());
  EXPECT_FALSE(lateral_ca.distortion);
  EXPECT_TRUE(lateral_ca.lateral_ca);

  const auto distortion = dng::CorrectionsOf(DistortionWarp());
  EXPECT_TRUE(distortion.distortion);
  EXPECT_FALSE(distortion.lateral_ca);

  // One coefficient set applies to every plane: it can correct only distortion.
  dng::WarpRectilinear single  = DistortionWarp();
  single.coefficient_set_count = 1;
  EXPECT_TRUE(dng::CorrectionsOf(single).distortion);
  EXPECT_FALSE(dng::CorrectionsOf(single).lateral_ca);

  // A tangential term alone moves the reference plane too.
  dng::WarpRectilinear tangential =
      ThreePlaneWarp({{{1.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}, {1.0, 0.0, 0.0, 0.0}}});
  for (auto& set : tangential.coefficient_sets) {
    set[4] = 0.001;
  }
  EXPECT_TRUE(dng::CorrectionsOf(tangential).distortion);
  EXPECT_FALSE(dng::CorrectionsOf(tangential).lateral_ca);
}

TEST(LensCalibDevelopResolve, DngWarpKeepsVignettingAndDisablesGeometryCorrections) {
  auto payload       = TouitPayload(true);
  payload.apply_tca  = true;
  payload.apply_crop = true;
  const auto without_warp =
      LensCalibrationResolver::Resolve(payload, FocalOnlyContext(), Extent2D{96, 64}, {});
  const auto with_warp = LensCalibrationResolver::Resolve(
      payload, FocalOnlyContext(), Extent2D{96, 64}, dng::CorrectionsOf(DistortionWarp()));
  ASSERT_TRUE(without_warp.has_value());
  ASSERT_TRUE(with_warp.has_value());
  EXPECT_EQ(without_warp->apply_distortion, 1);
  EXPECT_EQ(with_warp->apply_distortion, 0);
  EXPECT_EQ(with_warp->apply_tca, 0);
  EXPECT_EQ(with_warp->apply_projection, 0);
  EXPECT_EQ(with_warp->apply_crop, 0);
  EXPECT_EQ(with_warp->apply_vignetting, 1);
}

// Issue #1144: Camera Raw corrects the distortion of the lens with a lens profile outside the
// file and writes a warp that corrects only lateral chromatic aberration. Lensfun then corrects
// the distortion; only its TCA would correct the same error twice.
TEST(LensCalibDevelopResolve, DngWarpOfLateralCaOnlyKeepsLensfunDistortion) {
  auto payload       = TouitPayload(true);
  payload.apply_tca  = true;
  payload.apply_crop = true;
  const auto without_warp =
      LensCalibrationResolver::Resolve(payload, FocalOnlyContext(), Extent2D{96, 64}, {});
  const auto with_warp = LensCalibrationResolver::Resolve(
      payload, FocalOnlyContext(), Extent2D{96, 64}, dng::CorrectionsOf(CameraRawLateralCaWarp()));
  ASSERT_TRUE(without_warp.has_value());
  ASSERT_TRUE(with_warp.has_value());
  EXPECT_EQ(with_warp->apply_distortion, 1);
  EXPECT_EQ(with_warp->distortion_model, without_warp->distortion_model);
  EXPECT_EQ(with_warp->apply_crop, without_warp->apply_crop);
  EXPECT_EQ(with_warp->apply_vignetting, 1);
  EXPECT_EQ(with_warp->apply_tca, 0);
}

TEST(LensCalibDevelopResolve, MalformedDatabaseSkipsLensCorrectionWithoutError) {
  // Section 15.7: the pre-G10.6 LensCalibOp skipped lens correction when the Lensfun database gave
  // no profile; it did not report an error. The resolver keeps that behavior. Lensfun loads a
  // directory of malformed XML as an empty database, so the lens is not found.
  const auto broken_root =
      std::filesystem::temp_directory_path() / "alcedo_g10_6_malformed_lensfun_db";
  std::filesystem::remove_all(broken_root);
  std::filesystem::create_directories(broken_root);
  {
    std::ofstream broken(broken_root / "broken.xml");
    broken << "<lensdatabase version=\"2\"><lens><maker>Broken";
  }

  LensCorrectionSettings settings;
  settings.database_path_ = broken_root;
  LensInputMeta meta;
  meta.lens_maker_      = "Zeiss";
  meta.lens_model_      = "Touit 1.8/32";
  meta.focal_length_mm_ = 32.0f;
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(meta, settings, {}).status_,
            LensProfileStatus::LensNotInDatabase);

  auto payload                 = TouitPayload(true);
  payload.lens_profile_db_path = broken_root.string();
  std::optional<LensCalibGpuParams> runtime;
  EXPECT_NO_THROW(runtime = LensCalibrationResolver::Resolve(payload, FocalOnlyContext(),
                                                             Extent2D{96, 64}, {}));
  EXPECT_FALSE(runtime.has_value());

  // The database cache reloads the valid directory on the next call.
  EXPECT_TRUE(
      LensCalibrationResolver::Resolve(TouitPayload(true), FocalOnlyContext(), Extent2D{96, 64}, {})
          .has_value());
  std::filesystem::remove_all(broken_root);
}

namespace {

/// One lens as the RAW metadata names it, with the Lensfun profile it must match.
struct LensMatchCase {
  const char* camera_maker;
  const char* camera_model;
  const char* lens_maker;
  const char* lens_model;
  float       focal_length_mm;
  float       aperture_f_number;
  const char* expected_profile_maker;
  const char* expected_profile_model;
  bool        profile_has_vignetting;
};

auto MetaFor(const LensMatchCase& lens) -> LensInputMeta {
  LensInputMeta meta;
  meta.cam_maker_         = lens.camera_maker;
  meta.cam_model_         = lens.camera_model;
  meta.lens_maker_        = lens.lens_maker;
  meta.lens_model_        = lens.lens_model;
  meta.focal_length_mm_   = lens.focal_length_mm;
  meta.aperture_f_number_ = lens.aperture_f_number;
  meta.distance_m_        = 10.0f;
  return meta;
}

auto AllCorrectionsSettings() -> LensCorrectionSettings {
  LensCorrectionSettings settings;
  settings.database_path_    = LensDatabasePath();
  settings.apply_vignetting_ = true;
  settings.apply_distortion_ = true;
  settings.apply_tca_        = true;
  settings.apply_crop_       = false;
  return settings;
}

/// Resolve one case and check the matched profile and the corrections that its data supplies.
void ExpectProfileMatches(const LensMatchCase& lens) {
  const std::string context = std::string(lens.camera_model) + " + " + lens.lens_model;
  const auto        result =
      LensCalibrationResolver::ResolveProfile(MetaFor(lens), AllCorrectionsSettings(), {});
  ASSERT_EQ(result.status_, LensProfileStatus::Resolved) << context;
  EXPECT_EQ(result.matched_lens_maker_, lens.expected_profile_maker) << context;
  EXPECT_EQ(result.matched_lens_model_, lens.expected_profile_model) << context;
  // Every profile in the cases below has distortion and poly3 TCA data.
  EXPECT_EQ(result.params_.apply_distortion, 1) << context;
  EXPECT_NE(result.params_.distortion_model,
            static_cast<std::int32_t>(LensCalibDistortionModel::NONE))
      << context;
  EXPECT_EQ(result.params_.apply_tca, 1) << context;
  EXPECT_EQ(result.params_.tca_model, static_cast<std::int32_t>(LensCalibTCAModel::POLY3))
      << context;
  // Vignetting is requested for every case, and applies only where the profile has the data.
  EXPECT_EQ(result.params_.apply_vignetting, lens.profile_has_vignetting ? 1 : 0) << context;
  EXPECT_EQ(result.params_.vignetting_model,
            static_cast<std::int32_t>(lens.profile_has_vignetting ? LensCalibVignettingModel::PA
                                                                  : LensCalibVignettingModel::NONE))
      << context;
}

auto ReadNikonZLensName(const char* lens_id) -> std::string {
  const auto  id_map  = ReadJson(std::filesystem::path(CONFIG_PATH) / "nikon_lens" / "id_map.json");
  const auto& z_names = id_map.at("numeric_id_map");
  EXPECT_TRUE(z_names.contains(lens_id)) << "Nikon Z lens ID " << lens_id;
  return z_names.contains(lens_id) ? z_names.at(lens_id).get<std::string>() : std::string{};
}

}  // namespace

TEST(LensCalibDevelopResolve, ProfilesAddedInLensfun43f9e001MatchRawLensNames) {
  const LensMatchCase cases[] = {
      {"Sony", "ILCE-7M4", "", "FE 50mm F1.4 GM", 50.0f, 1.4f, "Sony", "FE 50mm f/1.4 GM", false},
      {"Sony", "ILCE-7M4", "", "FE 16mm F1.8 G", 16.0f, 1.8f, "Sony", "FE 16mm f/1.8 G", false},
      {"Canon", "Canon EOS R5", "", "RF50mm F1.4 L VCM", 50.0f, 1.4f, "Canon",
       "Canon RF 50mm F1.4 L VCM", false},
  };
  for (const auto& lens : cases) {
    ExpectProfileMatches(lens);
  }
}

TEST(LensCalibDevelopResolve, NikonZLensIdNamesMatchAddedLensfunProfiles) {
  // Nikon Z bodies store no lens name. The metadata extractor names the lens from the Z-mount
  // lens ID in config/nikon_lens/id_map.json, so these names must match the Lensfun profiles.
  const std::string   z_24_105   = ReadNikonZLensName("57");
  const std::string   z_dx_16_50 = ReadNikonZLensName("53");
  const std::string   z_24_70_f4 = ReadNikonZLensName("1");

  const LensMatchCase cases[]    = {
      {"Nikon Corporation", "Nikon Z 6_2", "Nikon", z_24_105.c_str(), 50.0f, 5.6f, "Nikon",
          "Nikkor Z 24-105mm f/4-7.1", false},
      {"Nikon Corporation", "Nikon Z 50", "Nikon", z_dx_16_50.c_str(), 24.0f, 2.8f, "Nikon",
          "Nikkor Z DX 16-50mm f/2.8 VR", true},
      // An ID that the map already had keeps matching its existing profile.
      {"Nikon Corporation", "Nikon Z 6_2", "Nikon", z_24_70_f4.c_str(), 35.0f, 4.0f, "Nikon",
          "Nikkor Z 24-70mm f/4 S", true},
  };
  for (const auto& lens : cases) {
    ExpectProfileMatches(lens);
  }
}

TEST(LensCalibDevelopResolve, SelectedLensNameOutranksLensWhoseRangesFitTheShot) {
  // The shot values fit the FE 28mm f/2 (28 mm, f/2) and not the f/4 zoom. The name decides: the
  // shot values only order Lensfun results with equal scores.
  const LensMatchCase cases[] = {
      {"Sony", "ILCE-7M4", "Sony", "FE 20-70mm F4 G", 28.0f, 2.0f, "Sony", "FE 20-70mm f/4 G",
       true},
      {"Sony", "ILCE-7M4", "Sony", "FE 28mm F2", 28.0f, 2.0f, "Sony", "FE 28mm f/2", true},
  };
  for (const auto& lens : cases) {
    ExpectProfileMatches(lens);
  }
}

TEST(LensCalibDevelopResolve, RevisedTamron17To70ProfileMatchesAllFourMounts) {
  // The old profile listed only Sony E. "E 17-70mm F2.8 B070" is the Sony E EXIF name and the
  // catalog name that existing projects saved before the profile was renamed.
  const LensMatchCase cases[] = {
      {"Sony", "ILCE-6400", "", "E 17-70mm F2.8 B070", 35.0f, 2.8f, "Tamron",
       "Tamron 17-70mm F2.8 B070 Z/RF/X/E", true},
      {"Fujifilm", "X-T5", "Tamron", "Tamron 17-70mm F2.8 B070 Z/RF/X/E", 35.0f, 2.8f, "Tamron",
       "Tamron 17-70mm F2.8 B070 Z/RF/X/E", true},
      {"Nikon Corporation", "Nikon Z 50", "Tamron", "Tamron 17-70mm F2.8 B070 Z/RF/X/E", 35.0f,
       2.8f, "Tamron", "Tamron 17-70mm F2.8 B070 Z/RF/X/E", true},
  };
  for (const auto& lens : cases) {
    ExpectProfileMatches(lens);
  }
}

TEST(LensCalibDevelopResolve, RevisedTamron17To70ProfileUsesUpdatedDistortionCoefficients) {
  // mil-tamron.xml at Lensfun 43f9e001: PTLens a, b, c at the 17 mm sample, calibrated at crop
  // factor 1.534 with the default 3:2 aspect ratio. The old profile had a = 0.02392,
  // b = -0.07326, c = 0.03809 at 17 mm and declared a 4:3 aspect ratio.
  constexpr double kA           = 0.0350474;
  constexpr double kB           = -0.1115517;
  constexpr double kC           = 0.0777971;
  constexpr double kCropFactor  = 1.534;
  constexpr double kAspectRatio = 1.5;

  LensMatchCase    lens;
  lens.camera_maker           = "Sony";
  lens.camera_model           = "ILCE-6400";
  lens.lens_maker             = "";
  lens.lens_model             = "E 17-70mm F2.8 B070";
  lens.focal_length_mm        = 17.0f;
  lens.aperture_f_number      = 2.8f;
  lens.expected_profile_maker = "Tamron";
  lens.expected_profile_model = "Tamron 17-70mm F2.8 B070 Z/RF/X/E";
  lens.profile_has_vignetting = true;

  const auto result =
      LensCalibrationResolver::ResolveProfile(MetaFor(lens), AllCorrectionsSettings(), {});
  ASSERT_EQ(result.status_, LensProfileStatus::Resolved);
  ASSERT_EQ(result.params_.distortion_model,
            static_cast<std::int32_t>(LensCalibDistortionModel::PTLENS));
  ASSERT_GT(result.params_.real_focal_mm, 0.0f);

  // Lensfun PTLens terms are relative to the calibration image half diagonal. The kernels use
  // terms relative to the real focal length (the Hugin scaling of the Lensfun modifier).
  const double hugin_scale_mm = 43.2666153 / kCropFactor / std::hypot(kAspectRatio, 1.0) * 0.5;
  const double hugin_scaling  = result.params_.real_focal_mm / hugin_scale_mm;
  const double d              = 1.0 - kA - kB - kC;
  const double expected[]     = {kA * std::pow(hugin_scaling, 3.0) / std::pow(d, 4.0),
                                 kB * std::pow(hugin_scaling, 2.0) / std::pow(d, 3.0),
                                 kC * hugin_scaling / std::pow(d, 2.0)};
  for (std::size_t i = 0; i < std::size(expected); ++i) {
    EXPECT_NEAR(result.params_.distortion_terms[i], expected[i], std::fabs(expected[i]) * 1e-5)
        << "distortion_terms[" << i << "]";
  }
}

TEST(LensCalibDevelopResolve, RenamedSony16To50ProfileMatchesVersionOneAndTwoNames) {
  const LensMatchCase cases[] = {
      {"Sony", "ILCE-6400", "", "E PZ 16-50mm F3.5-5.6 OSS", 16.0f, 3.5f, "Sony",
       "E PZ 16-50mm f/3.5-5.6 OSS I/II", true},
      {"Sony", "ILCE-6400", "", "E PZ 16-50mm F3.5-5.6 OSS II", 16.0f, 3.5f, "Sony",
       "E PZ 16-50mm f/3.5-5.6 OSS I/II", true},
  };
  for (const auto& lens : cases) {
    ExpectProfileMatches(lens);
  }
}

// The EXIF lens name writes the aperture with an upper-case "F". Lensfun reads the focal range
// from the searched name only with a lower-case "f"; without it, "E 15-30mm F2.8" matched the Zeiss
// "E 21mm F2.8" by its words. The database has no profile for this lens.
TEST(LensCalibDevelopResolve, ExifNameFocalRangeRejectsProfileWithOtherFocalLengths) {
  LensInputMeta meta;
  meta.cam_maker_         = "Sony";
  meta.cam_model_         = "ILCE-7RM3";
  meta.lens_model_        = "E 15-30mm F2.8";
  meta.focal_length_mm_   = 15.0f;
  meta.aperture_f_number_ = 10.0f;
  meta.distance_m_        = 10.0f;
  const auto result = LensCalibrationResolver::ResolveProfile(meta, AllCorrectionsSettings(), {});
  EXPECT_EQ(result.status_, LensProfileStatus::LensNotInDatabase)
      << "matched " << result.matched_lens_maker_ << " / " << result.matched_lens_model_;
  EXPECT_FALSE(LensCalibrationResolver::ShotLensProfile(meta, LensDatabasePath()).has_value());

  // The search by words, which follows when the first search finds nothing, also keeps the named
  // focal range: a shot at 21 mm fits the Zeiss range, but its name states 21 mm, not 15-30 mm.
  meta.focal_length_mm_ = 21.0f;
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(meta, AllCorrectionsSettings(), {}).status_,
            LensProfileStatus::LensNotInDatabase);
}

namespace {

/// A RAW lens name of a Panasonic DC-S5M2 shot, without a lens maker, as the Sigma L-mount
/// lenses write it.
auto S5M2Shot(const char* lens_model, float focal_length_mm) -> LensInputMeta {
  LensInputMeta meta;
  meta.cam_maker_         = "Panasonic";
  meta.cam_model_         = "DC-S5M2";
  meta.lens_model_        = lens_model;
  meta.focal_length_mm_   = focal_length_mm;
  meta.aperture_f_number_ = 2.8f;
  meta.distance_m_        = 10.0f;
  return meta;
}

constexpr const char* kSigma28To70Profile = "28-70mm F2.8 DG DN | Contemporary 021";

}  // namespace

// Issue #1142: the Sigma L-mount EXIF name writes spaces around the dash. Lensfun read
// "28 - 70mm f2.8" as a 70 mm lens and matched the Sigma 70mm f/2.8 DG Macro.
TEST(LensCalibDevelopResolve, FocalRangeWithSpacesOrEnDashMatchesItsProfile) {
  const char* names[] = {
      "28 - 70mm F2.8 DG DN | Contemporary 021",
      "28-70mm F2.8 DG DN | Contemporary 021",
      "28\xE2\x80\x93"
      "70mm F2.8 DG DN | Contemporary 021",
      "  28 -70mm  F2.8 DG DN | Contemporary 021 ",
  };
  for (const char* name : names) {
    for (const float focal_mm : {28.0f, 50.0f, 70.0f}) {
      const auto meta = S5M2Shot(name, focal_mm);
      const auto result =
          LensCalibrationResolver::ResolveProfile(meta, AllCorrectionsSettings(), {});
      ASSERT_EQ(result.status_, LensProfileStatus::Resolved) << name << " at " << focal_mm;
      EXPECT_EQ(result.matched_lens_maker_, "Sigma") << name << " at " << focal_mm;
      EXPECT_EQ(result.matched_lens_model_, kSigma28To70Profile) << name << " at " << focal_mm;
      EXPECT_EQ(result.params_.apply_distortion, 1) << name << " at " << focal_mm;

      const auto shown = LensCalibrationResolver::ShotLensProfile(meta, LensDatabasePath());
      ASSERT_TRUE(shown.has_value()) << name << " at " << focal_mm;
      EXPECT_EQ(shown->model_, kSigma28To70Profile) << name << " at " << focal_mm;
    }
  }
}

// A profile whose focal range does not contain the shooting focal length names another lens: a
// shot at 28 mm is not a shot with a 70 mm prime, whatever the words of the name.
TEST(LensCalibDevelopResolve, ShotFocalLengthOutsideTheProfileRangeRejectsTheProfile) {
  const char* macro = "70mm F2.8 DG Macro | Art 018";
  const auto  at_28 =
      LensCalibrationResolver::ResolveProfile(S5M2Shot(macro, 28.0f), AllCorrectionsSettings(), {});
  EXPECT_EQ(at_28.status_, LensProfileStatus::LensNotInDatabase)
      << "matched " << at_28.matched_lens_maker_ << " / " << at_28.matched_lens_model_;
  EXPECT_FALSE(LensCalibrationResolver::ShotLensProfile(S5M2Shot(macro, 28.0f), LensDatabasePath())
                   .has_value());

  const auto at_70 =
      LensCalibrationResolver::ResolveProfile(S5M2Shot(macro, 70.0f), AllCorrectionsSettings(), {});
  ASSERT_EQ(at_70.status_, LensProfileStatus::Resolved);
  EXPECT_EQ(at_70.matched_lens_model_, "Sigma 70mm f/2.8 DG Macro | Art 018");
}

// Lensfun compares the aperture only with a value that the profile XML states, and most profiles
// state none. The names state it: an f/2 lens is not the f/3.5-5.6 lens of the same focal range.
TEST(LensCalibDevelopResolve, NamedApertureRejectsProfileOfAnotherAperture) {
  const LensMatchCase lenses[] = {
      {"SONY", "ILCE-1M2", "", "FE 28-70mm F2 GM", 28.0f, 2.0f, "", "", false},
      {"NIKON CORPORATION", "NIKON Z 8", "", "Viltrox AF 35mm f/1.2 LAB Z", 35.0f, 1.2f, "", "",
       false},
      {"NIKON CORPORATION", "NIKON D300", "", "18-125mm f/3.8-5.6", 86.0f, 5.6f, "", "", false},
  };
  for (const auto& lens : lenses) {
    const auto result =
        LensCalibrationResolver::ResolveProfile(MetaFor(lens), AllCorrectionsSettings(), {});
    EXPECT_EQ(result.status_, LensProfileStatus::LensNotInDatabase)
        << lens.lens_model << " matched " << result.matched_lens_maker_ << " / "
        << result.matched_lens_model_;
  }
}

// RAW names that the checks of auto-detect keep matching: a short name whose profile name adds
// words, and a shot whose recorded focal length (0.2 mm) is no measurement.
TEST(LensCalibDevelopResolve, ShortRawNameAndUnmeasuredFocalLengthKeepTheirProfiles) {
  const struct {
    LensMatchCase lens;
    const char*   expected_model;
  } cases[] = {
      {{"NIKON CORPORATION", "NIKON D7000", "", "17-55mm f/2.8", 35.0f, 2.8f, "", "", false},
       "Nikon AF-S DX Zoom-Nikkor 17-55mm f/2.8G IF-ED"},
      {{"SONY", "ILCE-7M5", "", "24-70mm F2.8 DG DN II | Art 024", 0.2f, 2.8f, "", "", false},
       "24-70mm F2.8 DG DN II | Art 024"},
  };
  for (const auto& c : cases) {
    const auto result =
        LensCalibrationResolver::ResolveProfile(MetaFor(c.lens), AllCorrectionsSettings(), {});
    ASSERT_EQ(result.status_, LensProfileStatus::Resolved) << c.lens.lens_model;
    EXPECT_EQ(result.matched_lens_model_, c.expected_model) << c.lens.lens_model;
  }
}

TEST(LensCalibDevelopResolve, PanasonicS5M2DngWithSigma28To70ResolvesItsProfile) {
  const auto sample_path =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "lumix" / "s5ii" / "P2625177.dng";
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }
  const auto prepared = RawInputLoader::LoadEncoded(ReadBytes(sample_path), DecodeRes::EIGHTH);
  EXPECT_EQ(prepared.color_context.lens_model_, "28 - 70mm F2.8 DG DN | Contemporary 021");

  LensInputMeta meta;
  meta.cam_maker_         = prepared.color_context.camera_make_;
  meta.cam_model_         = prepared.color_context.camera_model_;
  meta.lens_maker_        = prepared.color_context.lens_make_;
  meta.lens_model_        = prepared.color_context.lens_model_;
  meta.focal_length_mm_   = prepared.color_context.focal_length_mm_;
  meta.aperture_f_number_ = prepared.color_context.aperture_f_number_;
  const auto shown        = LensCalibrationResolver::ShotLensProfile(meta, LensDatabasePath());
  ASSERT_TRUE(shown.has_value());
  EXPECT_EQ(shown->model_, kSigma28To70Profile);

  DevelopPayload payload;
  payload.lens_enabled         = true;
  payload.lens_profile_db_path = LensDatabasePath();
  const auto runtime           = LensCalibrationResolver::Resolve(
      payload, prepared.color_context, prepared.CompileSource().develop_output_extent, {});
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(runtime->apply_distortion, 1);
}

// A catalog selection names the lens on the camera, also a lens of another mount on an adapter.
// The Lensfun search used to reject such a profile for its mount, so a selected Tamron, Pentax, or
// Canon lens corrected nothing on a Sony E camera.
TEST(LensCalibDevelopResolve, CatalogSelectionOfAnotherMountResolvesOnTheCamera) {
  const char* selections[][2] = {
      {"Tamron", "Tamron SP 15-30mm f/2.8 Di VC USD (A012)"},
      {"Pentax", "HD Pentax-D FA 15-30mm f/2.8 ED SDM WR"},
      {"Canon", "Canon RF 15-30mm F4.5-6.3 IS STM"},
  };
  for (const auto& selection : selections) {
    LensInputMeta meta;
    meta.cam_maker_         = "Sony";
    meta.cam_model_         = "ILCE-7RM3";
    meta.lens_maker_        = selection[0];
    meta.lens_model_        = selection[1];
    meta.focal_length_mm_   = 15.0f;
    meta.aperture_f_number_ = 10.0f;
    meta.lens_selected_     = true;
    const auto result = LensCalibrationResolver::ResolveProfile(meta, AllCorrectionsSettings(), {});
    ASSERT_EQ(result.status_, LensProfileStatus::Resolved) << selection[1];
    EXPECT_EQ(result.matched_lens_maker_, selection[0]);
    EXPECT_EQ(result.matched_lens_model_, selection[1]);
    EXPECT_EQ(result.params_.apply_distortion, 1) << selection[1];

    // The RAW lens name keeps the mount check.
    meta.lens_selected_ = false;
    EXPECT_EQ(LensCalibrationResolver::ResolveProfile(meta, AllCorrectionsSettings(), {}).status_,
              LensProfileStatus::LensNotInDatabase)
        << selection[1];
  }

  // Develop marks the payload lens as a selection.
  RawRuntimeColorContext context;
  context.valid_             = true;
  context.camera_make_       = "Sony";
  context.camera_model_      = "ILCE-7RM3";
  context.lens_model_        = "E 15-30mm F2.8";
  context.focal_length_mm_   = 15.0f;
  context.aperture_f_number_ = 10.0f;
  DevelopPayload payload;
  payload.lens_enabled         = true;
  payload.lens_profile_db_path = LensDatabasePath();
  EXPECT_FALSE(
      LensCalibrationResolver::Resolve(payload, context, Extent2D{96, 64}, {}).has_value());
  payload.lens_maker = "Tamron";
  payload.lens_model = "Tamron SP 15-30mm f/2.8 Di VC USD (A012)";
  const auto runtime = LensCalibrationResolver::Resolve(payload, context, Extent2D{96, 64}, {});
  ASSERT_TRUE(runtime.has_value());
  EXPECT_EQ(runtime->apply_distortion, 1);
}
// Issue #714: the Panasonic maker note names the lens with the Lensfun model name.
TEST(LensCalibDevelopResolve, PanasonicMakerNoteLensNameMatchesLensfunProfile) {
  ExpectProfileMatches({"Panasonic", "DC-G9", "", "LEICA DG 12-60/F2.8-4.0", 32.0f, 8.0f,
                        "Panasonic", "LEICA DG 12-60/F2.8-4.0", true});
}

// Issue #714: Develop reads the lens name of a DC-G9 RW2 from the maker note of the embedded
// JPEG, so Lens Correction finds the profile without a user lens selection.
TEST(LensCalibDevelopResolve, Rw2EmbeddedJpegLensNameResolvesProfileWithoutUserSelection) {
  const auto sample_path =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "bad_lens" / "P1832629.RW2";
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample RW2 not found: " << sample_path.string();
  }
  const auto prepared = RawInputLoader::LoadEncoded(ReadBytes(sample_path), DecodeRes::EIGHTH);
  EXPECT_EQ(prepared.color_context.lens_model_, "LEICA DG 12-60/F2.8-4.0");
  EXPECT_TRUE(prepared.color_context.lens_metadata_valid_);

  DevelopPayload payload;
  payload.lens_enabled         = true;
  payload.lens_profile_db_path = LensDatabasePath();
  const auto runtime           = LensCalibrationResolver::Resolve(
      payload, prepared.color_context, prepared.CompileSource().develop_output_extent, {});
  EXPECT_TRUE(runtime.has_value());
}

namespace {

/// A shot of a camera whose RAW writes no lens name, as the RAW color context reports it.
auto NamelessLensShot(const char* camera_maker, const char* camera_model, float focal_length_mm,
                      float aperture_f_number) -> LensInputMeta {
  LensInputMeta meta;
  meta.cam_maker_         = camera_maker;
  meta.cam_model_         = camera_model;
  meta.focal_length_mm_   = focal_length_mm;
  meta.aperture_f_number_ = aperture_f_number;
  meta.distance_m_        = 10.0f;
  return meta;
}

}  // namespace

// Issue #960: a fixed-lens camera writes no lens name; the camera names its built-in lens.
TEST(LensCalibDevelopResolve, FixedLensCameraWithoutLensNameResolvesBuiltInLensProfile) {
  const auto gfx = LensCalibrationResolver::ResolveProfile(
      NamelessLensShot("Fujifilm", "GFX100RF", 35.0f, 7.1f), AllCorrectionsSettings(), {});
  ASSERT_EQ(gfx.status_, LensProfileStatus::Resolved);
  EXPECT_EQ(gfx.matched_lens_maker_, "Fujifilm");
  EXPECT_EQ(gfx.matched_lens_model_, "GFX100RF & compatibles (Standard)");
  EXPECT_EQ(gfx.params_.apply_distortion, 1);
  EXPECT_EQ(gfx.params_.distortion_model,
            static_cast<std::int32_t>(LensCalibDistortionModel::PTLENS));

  const auto gr3x = LensCalibrationResolver::ResolveProfile(
      NamelessLensShot("Ricoh", "GR IIIx", 26.1f, 5.0f), AllCorrectionsSettings(), {});
  ASSERT_EQ(gr3x.status_, LensProfileStatus::Resolved);
  EXPECT_EQ(gr3x.matched_lens_model_, "Ricoh GR IIIx & compatibles");
  EXPECT_EQ(gr3x.params_.apply_distortion, 1);
  EXPECT_EQ(gr3x.params_.apply_vignetting, 1);

  const auto gr3 = LensCalibrationResolver::ResolveProfile(
      NamelessLensShot("Ricoh", "GR III", 18.3f, 2.8f), AllCorrectionsSettings(), {});
  ASSERT_EQ(gr3.status_, LensProfileStatus::Resolved);
  EXPECT_EQ(gr3.matched_lens_model_, "Ricoh GR III & compatibles + GW-4");
  EXPECT_EQ(gr3.params_.apply_distortion, 1);
  EXPECT_EQ(gr3.params_.apply_tca, 1);
  EXPECT_EQ(gr3.params_.apply_vignetting, 1);

  // The EXIF names, which the Lensfun entry lists first, select the same camera.
  const auto gr3x_exif = LensCalibrationResolver::ResolveProfile(
      NamelessLensShot("RICOH IMAGING COMPANY, LTD.", "RICOH GR IIIx", 26.1f, 5.0f),
      AllCorrectionsSettings(), {});
  ASSERT_EQ(gr3x_exif.status_, LensProfileStatus::Resolved);
  EXPECT_EQ(gr3x_exif.matched_lens_model_, "Ricoh GR IIIx & compatibles");
}

TEST(LensCalibDevelopResolve, FixedLensCameraWithUnknownLensNameResolvesBuiltInLensProfile) {
  auto meta         = NamelessLensShot("Fujifilm", "GFX100RF", 35.0f, 7.1f);
  meta.lens_model_  = "GF 35mm F4";
  const auto result = LensCalibrationResolver::ResolveProfile(meta, AllCorrectionsSettings(), {});
  ASSERT_EQ(result.status_, LensProfileStatus::Resolved);
  EXPECT_EQ(result.matched_lens_model_, "GFX100RF & compatibles (Standard)");
}

TEST(LensCalibDevelopResolve, InterchangeableLensCameraWithoutLensNameHasNoLensIdentity) {
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(
                NamelessLensShot("Fujifilm", "GFX100S", 35.0f, 4.0f), AllCorrectionsSettings(), {})
                .status_,
            LensProfileStatus::MissingLensIdentity);
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(
                NamelessLensShot("Sony", "ILCE-7M4", 50.0f, 1.4f), AllCorrectionsSettings(), {})
                .status_,
            LensProfileStatus::MissingLensIdentity);
}

// The auto-detect control shows the profile that ResolveProfile matches, so enabling the correction
// writes the lens that the render uses.
TEST(LensCalibDevelopResolve, ShotLensProfileNamesTheProfileThatResolveProfileMatches) {
  const auto gfx = LensCalibrationResolver::ShotLensProfile(
      NamelessLensShot("Fujifilm", "GFX100RF", 35.0f, 7.1f), LensDatabasePath());
  ASSERT_TRUE(gfx.has_value());
  EXPECT_EQ(gfx->maker_, "Fujifilm");
  EXPECT_EQ(gfx->model_, "GFX100RF & compatibles (Standard)");

  auto named        = NamelessLensShot("Sony", "ILCE-7M4", 50.0f, 1.4f);
  named.lens_model_ = "FE 50mm F1.4 GM";
  const auto sony   = LensCalibrationResolver::ShotLensProfile(named, LensDatabasePath());
  ASSERT_TRUE(sony.has_value());
  EXPECT_EQ(sony->maker_, "Sony");
  EXPECT_EQ(sony->model_, "FE 50mm f/1.4 GM");
  const auto resolved =
      LensCalibrationResolver::ResolveProfile(named, AllCorrectionsSettings(), {});
  EXPECT_EQ(resolved.matched_lens_maker_, sony->maker_);
  EXPECT_EQ(resolved.matched_lens_model_, sony->model_);

  EXPECT_FALSE(LensCalibrationResolver::ShotLensProfile(
                   NamelessLensShot("Fujifilm", "GFX100S", 35.0f, 4.0f), LensDatabasePath())
                   .has_value());
  EXPECT_FALSE(LensCalibrationResolver::ShotLensProfile(NamelessLensShot("", "", 35.0f, 4.0f),
                                                        LensDatabasePath())
                   .has_value());
}

// Issue #960: the camera maker and model that the RAW loader reports reach the Lensfun camera, so
// Develop corrects these files in Auto mode without a lens selection.
TEST(LensCalibDevelopResolve, FixedLensCameraRawsResolveProfileWithoutUserSelection) {
  struct Sample {
    const char* path;
    const char* expected_profile_model;
  };
  const Sample samples[] = {
      {"raw/camera/fuji/gfx100rf/4169961381.raf", "GFX100RF & compatibles (Standard)"},
      {"raw/camera/ricoh/gr3/R0000948.DNG", "Ricoh GR IIIx & compatibles"},
  };
  std::size_t checked = 0;
  for (const auto& sample : samples) {
    const auto sample_path = std::filesystem::path(TEST_IMG_PATH) / sample.path;
    if (!std::filesystem::exists(sample_path)) {
      continue;
    }
    ++checked;
    const auto prepared = RawInputLoader::LoadEncoded(ReadBytes(sample_path), DecodeRes::EIGHTH);
    EXPECT_TRUE(prepared.color_context.lens_model_.empty()) << sample.path;

    LensInputMeta meta;
    meta.cam_maker_         = prepared.color_context.camera_make_;
    meta.cam_model_         = prepared.color_context.camera_model_;
    meta.focal_length_mm_   = prepared.color_context.focal_length_mm_;
    meta.aperture_f_number_ = prepared.color_context.aperture_f_number_;
    const auto profile =
        LensCalibrationResolver::ResolveProfile(meta, AllCorrectionsSettings(), {});
    ASSERT_EQ(profile.status_, LensProfileStatus::Resolved)
        << sample.path << " camera \"" << meta.cam_maker_ << "\" / \"" << meta.cam_model_ << "\"";
    EXPECT_EQ(profile.matched_lens_model_, sample.expected_profile_model) << sample.path;

    DevelopPayload payload;
    payload.lens_enabled         = true;
    payload.lens_profile_db_path = LensDatabasePath();
    const auto runtime           = LensCalibrationResolver::Resolve(
        payload, prepared.color_context, prepared.CompileSource().develop_output_extent,
        dng::CorrectionsOf(prepared.dng_warp_rectilinear));
    EXPECT_TRUE(runtime.has_value()) << sample.path;
  }
  if (checked == 0) {
    GTEST_SKIP() << "No fixed-lens camera sample under " << TEST_IMG_PATH;
  }
}

}  // namespace alcedo
