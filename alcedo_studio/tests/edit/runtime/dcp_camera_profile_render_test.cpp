// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// DCP profile reference in the Develop payload and the effective camera profile on the scalar
// render path: color transform, table packing, and content keys (DCP plan, phase C3).

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "edit/runtime/profile_tone_stage.hpp"
#include "edit/runtime/result_content_key.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "image/dcp_camera_profile.hpp"
#include "image/dcp_profile_library.hpp"
#include "image/dcp_source_tables.hpp"
#include "image/dng_color_profile.hpp"
#include "image/dng_color_profile_import.hpp"
#include "image/dng_tone_curve.hpp"

namespace alcedo {
namespace {

/// IFD0 of a DNG source: AnalogBalance, CameraCalibration1/2, both signatures, BaselineExposure,
/// and a 2x2x1 hue and saturation map. @p camera_signature is CameraCalibrationSignature;
/// @p profile_signature is the ProfileCalibrationSignature of the embedded profile.
auto MakeSourceExif(const std::string& camera_signature, const std::string& profile_signature)
    -> Exiv2::ExifData {
  Exiv2::ExifData exif;
  exif["Exif.Image.AnalogBalance"]         = "9/10 1/1 11/10";
  exif["Exif.Image.CameraCalibration1"]    = "105/100 0/1 0/1 0/1 1/1 0/1 0/1 0/1 95/100";
  exif["Exif.Image.CameraCalibration2"]    = "102/100 0/1 0/1 0/1 1/1 0/1 0/1 0/1 97/100";
  exif["Exif.Image.BaselineExposure"]      = "-1/2";
  exif["Exif.Image.ProfileHueSatMapDims"]  = "2 2 1";
  exif["Exif.Image.ProfileHueSatMapData1"] = "10 1 1 10 1.2 1 -20 1 1 -20 1.4 1";
  exif["Exif.Image.ProfileHueSatMapData2"] = "8 1 1 8 1.1 1 -15 1 1 -15 1.3 1";
  const Exiv2::AsciiValue camera(camera_signature);
  const Exiv2::AsciiValue profile(profile_signature);
  exif.add(Exiv2::ExifKey("Exif.Image.CameraCalibrationSignature"), &camera);
  exif.add(Exiv2::ExifKey("Exif.Image.ProfileCalibrationSignature"), &profile);
  return exif;
}

constexpr std::array<double, 9> kDcpColorMatrixA{0.92, -0.31, -0.06, -0.44, 1.21,
                                                 0.19, -0.08, 0.27,  0.68};
constexpr std::array<double, 9> kDcpColorMatrixD65{0.81, -0.22, -0.09, -0.41, 1.17,
                                                   0.27, -0.06, 0.18,  0.59};
constexpr std::array<double, 9> kDcpForwardMatrixA{0.71,  0.16, 0.09,  0.27, 0.81,
                                                   -0.08, 0.02, -0.17, 0.98};
constexpr std::array<double, 9> kDcpForwardMatrixD65{0.68,  0.19, 0.09,  0.25, 0.86,
                                                     -0.11, 0.01, -0.12, 0.94};
constexpr double                kBaselineExposureOffset = 0.25;

/// Hue, saturation, and value corrections of a table with saturation varying fastest. The first
/// saturation division of each hue keeps value scale 1, as DNG requires for the neutral axis.
auto MakeTable(std::array<std::uint32_t, 3> divisions, std::uint32_t encoding, float hue_shift)
    -> DngHueSatMap {
  DngHueSatMap map;
  map.divisions        = divisions;
  map.encoding         = encoding;
  const auto [h, s, v] = divisions;
  for (std::uint32_t value = 0; value < v; ++value) {
    for (std::uint32_t hue = 0; hue < h; ++hue) {
      for (std::uint32_t sat = 0; sat < s; ++sat) {
        map.entries.push_back(hue_shift * static_cast<float>(hue + 1));
        map.entries.push_back(1.0f + 0.05f * static_cast<float>(sat));
        map.entries.push_back(sat == 0 ? 1.0f : 0.9f + 0.03f * static_cast<float>(value));
      }
    }
  }
  return map;
}

struct DcpFixtureOptions {
  std::array<double, 9> color_matrix_1   = kDcpColorMatrixA;
  std::array<double, 9> color_matrix_2   = kDcpColorMatrixD65;
  std::array<double, 9> forward_matrix_1 = kDcpForwardMatrixA;
  std::array<double, 9> forward_matrix_2 = kDcpForwardMatrixD65;
  std::string           profile_name     = "Camera Standard";
  std::string           calibration_signature;  ///< ProfileCalibrationSignature.
  DngToneCurvePtr       tone_curve;             ///< ProfileToneCurve; null when the file lacks it.
};

auto MakeDcp(const DcpFixtureOptions& options = {}) -> DcpCameraProfilePtr {
  DngColorProfile tables;
  tables.name                     = options.profile_name;
  tables.hue_sat_map_1            = MakeTable({4, 3, 1}, 0, 2.0f);
  tables.hue_sat_map_2            = MakeTable({4, 3, 1}, 0, -3.0f);
  tables.look_table               = MakeTable({6, 4, 3}, 1, 1.5f);
  tables.baseline_exposure_offset = kBaselineExposureOffset;

  DcpCameraProfile profile;
  profile.unique_camera_model    = "Test Camera Z1";
  profile.profile_name           = options.profile_name;
  profile.color_matrix_1         = options.color_matrix_1;
  profile.color_matrix_2         = options.color_matrix_2;
  profile.forward_matrices_valid = true;
  profile.forward_matrix_1       = options.forward_matrix_1;
  profile.forward_matrix_2       = options.forward_matrix_2;
  profile.color_matrix_1_cct     = 2856.0;
  profile.color_matrix_2_cct     = 6504.0;
  profile.profile_calibration_signature = options.calibration_signature;
  profile.tables                        = MakeDngColorProfile(std::move(tables));
  profile.profile_tone_curve            = options.tone_curve;
  return MakeDcpCameraProfile(std::move(profile));
}

// Exiv2 reads the rational tags at float precision.
constexpr std::array<double, 9> kSourceCalibration1{1.05f, 0, 0, 0, 1, 0, 0, 0, 0.95f};
constexpr std::array<double, 9> kSourceCalibration2{1.02f, 0, 0, 0, 1, 0, 0, 0, 0.97f};

/// RAW payload of a DNG source: the import matrices of the CameraColor tests and the embedded
/// profile @p source, with solved as-shot values.
auto MakeDngSourcePayload(const DngColorProfilePtr& source) -> DevelopPayload {
  DevelopPayload payload;
  gpu_dag_test::FillTestCameraProfile(payload.camera_profile);
  payload.camera_profile.dng_profile = DngColorProfileRef(source);
  SolveDevelopAsShotWhiteBalance(payload);
  return payload;
}

auto MakeDngSourceModel(const DngColorProfilePtr& source) -> DevelopParamsModel {
  DevelopParamsModel model;
  model.ReplaceParams(MakeDngSourcePayload(source));
  return model;
}

/// Select @p dcp with the tables merged for the DNG profile of @p model, as AddDcpSourceTables
/// does with DcpProfileLibrary::Shared().
void SelectForSource(DevelopParamsModel& model, DcpProfileLibrary& library,
                     const DcpCameraProfilePtr& dcp) {
  const auto                     source = model.DngProfile();
  DevelopCameraProfileFileUpdate update;
  update.profile            = dcp;
  update.source_tables      = library.SourceTables(dcp, source.Profile());
  update.source_fingerprint = *source.Fingerprint();
  model.ApplyCameraProfileFileUpdate(std::move(update));
}

/// RAW payload with the import matrices of the CameraColor tests and solved as-shot values, as
/// BindDevelopCameraProfile leaves it after import.
auto MakeImportedPayload() -> DevelopPayload {
  DevelopPayload payload;
  gpu_dag_test::FillTestCameraProfile(payload.camera_profile);
  SolveDevelopAsShotWhiteBalance(payload);
  return payload;
}

auto MakeImportedModel() -> DevelopParamsModel {
  DevelopParamsModel model;
  model.ReplaceParams(MakeImportedPayload());
  return model;
}

auto RequireTransform(const DevelopPayload& payload) -> DevelopColorTransform {
  const auto result = ResolveDevelopColorTransform(payload);
  EXPECT_TRUE(result.ok) << ColorTransformFailureMessage(payload, result.error);
  return result.transform;
}

void ExpectMatrixNear(const std::array<float, 9>& actual, const std::array<double, 9>& expected,
                      double tolerance) {
  for (std::size_t i = 0; i < 9; ++i) {
    EXPECT_NEAR(actual[i], expected[i], tolerance) << "matrix element " << i;
  }
}

auto Invert(const std::array<double, 9>& m) -> std::array<double, 9> {
  const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                     m[2] * (m[3] * m[7] - m[4] * m[6]);
  return {(m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det,
          (m[1] * m[5] - m[2] * m[4]) / det, (m[5] * m[6] - m[3] * m[8]) / det,
          (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
          (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det,
          (m[0] * m[4] - m[1] * m[3]) / det};
}

auto ReadText(const std::string& path) -> std::string {
  std::ifstream stream(path, std::ios::binary);
  EXPECT_TRUE(stream.good()) << path;
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  return text;
}

TEST(DcpCameraProfileRender, DocumentWithoutReferenceSerializesAsBefore) {
  // Captured from DevelopParamsModel::ToJson at revision c54ac05fa (before this phase) for a RAW
  // payload with the import matrices of FillTestCameraProfile and default settings.
  const auto         expected = ReadText(std::string(ALCEDO_EXPECTED_JSON_DIR) +
                                         "/expected_develop_without_camera_profile_file.json");
  DevelopParamsModel model;
  auto               payload = model.Params();
  gpu_dag_test::FillTestCameraProfile(payload.camera_profile);
  model.ReplaceParams(payload);
  EXPECT_EQ(model.ToJson().dump(), expected);
  EXPECT_FALSE(model.ToJson().contains("camera_profile_file"));

  // Set and clear: the document returns to the same bytes.
  model.ApplyCameraProfileFileUpdate({MakeDcp()});
  model.ApplyCameraProfileFileUpdate({});
  auto restored         = model.Params();
  restored.as_shot_cct  = payload.as_shot_cct;
  restored.as_shot_tint = payload.as_shot_tint;
  model.ReplaceParams(restored);
  EXPECT_EQ(model.ToJson().dump(), expected);
}

TEST(DcpCameraProfileRender, JsonStoresReferenceAndNoProfileValues) {
  const auto dcp   = MakeDcp();
  auto       model = MakeImportedModel();
  EXPECT_TRUE(model.IsDefault());
  model.ApplyCameraProfileFileUpdate({dcp});
  EXPECT_FALSE(model.IsDefault());

  const auto json = model.ToJson();
  ASSERT_TRUE(json.contains("camera_profile_file"));
  const auto& stored = json.at("camera_profile_file");
  EXPECT_EQ(stored.size(), 3u);
  EXPECT_EQ(stored.at("camera_model"), "Test Camera Z1");
  EXPECT_EQ(stored.at("profile_name"), "Camera Standard");
  EXPECT_EQ(stored.at("fingerprint"), DngColorProfileFingerprintToText(dcp->fingerprint));

  // No DCP matrix element, table value, or exposure offset is in the document: apart from the
  // reference, the document has the keys of a document without one, and camera_profile keeps the
  // import values.
  const auto import_json = MakeImportedModel().ToJson();
  auto       without     = json;
  without.erase("camera_profile_file");
  ASSERT_EQ(without.size(), import_json.size());
  for (const auto& [key, value] : import_json.items()) {
    EXPECT_TRUE(without.contains(key)) << key;
  }
  EXPECT_EQ(json.at("camera_profile"), import_json.at("camera_profile"));
  EXPECT_EQ(model.Params().camera_profile, MakeImportedPayload().camera_profile);
}

TEST(DcpCameraProfileRender, LoadJsonKeepsBoundProfileOnlyForEqualReference) {
  const auto dcp   = MakeDcp();
  auto       model = MakeImportedModel();
  model.ApplyCameraProfileFileUpdate({dcp});
  const auto json = model.ToJson();

  model.LoadJson(json);
  ASSERT_TRUE(model.CameraProfileFile().has_value());
  EXPECT_EQ(model.CameraProfileFile()->profile, dcp);

  DevelopParamsModel fresh;
  fresh.LoadJson(json);
  const auto unbound = fresh.CameraProfileFile();
  ASSERT_TRUE(unbound.has_value());
  EXPECT_FALSE(unbound->IsBound());
  EXPECT_EQ(*unbound, *model.CameraProfileFile());

  auto other_file                                  = json;
  other_file["camera_profile_file"]["fingerprint"] = "0123456789abcdef";
  model.LoadJson(other_file);
  ASSERT_TRUE(model.CameraProfileFile().has_value());
  EXPECT_FALSE(model.CameraProfileFile()->IsBound());
  EXPECT_EQ(model.CameraProfileFile()->fingerprint, 0x0123456789abcdefULL);

  auto without = json;
  without.erase("camera_profile_file");
  model.LoadJson(without);
  EXPECT_FALSE(model.CameraProfileFile().has_value());
}

TEST(DcpCameraProfileRender, MalformedReferenceJsonIsRejected) {
  auto json = MakeImportedModel().ToJson();
  for (const auto& reference :
       {nlohmann::json(nullptr), nlohmann::json("Camera Standard"),
        nlohmann::json{{"camera_model", "Test Camera Z1"}, {"profile_name", "Camera Standard"}},
        nlohmann::json{{"camera_model", ""},
                       {"profile_name", "Camera Standard"},
                       {"fingerprint", "0123456789abcdef"}},
        nlohmann::json{{"camera_model", "Test Camera Z1"},
                       {"profile_name", "Camera Standard"},
                       {"fingerprint", "0123456789ABCDEF"}}}) {
    json["camera_profile_file"] = reference;
    DevelopParamsModel model;
    EXPECT_THROW(model.LoadJson(json), std::runtime_error) << reference.dump();
  }
}

TEST(DcpCameraProfileRender, ReplaceParamsInstallsBoundProfileOfEqualReference) {
  const auto dcp         = MakeDcp();
  auto       bound_model = MakeImportedModel();
  bound_model.ApplyCameraProfileFileUpdate({dcp});

  DevelopParamsModel model;
  model.LoadJson(bound_model.ToJson());
  ASSERT_FALSE(model.CameraProfileFile()->IsBound());
  const auto before  = model.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance});

  auto       payload = model.Params();
  payload.camera_profile_file->profile = dcp;
  model.ReplaceParams(payload);
  EXPECT_EQ(model.CameraProfileFile()->profile, dcp);
  EXPECT_NE(model.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance}), before);
  EXPECT_TRUE(ResolveDevelopColorTransform(model.Params()).ok);
}

TEST(DcpCameraProfileRender, BoundReferenceReplacesMatricesAndKeepsAsShotNeutral) {
  // One matrix for both illuminants, so the result does not depend on the solved CCT.
  DcpFixtureOptions options;
  options.color_matrix_2      = options.color_matrix_1;
  options.forward_matrix_2    = options.forward_matrix_1;
  const auto dcp              = MakeDcp(options);
  auto       model            = MakeImportedModel();
  const auto import_values    = model.Params().camera_profile;
  const auto import_transform = RequireTransform(model.Params());

  model.ApplyCameraProfileFileUpdate({dcp});
  const auto payload   = model.Params();
  const auto transform = RequireTransform(payload);
  EXPECT_EQ(payload.camera_profile, import_values);

  ExpectMatrixNear(transform.camera_to_xyz, Invert(kDcpColorMatrixA), 1e-5);

  // DNG: each ForwardMatrix row is scaled so that unit camera RGB maps to D50, and the camera
  // neutral of the shot (AsShotNeutral of the import, maximum 1) maps to unit camera RGB.
  const double                x = 0.34567, y = 0.35850;
  const std::array<double, 3> d50{x / y, 1.0, (1.0 - x - y) / y};
  const auto&                 neutral = import_values.as_shot_neutral;
  ASSERT_DOUBLE_EQ(*std::max_element(neutral.begin(), neutral.end()), 1.0);
  std::array<double, 9> expected{};
  for (int r = 0; r < 3; ++r) {
    const double sum =
        kDcpForwardMatrixA[r * 3] + kDcpForwardMatrixA[r * 3 + 1] + kDcpForwardMatrixA[r * 3 + 2];
    for (int c = 0; c < 3; ++c) {
      expected[r * 3 + c] = kDcpForwardMatrixA[r * 3 + c] * d50[r] / sum / neutral[c];
    }
  }
  ExpectMatrixNear(transform.camera_to_xyz_d50, expected, 1e-4);
  EXPECT_NE(transform.camera_to_xyz_d50, import_transform.camera_to_xyz_d50);
}

TEST(DcpCameraProfileRender, BoundReferencePacksDcpLookTable) {
  const auto dcp   = MakeDcp();
  auto       model = MakeImportedModel();
  EXPECT_EQ(PackDngProfileGpuData(model.Params(), RequireTransform(model.Params()))[0], 0.0f);

  model.ApplyCameraProfileFileUpdate({dcp});
  const auto  payload = model.Params();
  const auto  data    = PackDngProfileGpuData(payload, RequireTransform(payload));
  const auto& look    = dcp->tables->look_table;
  ASSERT_GE(data.size(), 32u);
  EXPECT_EQ(data[0], 1.0f);
  EXPECT_FLOAT_EQ(data[1], static_cast<float>(std::exp2(kBaselineExposureOffset)));
  ASSERT_GT(data[7], 0.0f);
  EXPECT_EQ(data[8], 6.0f);
  EXPECT_EQ(data[9], 4.0f);
  EXPECT_EQ(data[10], 3.0f);
  EXPECT_EQ(data[11], 1.0f);
  const auto offset = static_cast<std::size_t>(data[7]);
  ASSERT_EQ(data.size(), offset + look.entries.size());
  EXPECT_TRUE(std::equal(look.entries.begin(), look.entries.end(), data.begin() + offset));
  // The hue and saturation maps are blended by the DCP illuminant CCT values, so the packed map
  // lies between the two DCP maps.
  const auto  map_offset = static_cast<std::size_t>(data[2]);
  const auto& map_1      = dcp->tables->hue_sat_map_1.entries;
  const auto& map_2      = dcp->tables->hue_sat_map_2.entries;
  for (std::size_t i = 0; i < map_1.size(); ++i) {
    EXPECT_GE(data[map_offset + i], std::min(map_1[i], map_2[i]) - 1e-6f) << i;
    EXPECT_LE(data[map_offset + i], std::max(map_1[i], map_2[i]) + 1e-6f) << i;
  }
}

TEST(DcpCameraProfileRender, ClearingReferenceRestoresImportTransform) {
  auto       model     = MakeImportedModel();
  const auto before    = model.Params();
  const auto transform = RequireTransform(before);

  model.ApplyCameraProfileFileUpdate({MakeDcp()});
  EXPECT_NE(RequireTransform(model.Params()).camera_to_ap1, transform.camera_to_ap1);
  model.ApplyCameraProfileFileUpdate({});

  const auto after = model.Params();
  EXPECT_EQ(after, before);
  const auto restored = RequireTransform(after);
  EXPECT_EQ(restored.camera_to_xyz, transform.camera_to_xyz);
  EXPECT_EQ(restored.camera_to_xyz_d50, transform.camera_to_xyz_d50);
  EXPECT_EQ(restored.camera_to_ap1, transform.camera_to_ap1);
  EXPECT_EQ(PackDngProfileGpuData(after, restored), PackDngProfileGpuData(before, transform));
}

TEST(DcpCameraProfileRender, UnboundReferenceFailsWithNamedError) {
  auto bound_model = MakeImportedModel();
  bound_model.ApplyCameraProfileFileUpdate({MakeDcp()});
  DevelopParamsModel model;
  model.LoadJson(bound_model.ToJson());
  const auto payload = model.Params();

  const auto result  = ResolveDevelopColorTransform(payload);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, ColorTransformError::UnboundCameraProfileFile);
  const auto message = ColorTransformFailureMessage(payload, result.error);
  EXPECT_NE(message.find("Camera Standard"), std::string::npos) << message;
  EXPECT_NE(message.find("Test Camera Z1"), std::string::npos) << message;

  try {
    (void)PackDngProfileGpuData(payload, DevelopColorTransform{});
    ADD_FAILURE() << "PackDngProfileGpuData accepted an unbound reference";
  } catch (const std::runtime_error& error) {
    EXPECT_EQ(std::string(error.what()), message);
  }
  EXPECT_THROW((void)EffectiveCameraProfile(payload), std::runtime_error);
  EXPECT_FALSE(RawWhiteBalanceCct(payload).has_value());
}

TEST(DcpCameraProfileRender, ProfileChangeKeepsSensorKeyAndChangesCameraKey) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12));
  auto document = CreateDefaultPipelineDocument();
  document.Develop()->Params().ReplaceParams(MakeImportedPayload());
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto base = BuildFrameResultContentKeys(plan, prepared, document);

  document.Develop()->Params().ApplyCameraProfileFileUpdate({MakeDcp()});
  const auto selected = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(selected.sensor_linear, base.sensor_linear);
  EXPECT_EQ(selected.geometry_scene_source, base.geometry_scene_source);
  EXPECT_NE(selected.develop_image, base.develop_image);

  DcpFixtureOptions other;
  other.profile_name = "Camera Vivid";
  document.Develop()->Params().ApplyCameraProfileFileUpdate({MakeDcp(other)});
  const auto changed = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(changed.sensor_linear, base.sensor_linear);
  EXPECT_NE(changed.develop_image, selected.develop_image);

  document.Develop()->Params().ApplyCameraProfileFileUpdate({});
  const auto cleared = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(cleared.develop_image, base.develop_image);
}

TEST(DcpCameraProfileRender, AsShotValuesFollowEffectiveMatrices) {
  const auto dcp     = MakeDcp();
  auto       model   = MakeImportedModel();
  auto       custom  = DevelopColorTemperatureUpdate{};
  custom.custom_cct  = 4321.0f;
  custom.custom_tint = 12.0f;
  model.ApplyColorTemperatureUpdate(custom);
  const auto import_cct = model.AsShotCct();
  const auto before     = model.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance});

  model.ApplyCameraProfileFileUpdate({dcp});

  // Independent solve: the import neutral with the DCP matrices as plain camera matrices.
  DevelopPayload expected;
  expected.camera_profile                               = model.Params().camera_profile;
  expected.camera_profile.color_matrix_1                = dcp->color_matrix_1;
  expected.camera_profile.color_matrix_2                = dcp->color_matrix_2;
  expected.camera_profile.forward_matrices_valid        = false;
  expected.camera_profile.calibration_illuminants_valid = true;
  expected.camera_profile.color_matrix_1_cct            = dcp->color_matrix_1_cct;
  expected.camera_profile.color_matrix_2_cct            = dcp->color_matrix_2_cct;
  const auto solved                                     = RequireTransform(expected);

  EXPECT_EQ(model.AsShotCct(), solved.resolved_cct);
  EXPECT_EQ(model.AsShotTint(), solved.resolved_tint);
  EXPECT_GT(std::abs(model.AsShotCct() - import_cct), 50.0f);
  EXPECT_EQ(model.CustomCct(), 4321.0f);
  EXPECT_EQ(model.CustomTint(), 12.0f);
  EXPECT_NE(model.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance}), before);

  // The same profile again changes nothing.
  const auto after_set = model.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance});
  model.ApplyCameraProfileFileUpdate({dcp});
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance}), after_set);

  // The import binding solves with the effective matrices too.
  auto rebound        = model.Params();
  rebound.as_shot_cct = 6500.0f;
  SolveDevelopAsShotWhiteBalance(rebound);
  EXPECT_EQ(rebound.as_shot_cct, solved.resolved_cct);
}

TEST(DcpCameraProfileRender, RasterDocumentRejectsUpdate) {
  DevelopParamsModel model;
  auto               payload = model.Params();
  BindRgbWorkingSpaceCameraProfile(payload);
  payload.input = DevelopRasterInput{};
  model.ReplaceParams(payload);
  const auto before   = model.Params();
  const auto revision = model.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance});

  EXPECT_THROW(model.ApplyCameraProfileFileUpdate({MakeDcp()}), std::logic_error);
  EXPECT_EQ(model.Params(), before);
  EXPECT_FALSE(model.CameraProfileFile().has_value());
  EXPECT_EQ(model.FieldsRevision(DirtyFieldMask{DevelopDirty::WhiteBalance}), revision);
}

TEST(DcpCameraProfileRender, StoredDngFingerprintIsUnchangedByThisPhase) {
  // Captured from ReadDngColorProfile at revision 99632f8f6 (before phase C6) for the fixture
  // source, with equal and with different calibration signatures.
  const auto matched   = ReadDngColorProfile(MakeSourceExif("com.example", "com.example"));
  const auto unmatched = ReadDngColorProfile(MakeSourceExif("com.example", "com.other"));
  EXPECT_EQ(DngColorProfileFingerprintToText(matched->fingerprint), "8e0ba24fda9f0e83");
  EXPECT_EQ(DngColorProfileFingerprintToText(unmatched->fingerprint), "972fcc24583d5cf5");
}

TEST(DcpCameraProfileRender, DngSourceKeepsAnalogBalanceAndBaselineExposureWithDcpProfile) {
  const auto        source = ReadDngColorProfile(MakeSourceExif("com.example", "com.example"));
  const auto        dcp    = MakeDcp();
  DcpProfileLibrary library;
  auto              model = MakeDngSourceModel(source);
  SelectForSource(model, library, dcp);

  const auto payload = model.Params();
  ASSERT_TRUE(payload.camera_profile_file.has_value());
  EXPECT_EQ(payload.camera_profile_file->source_fingerprint, source->fingerprint);
  EXPECT_EQ(payload.camera_profile.dng_profile.Profile(), source) << "import values stay";
  const auto effective = EffectiveCameraProfile(payload).dng_profile;
  ASSERT_TRUE(effective.IsBound());
  EXPECT_EQ(effective.Profile(), payload.camera_profile_file->source_tables);
  // Source values.
  EXPECT_EQ(effective->analog_balance, (std::array<double, 3>{0.9f, 1.0f, 1.1f}));
  EXPECT_EQ(effective->analog_balance, source->analog_balance);
  EXPECT_EQ(effective->baseline_exposure, -0.5);
  // DCP values.
  EXPECT_EQ(effective->baseline_exposure_offset, kBaselineExposureOffset);
  EXPECT_EQ(effective->look_table, dcp->tables->look_table);
  EXPECT_EQ(effective->hue_sat_map_1, dcp->tables->hue_sat_map_1);
  EXPECT_EQ(effective->hue_sat_map_2, dcp->tables->hue_sat_map_2);
  EXPECT_NE(effective->hue_sat_map_1, source->hue_sat_map_1);

  const auto transform = RequireTransform(payload);
  const auto data      = PackDngProfileGpuData(payload, transform);
  ASSERT_GE(data.size(), 32u);
  EXPECT_FLOAT_EQ(data[1], static_cast<float>(std::exp2(-0.5 + kBaselineExposureOffset)));
  const auto  offset = static_cast<std::size_t>(data[7]);
  const auto& look   = dcp->tables->look_table.entries;
  ASSERT_EQ(data.size(), offset + look.size());
  EXPECT_TRUE(std::equal(look.begin(), look.end(), data.begin() + offset));

  // Without the source values (the C3 rule for a RAW source) the camera matrix differs: the
  // analog balance enters XYZ to camera.
  auto without                                    = payload;
  without.camera_profile.dng_profile              = {};
  without.camera_profile_file->source_tables      = nullptr;
  without.camera_profile_file->source_fingerprint = 0;
  EXPECT_NE(RequireTransform(without).camera_to_xyz, transform.camera_to_xyz);
}

TEST(DcpCameraProfileRender, CameraCalibrationAppliesOnlyWhenSignaturesMatch) {
  // The embedded profile has another signature than the camera, so its own calibration is
  // identity; only the DCP signature decides for the DCP tables.
  const auto source = ReadDngColorProfile(MakeSourceExif("com.example", "com.other"));
  EXPECT_EQ(source->camera_calibration_1, kDngIdentityMatrix);
  EXPECT_EQ(source->source_calibration.signature, "com.example");
  EXPECT_EQ(source->source_calibration.camera_calibration_1, kSourceCalibration1);
  EXPECT_EQ(source->source_calibration.camera_calibration_2, kSourceCalibration2);

  DcpFixtureOptions matching;
  matching.calibration_signature = "com.example";
  const auto equal               = MergeDcpTablesWithSourceCalibration(*MakeDcp(matching), *source);
  EXPECT_EQ(equal->camera_calibration_1, kSourceCalibration1);
  EXPECT_EQ(equal->camera_calibration_2, kSourceCalibration2);
  EXPECT_EQ(equal->source_calibration, DngSourceCalibration{});

  DcpFixtureOptions other;
  other.calibration_signature = "com.other";
  for (const auto& dcp : {MakeDcp(other), MakeDcp()}) {
    const auto different = MergeDcpTablesWithSourceCalibration(*dcp, *source);
    EXPECT_EQ(different->camera_calibration_1, kDngIdentityMatrix) << dcp->profile_name;
    EXPECT_EQ(different->camera_calibration_2, kDngIdentityMatrix);
    EXPECT_EQ(different->analog_balance, source->analog_balance);
  }

  // DNG permits a BYTE signature; it reads as the same text.
  auto       exif = MakeSourceExif("com.example", "com.other");
  const auto key  = Exiv2::ExifKey("Exif.Image.CameraCalibrationSignature");
  exif.erase(exif.findKey(key));
  const std::string text("com.example");
  Exiv2::DataValue  bytes(Exiv2::unsignedByte);
  bytes.read(reinterpret_cast<const Exiv2::byte*>(text.c_str()), text.size() + 1);
  exif.add(key, &bytes);
  const auto byte_source = ReadDngColorProfile(exif);
  EXPECT_EQ(byte_source->source_calibration.signature, "com.example");
  EXPECT_EQ(
      MergeDcpTablesWithSourceCalibration(*MakeDcp(matching), *byte_source)->camera_calibration_1,
      kSourceCalibration1);
}

TEST(DcpCameraProfileRender, SourceTablesAreSharedForEqualInputs) {
  const auto        source = ReadDngColorProfile(MakeSourceExif("com.example", "com.example"));
  const auto        dcp    = MakeDcp();
  DcpProfileLibrary library;
  {
    const auto first  = library.SourceTables(dcp, source);
    const auto second = library.SourceTables(dcp, source);
    EXPECT_EQ(first, second);
    EXPECT_EQ(library.SourceTablesMergeCount(), 1u);
  }
  // The map holds no strong reference: the released result is merged again.
  (void)library.SourceTables(dcp, source);
  EXPECT_EQ(library.SourceTablesMergeCount(), 2u);

  // Another source calibration with the same source fingerprint is another merge. Both embedded
  // profiles have another signature than the camera, so both fingerprints omit the calibration.
  DcpFixtureOptions matching;
  matching.calibration_signature = "com.example";
  const auto matching_dcp        = MakeDcp(matching);
  const auto example             = ReadDngColorProfile(MakeSourceExif("com.example", "com.other"));
  const auto third               = ReadDngColorProfile(MakeSourceExif("com.third", "com.other"));
  ASSERT_EQ(example->fingerprint, third->fingerprint);
  const auto example_tables = library.SourceTables(matching_dcp, example);
  const auto third_tables   = library.SourceTables(matching_dcp, third);
  EXPECT_NE(example_tables, third_tables);
  EXPECT_EQ(example_tables->camera_calibration_1, kSourceCalibration1);
  EXPECT_EQ(third_tables->camera_calibration_1, kDngIdentityMatrix);
  EXPECT_THROW((void)library.SourceTables(nullptr, source), std::invalid_argument);
}

TEST(DcpCameraProfileRender, ClearingReferenceOnDngSourceRestoresEmbeddedProfile) {
  const auto        source = ReadDngColorProfile(MakeSourceExif("com.example", "com.example"));
  DcpProfileLibrary library;
  auto              model     = MakeDngSourceModel(source);
  const auto        before    = model.Params();
  const auto        transform = RequireTransform(before);
  const auto        packed    = PackDngProfileGpuData(before, transform);

  SelectForSource(model, library, MakeDcp());
  EXPECT_NE(PackDngProfileGpuData(model.Params(), RequireTransform(model.Params())), packed);
  model.ApplyCameraProfileFileUpdate({});

  const auto after = model.Params();
  EXPECT_EQ(after, before);
  EXPECT_EQ(after.camera_profile.dng_profile.Profile(), source);
  const auto restored = RequireTransform(after);
  EXPECT_EQ(restored.camera_to_xyz, transform.camera_to_xyz);
  EXPECT_EQ(restored.camera_to_xyz_d50, transform.camera_to_xyz_d50);
  EXPECT_EQ(restored.camera_to_ap1, transform.camera_to_ap1);
  EXPECT_EQ(PackDngProfileGpuData(after, restored), packed);
}

TEST(DcpCameraProfileRender, SourceTablesOfAnotherDngProfileFailUntilMergedAgain) {
  const auto        source = ReadDngColorProfile(MakeSourceExif("com.example", "com.example"));
  const auto        dcp    = MakeDcp();
  DcpProfileLibrary library;
  auto              model  = MakeDngSourceModel(source);

  // A DNG source rejects a selection without merged tables; a RAW source rejects merged tables.
  const auto        before = model.Params();
  EXPECT_THROW(model.ApplyCameraProfileFileUpdate({dcp}), std::invalid_argument);
  EXPECT_EQ(model.Params(), before);
  EXPECT_FALSE(model.CameraProfileFile().has_value());
  auto                           raw = MakeImportedModel();
  DevelopCameraProfileFileUpdate with_tables;
  with_tables.profile            = dcp;
  with_tables.source_tables      = library.SourceTables(dcp, source);
  with_tables.source_fingerprint = source->fingerprint;
  EXPECT_THROW(raw.ApplyCameraProfileFileUpdate(with_tables), std::invalid_argument);
  EXPECT_FALSE(raw.CameraProfileFile().has_value());

  // An import binding that gives another DNG profile leaves the old merge: no render with it.
  SelectForSource(model, library, dcp);
  const auto replaced = ReadDngColorProfile(MakeSourceExif("com.example", "com.other"));
  ASSERT_NE(replaced->fingerprint, source->fingerprint);
  auto payload                       = model.Params();
  payload.camera_profile.dng_profile = DngColorProfileRef(replaced);
  model.ReplaceParams(payload);
  const auto stale  = model.Params();
  const auto result = ResolveDevelopColorTransform(stale);
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, ColorTransformError::UnboundCameraProfileFile);
  const auto message = ColorTransformFailureMessage(stale, result.error);
  EXPECT_NE(message.find("not bound to the DNG profile"), std::string::npos) << message;
  EXPECT_THROW((void)EffectiveCameraProfile(stale), std::runtime_error);

  SelectForSource(model, library, dcp);
  EXPECT_TRUE(ResolveDevelopColorTransform(model.Params()).ok);
  EXPECT_EQ(model.CameraProfileFile()->source_fingerprint, replaced->fingerprint);
}

TEST(DcpCameraProfileRender, ComparisonDocumentTakesReferenceFromCurrent) {
  const auto dcp     = MakeDcp();
  auto       current = MakeImportedModel();
  current.ApplyCameraProfileFileUpdate({dcp});

  auto comparison = MakeImportedModel();
  comparison.UseSensorSettingsFrom(current);
  ASSERT_TRUE(comparison.CameraProfileFile().has_value());
  EXPECT_EQ(*comparison.CameraProfileFile(), *current.CameraProfileFile());
  EXPECT_EQ(comparison.CameraProfileFile()->profile, dcp);

  auto analysis = MakeImportedModel();
  analysis.UseAllSettingsFrom(current);
  ASSERT_TRUE(analysis.CameraProfileFile().has_value());
  EXPECT_EQ(analysis.CameraProfileFile()->profile, dcp);
  EXPECT_EQ(analysis.Params(), current.Params());
}

// --- Profile tone stage (dcp_profile_tone_curve_study.md, phase T2) ---

auto ToneCurve() -> DngToneCurvePtr {
  return MakeDngToneCurve({{0.0f, 0.0f}, {0.18f, 0.44f}, {0.6f, 0.9f}, {1.0f, 1.0f}});
}

auto ToneDcp() -> DcpCameraProfilePtr {
  DcpFixtureOptions options;
  options.tone_curve = ToneCurve();
  return MakeDcp(options);
}

auto SelectTone(DevelopParamsModel& model, const DcpCameraProfilePtr& dcp, bool profile_tone) {
  DevelopCameraProfileFileUpdate update;
  update.profile      = dcp;
  update.profile_tone = profile_tone;
  model.ApplyCameraProfileFileUpdate(std::move(update));
}

auto SetDrtMethod(PipelineDocument& document, DrtMethod method) {
  DrtParameterUpdate update;
  update.method = method;
  document.Drt()->Params().ApplyUpdate(update);
}

TEST(DcpCameraProfileRender, ProfileToneIsWrittenOnlyAsFalse) {
  auto model = MakeImportedModel();
  SelectTone(model, ToneDcp(), true);
  const auto on = model.ToJson().at("camera_profile_file");
  EXPECT_FALSE(on.contains("profile_tone")) << on.dump();
  EXPECT_TRUE(DevelopCameraProfileFileFromJson(on).profile_tone);

  SelectTone(model, ToneDcp(), false);
  const auto off = model.ToJson().at("camera_profile_file");
  ASSERT_TRUE(off.contains("profile_tone")) << off.dump();
  EXPECT_EQ(off.at("profile_tone"), false);
  EXPECT_FALSE(DevelopCameraProfileFileFromJson(off).profile_tone);

  auto malformed            = off;
  malformed["profile_tone"] = "no";
  EXPECT_THROW((void)DevelopCameraProfileFileFromJson(malformed), std::runtime_error);
  // The stored values compare; the profile identity alone does not.
  const auto stored_on  = DevelopCameraProfileFileFromJson(on);
  const auto stored_off = DevelopCameraProfileFileFromJson(off);
  EXPECT_NE(stored_on, stored_off);
  EXPECT_TRUE(stored_on.SameProfile(stored_off));
}

TEST(DcpCameraProfileRender, ProfileToneSwitchMarksOnlyTheProfileToneField) {
  const auto dcp   = ToneDcp();
  auto       model = MakeImportedModel();
  SelectTone(model, dcp, true);
  const DirtyFieldMask white_balance{DevelopDirty::WhiteBalance};
  const DirtyFieldMask profile_tone{DevelopDirty::ProfileTone};
  const auto           wb_before   = model.FieldsRevision(white_balance);
  const auto           tone_before = model.FieldsRevision(profile_tone);

  SelectTone(model, dcp, false);
  EXPECT_EQ(model.FieldsRevision(white_balance), wb_before);
  EXPECT_NE(model.FieldsRevision(profile_tone), tone_before);
  EXPECT_FALSE(model.CameraProfileFile()->profile_tone);
  EXPECT_TRUE(model.CameraProfileFile()->IsBound());

  // An equal write changes nothing.
  const auto tone_off = model.FieldsRevision(profile_tone);
  SelectTone(model, dcp, false);
  EXPECT_EQ(model.FieldsRevision(profile_tone), tone_off);

  // LoadJson of the switched reference keeps the bound profile.
  auto json = model.ToJson();
  json["camera_profile_file"].erase("profile_tone");
  model.LoadJson(json);
  EXPECT_TRUE(model.CameraProfileFile()->profile_tone);
  EXPECT_EQ(model.CameraProfileFile()->profile, dcp);

  // ReplaceParams with only the switch changed marks the switch and not the camera pass.
  auto payload                              = model.Params();
  payload.camera_profile_file->profile_tone = false;
  const auto wb_replace                     = model.FieldsRevision(white_balance);
  const auto tone_replace                   = model.FieldsRevision(profile_tone);
  model.ReplaceParams(payload);
  EXPECT_EQ(model.FieldsRevision(white_balance), wb_replace);
  EXPECT_NE(model.FieldsRevision(profile_tone), tone_replace);
}

TEST(DcpCameraProfileRender, ProfileToneStageRunsOnlyForABoundCurveWithAces20OrOpenDrt) {
  auto document = CreateDefaultPipelineDocument();
  document.Develop()->Params().ReplaceParams(MakeImportedPayload());
  EXPECT_FALSE(ResolveProfileToneStage(document).has_value()) << "no reference";

  const auto dcp = ToneDcp();
  SelectTone(document.Develop()->Params(), dcp, true);
  for (const auto method : {DrtMethod::Aces20, DrtMethod::OpenDrt}) {
    SetDrtMethod(document, method);
    const auto stage = ResolveProfileToneStage(document);
    ASSERT_TRUE(stage.has_value());
    EXPECT_EQ(stage->curve, dcp->profile_tone_curve);
    EXPECT_EQ(stage->method, method);
  }
  SetDrtMethod(document, DrtMethod::FilmPrint);
  EXPECT_FALSE(ResolveProfileToneStage(document).has_value()) << "Film Print";
  // The stored setting stays under Film Print.
  EXPECT_TRUE(document.Develop()->Params().CameraProfileFile()->profile_tone);

  SetDrtMethod(document, DrtMethod::Aces20);
  SelectTone(document.Develop()->Params(), dcp, false);
  EXPECT_FALSE(ResolveProfileToneStage(document).has_value()) << "profile_tone false";
  document.Develop()->Params().ApplyCameraProfileFileUpdate({MakeDcp()});
  EXPECT_FALSE(ResolveProfileToneStage(document).has_value()) << "profile without the curve";

  // An unbound reference whose stage would run fails with its identity.
  SelectTone(document.Develop()->Params(), dcp, true);
  auto loaded = CreateDefaultPipelineDocument();
  SetDrtMethod(loaded, DrtMethod::Aces20);
  loaded.Develop()->Params().LoadJson(document.Develop()->Params().ToJson());
  ASSERT_FALSE(loaded.Develop()->Params().CameraProfileFile()->IsBound());
  try {
    (void)ResolveProfileToneStage(loaded);
    ADD_FAILURE() << "ResolveProfileToneStage accepted an unbound reference";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("Camera Standard"), std::string::npos) << error.what();
  }
}

TEST(DcpCameraProfileRender, ProfileToneSwitchKeepsTheCameraKeyAndChangesTheDisplayKey) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12));
  auto document = CreateDefaultPipelineDocument();
  document.Develop()->Params().ReplaceParams(MakeImportedPayload());
  const auto plan  = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  const auto dcp   = ToneDcp();
  auto&      model = document.Develop()->Params();

  SelectTone(model, dcp, false);
  const auto off = BuildFrameResultContentKeys(plan, prepared, document);
  SelectTone(model, dcp, true);
  const auto on = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(on.develop_image, off.develop_image);
  EXPECT_EQ(on.primary_grade, off.primary_grade);
  EXPECT_NE(on.drt_display, off.drt_display);

  SetDrtMethod(document, DrtMethod::OpenDrt);
  const auto open_drt_on = BuildFrameResultContentKeys(plan, prepared, document);
  SelectTone(model, dcp, false);
  const auto open_drt_off = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_NE(open_drt_on.drt_display, open_drt_off.drt_display);

  // Film Print keeps the stage out, so the switch keeps its display key.
  SetDrtMethod(document, DrtMethod::FilmPrint);
  const auto film_off = BuildFrameResultContentKeys(plan, prepared, document);
  SelectTone(model, dcp, true);
  const auto film_on = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(film_on.drt_display, film_off.drt_display);

  // A profile without the curve has the display key of the switch turned off.
  SetDrtMethod(document, DrtMethod::Aces20);
  DcpFixtureOptions flat_options;
  flat_options.profile_name = "Camera Flat";
  const auto flat           = MakeDcp(flat_options);
  SelectTone(model, flat, true);
  const auto flat_on = BuildFrameResultContentKeys(plan, prepared, document);
  SelectTone(model, flat, false);
  const auto flat_off = BuildFrameResultContentKeys(plan, prepared, document);
  EXPECT_EQ(flat_on.drt_display, flat_off.drt_display);
}

TEST(DcpCameraProfileRender, ProfileToneSwitchInvalidatesTheDisplayResultOnly) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(16, 12),
                                                gpu_dag_test::FullSensor(16, 12));
  auto document = CreateDefaultPipelineDocument();
  document.Develop()->Params().ReplaceParams(MakeImportedPayload());
  const auto dcp = ToneDcp();
  SelectTone(document.Develop()->Params(), dcp, true);
  const auto plan = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  RuntimeInvalidationState state;
  state.BindCompiledPlan(plan);
  state.CollectAndPropagate(plan, document, prepared);
  const auto develop = state.RequiredRevision(plan.develop_output);
  const auto scene   = state.RequiredRevision(plan.SceneInputForDrt());
  const auto display = state.RequiredRevision(plan.display_output);

  SelectTone(document.Develop()->Params(), dcp, false);
  state.CollectAndPropagate(plan, document, prepared);
  EXPECT_EQ(state.RequiredRevision(plan.develop_output), develop);
  EXPECT_EQ(state.RequiredRevision(plan.SceneInputForDrt()), scene);
  EXPECT_NE(state.RequiredRevision(plan.display_output), display);

  // A profile change still invalidates the camera pass.
  const auto display_off = state.RequiredRevision(plan.display_output);
  document.Develop()->Params().ApplyCameraProfileFileUpdate({MakeDcp()});
  state.CollectAndPropagate(plan, document, prepared);
  EXPECT_NE(state.RequiredRevision(plan.develop_output), develop);
  EXPECT_NE(state.RequiredRevision(plan.display_output), display_off);
}

}  // namespace
}  // namespace alcedo
