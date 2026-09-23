//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/lens/lens_calibration_resolver.hpp"

#include <gtest/gtest.h>

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
 */
struct LensFixture {
  const char* expected_file;
  const char* catalog_lens_maker;
  const char* catalog_lens_model;
};

void ExpectFloatField(const nlohmann::json& expected, const char* name, float actual,
                      const std::string& context) {
  ASSERT_TRUE(expected.contains(name)) << context << " missing " << name;
  EXPECT_EQ(expected.at(name).get<float>(), actual) << context << " field " << name;
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
    EXPECT_EQ(values.at(i).get<float>(), actual[i])
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
        LensCalibrationResolver::Resolve(payload, prepared.color_context, extent, warp);

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
                                                        Extent2D{96, 64}, false);
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
                                                        Extent2D{96, 64}, false);
  EXPECT_FALSE(runtime.has_value());
}

TEST(LensCalibDevelopResolve, EmptyCatalogIdentityWithoutRawLensNameReturnsNoRuntime) {
  auto payload       = TouitPayload(true);
  payload.lens_maker = {};
  payload.lens_model = {};
  const auto runtime =
      LensCalibrationResolver::Resolve(payload, FocalOnlyContext(), Extent2D{96, 64}, false);
  EXPECT_FALSE(runtime.has_value());
}

TEST(LensCalibDevelopResolve, CatalogIdentityWithoutFocalLengthReturnsNoRuntime) {
  auto context                 = FocalOnlyContext();
  context.focal_length_mm_     = 0.0f;
  context.aperture_f_number_   = 0.0f;
  context.lens_metadata_valid_ = false;
  const auto runtime =
      LensCalibrationResolver::Resolve(TouitPayload(true), context, Extent2D{96, 64}, false);
  EXPECT_FALSE(runtime.has_value());
}

TEST(LensCalibDevelopResolve, EnabledPayloadWithEmptyExtentThrows) {
  EXPECT_THROW((void)LensCalibrationResolver::Resolve(TouitPayload(true), FocalOnlyContext(),
                                                      Extent2D{0, 0}, false),
               std::runtime_error);
}

TEST(LensCalibDevelopResolve, ProfileStatusNamesMissingIdentityAndUnknownLens) {
  LensCorrectionSettings settings;
  settings.database_path_ = LensDatabasePath();

  LensInputMeta no_focal;
  no_focal.lens_model_ = "Touit 1.8/32";
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(no_focal, settings, false).status_,
            LensProfileStatus::MissingLensIdentity);

  LensInputMeta unknown;
  unknown.lens_maker_      = "Nonexistent Optics";
  unknown.lens_model_      = "Imaginary 7.3/1234 Mk IX";
  unknown.focal_length_mm_ = 1234.0f;
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(unknown, settings, false).status_,
            LensProfileStatus::LensNotInDatabase);
}

TEST(LensCalibDevelopResolve, DngWarpKeepsVignettingAndDisablesGeometryCorrections) {
  auto payload       = TouitPayload(true);
  payload.apply_tca  = true;
  payload.apply_crop = true;
  const auto without_warp =
      LensCalibrationResolver::Resolve(payload, FocalOnlyContext(), Extent2D{96, 64}, false);
  const auto with_warp =
      LensCalibrationResolver::Resolve(payload, FocalOnlyContext(), Extent2D{96, 64}, true);
  ASSERT_TRUE(without_warp.has_value());
  ASSERT_TRUE(with_warp.has_value());
  EXPECT_EQ(without_warp->apply_distortion, 1);
  EXPECT_EQ(with_warp->apply_distortion, 0);
  EXPECT_EQ(with_warp->apply_tca, 0);
  EXPECT_EQ(with_warp->apply_projection, 0);
  EXPECT_EQ(with_warp->apply_crop, 0);
  EXPECT_EQ(with_warp->apply_vignetting, 1);
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
  EXPECT_EQ(LensCalibrationResolver::ResolveProfile(meta, settings, false).status_,
            LensProfileStatus::LensNotInDatabase);

  auto payload                 = TouitPayload(true);
  payload.lens_profile_db_path = broken_root.string();
  std::optional<LensCalibGpuParams> runtime;
  EXPECT_NO_THROW(runtime = LensCalibrationResolver::Resolve(payload, FocalOnlyContext(),
                                                             Extent2D{96, 64}, false));
  EXPECT_FALSE(runtime.has_value());

  // The database cache reloads the valid directory on the next call.
  EXPECT_TRUE(LensCalibrationResolver::Resolve(TouitPayload(true), FocalOnlyContext(),
                                               Extent2D{96, 64}, false)
                  .has_value());
  std::filesystem::remove_all(broken_root);
}

}  // namespace alcedo
