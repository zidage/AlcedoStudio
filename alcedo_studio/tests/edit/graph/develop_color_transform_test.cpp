//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/develop_color_transform.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <utility>

#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "image/dng_camera_matrix.hpp"
#include "imported_camera_profile_fixtures.hpp"
#include "json.hpp"
#include "test_camera_profile.hpp"

namespace alcedo {
namespace {

constexpr double kLowCct  = 2856.0;
constexpr double kHighCct = 6504.0;

auto DualIlluminantPayload() -> DevelopPayload {
  DevelopPayload payload;
  payload.wb_mode     = "custom";
  payload.custom_cct  = 6504.0f;
  payload.custom_tint = 0.0f;
  gpu_dag_test::FillTestCameraProfile(payload.camera_profile);
  return payload;
}

auto Invert3x3(const std::array<double, 9>& m) -> std::array<double, 9> {
  const double a = m[0];
  const double b = m[1];
  const double c = m[2];
  const double d = m[3];
  const double e = m[4];
  const double f = m[5];
  const double g = m[6];
  const double h = m[7];
  const double i = m[8];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  const double inv = 1.0 / det;
  return {(e * i - f * h) * inv, (c * h - b * i) * inv, (b * f - c * e) * inv,
          (f * g - d * i) * inv, (a * i - c * g) * inv, (c * d - a * f) * inv,
          (d * h - e * g) * inv, (b * g - a * h) * inv, (a * e - b * d) * inv};
}

auto MeanMatrix(const std::array<double, 9>& a, const std::array<double, 9>& b)
    -> std::array<double, 9> {
  std::array<double, 9> mean{};
  for (int i = 0; i < 9; ++i) {
    mean[static_cast<std::size_t>(i)] = 0.5 * (a[static_cast<std::size_t>(i)] + b[static_cast<std::size_t>(i)]);
  }
  return mean;
}

void ExpectMatrixNear(const std::array<float, 9>& actual, const std::array<double, 9>& expected,
                      double epsilon) {
  for (int i = 0; i < 9; ++i) {
    EXPECT_NEAR(actual[static_cast<std::size_t>(i)], expected[static_cast<std::size_t>(i)], epsilon)
        << "index " << i;
  }
}

auto CameraToXyzAt(DevelopPayload payload, float cct) -> ColorTransformResult {
  payload.wb_mode     = "custom";
  payload.custom_cct  = cct;
  payload.custom_tint = 0.0f;
  return ResolveDevelopColorTransform(payload);
}

TEST(GpuDagModelGraph, DevelopColorTransformInterpolatesDualIlluminantMatricesInMiredSpace) {
  auto payload = DualIlluminantPayload();
  const double mired_mid = 2.0 / (1.0 / kLowCct + 1.0 / kHighCct);
  const double kelvin_mid = 0.5 * (kLowCct + kHighCct);

  const auto low    = CameraToXyzAt(payload, static_cast<float>(kLowCct));
  const auto high   = CameraToXyzAt(payload, static_cast<float>(kHighCct));
  const auto mired  = CameraToXyzAt(payload, static_cast<float>(mired_mid));
  const auto kelvin = CameraToXyzAt(payload, static_cast<float>(kelvin_mid));
  ASSERT_TRUE(low.ok);
  ASSERT_TRUE(high.ok);
  ASSERT_TRUE(mired.ok);
  ASSERT_TRUE(kelvin.ok);

  ExpectMatrixNear(low.transform.camera_to_xyz, Invert3x3(payload.camera_profile.color_matrix_1),
                   1.0e-5);
  ExpectMatrixNear(high.transform.camera_to_xyz, Invert3x3(payload.camera_profile.color_matrix_2),
                   1.0e-5);
  ExpectMatrixNear(mired.transform.camera_to_xyz,
                   Invert3x3(MeanMatrix(payload.camera_profile.color_matrix_1,
                                        payload.camera_profile.color_matrix_2)),
                   1.0e-5);

  double kelvin_delta = 0.0;
  for (int i = 0; i < 9; ++i) {
    kelvin_delta += std::abs(mired.transform.camera_to_xyz[static_cast<std::size_t>(i)] -
                             kelvin.transform.camera_to_xyz[static_cast<std::size_t>(i)]);
  }
  EXPECT_GT(kelvin_delta, 1.0e-4);
}

TEST(GpuDagModelGraph, DevelopColorTransformInterpolatesForwardMatricesWithTheSameWeight) {
  auto payload = DualIlluminantPayload();
  payload.camera_profile.forward_matrices_valid = true;
  payload.camera_profile.forward_matrix_1 = {0.80, 0.10, 0.05, 0.12, 0.78, -0.04, 0.02, -0.11, 0.95};
  payload.camera_profile.forward_matrix_2 = {0.70, 0.16, 0.09, 0.08, 0.72, -0.08, 0.04, -0.18, 1.05};

  const double mired_mid = 2.0 / (1.0 / kLowCct + 1.0 / kHighCct);
  const auto without_fm = CameraToXyzAt(DualIlluminantPayload(), static_cast<float>(mired_mid));
  const auto with_fm    = CameraToXyzAt(payload, static_cast<float>(mired_mid));
  ASSERT_TRUE(without_fm.ok);
  ASSERT_TRUE(with_fm.ok);

  double d50_delta = 0.0;
  for (int i = 0; i < 9; ++i) {
    d50_delta += std::abs(with_fm.transform.camera_to_xyz_d50[static_cast<std::size_t>(i)] -
                          without_fm.transform.camera_to_xyz_d50[static_cast<std::size_t>(i)]);
  }
  EXPECT_GT(d50_delta, 1.0e-3);

  auto swapped = payload;
  swapped.camera_profile.color_matrix_1   = payload.camera_profile.color_matrix_2;
  swapped.camera_profile.color_matrix_2   = payload.camera_profile.color_matrix_1;
  swapped.camera_profile.forward_matrix_1 = payload.camera_profile.forward_matrix_2;
  swapped.camera_profile.forward_matrix_2 = payload.camera_profile.forward_matrix_1;
  swapped.camera_profile.color_matrix_1_cct = kHighCct;
  swapped.camera_profile.color_matrix_2_cct = kLowCct;
  const auto swapped_mid = CameraToXyzAt(swapped, static_cast<float>(mired_mid));
  ASSERT_TRUE(swapped_mid.ok);
  for (int i = 0; i < 9; ++i) {
    EXPECT_NEAR(with_fm.transform.camera_to_ap1[static_cast<std::size_t>(i)],
                swapped_mid.transform.camera_to_ap1[static_cast<std::size_t>(i)], 1.0e-5f);
  }
}

TEST(GpuDagModelGraph, DevelopColorTransformUsesSingleProfileWhenCalibrationIlluminantsAreIncomplete) {
  auto payload = DualIlluminantPayload();
  payload.camera_profile.calibration_illuminants_valid = false;
  const auto low  = CameraToXyzAt(payload, static_cast<float>(kLowCct));
  const auto high = CameraToXyzAt(payload, static_cast<float>(kHighCct));
  ASSERT_TRUE(low.ok);
  ASSERT_TRUE(high.ok);
  for (int i = 0; i < 9; ++i) {
    EXPECT_FLOAT_EQ(low.transform.camera_to_xyz[static_cast<std::size_t>(i)],
                    high.transform.camera_to_xyz[static_cast<std::size_t>(i)]);
  }
  ExpectMatrixNear(low.transform.camera_to_xyz, Invert3x3(payload.camera_profile.color_matrix_1),
                   1.0e-5);
}

TEST(GpuDagModelGraph, DevelopColorTransformSolvesAsShotNeutralWithoutUsingCamMulAsColorMatrix) {
  auto payload = DualIlluminantPayload();
  payload.wb_mode = "as_shot";
  payload.camera_profile.cam_mul = {4.0f, 1.0f, 0.25f};
  const auto first = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(first.ok);

  payload.camera_profile.cam_mul = {0.2f, 1.0f, 5.0f};
  const auto second = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(second.ok);
  for (int i = 0; i < 9; ++i) {
    EXPECT_FLOAT_EQ(first.transform.camera_to_ap1[static_cast<std::size_t>(i)],
                    second.transform.camera_to_ap1[static_cast<std::size_t>(i)]);
  }

  bool diagonal = true;
  for (int i = 0; i < 9; ++i) {
    const float value = first.transform.camera_to_ap1[static_cast<std::size_t>(i)];
    if (i % 4 != 0 && std::abs(value) > 1.0e-4f) {
      diagonal = false;
    }
  }
  EXPECT_FALSE(diagonal);
}

TEST(GpuDagModelGraph, DevelopColorTransformDoesNotUseLibRawRgbCamOrPreMulAsCameraMatrix) {
  RawRuntimeColorContext imported;
  imported.valid_                           = true;
  imported.color_matrices_valid_            = true;
  imported.calibration_illuminants_valid_   = true;
  imported.as_shot_neutral_valid_           = true;
  imported.color_matrix_1_cct_              = kLowCct;
  imported.color_matrix_2_cct_              = kHighCct;
  imported.as_shot_neutral_[0]              = 0.45;
  imported.as_shot_neutral_[1]              = 1.0;
  imported.as_shot_neutral_[2]              = 0.62;
  imported.cam_mul_[0]                      = 2.0f;
  imported.cam_mul_[1]                      = 1.0f;
  imported.cam_mul_[2]                      = 1.5f;
  const auto profile = DualIlluminantPayload().camera_profile;
  for (int i = 0; i < 9; ++i) {
    imported.color_matrix_1_[i] = profile.color_matrix_1[static_cast<std::size_t>(i)];
    imported.color_matrix_2_[i] = profile.color_matrix_2[static_cast<std::size_t>(i)];
    imported.rgb_cam_[i]        = 9.0f;
    imported.pre_mul_[i % 3]    = 8.0f;
  }

  DevelopPayload first;
  first.wb_mode     = "custom";
  first.custom_cct  = 5000.0f;
  BindDevelopCameraProfile(first, imported);
  const auto a = ResolveDevelopColorTransform(first);
  ASSERT_TRUE(a.ok);

  for (int i = 0; i < 9; ++i) {
    imported.rgb_cam_[i] = -3.0f;
  }
  imported.pre_mul_[0] = 0.1f;
  imported.pre_mul_[1] = 4.0f;
  imported.pre_mul_[2] = 0.2f;
  DevelopPayload second;
  second.wb_mode    = "custom";
  second.custom_cct = 5000.0f;
  BindDevelopCameraProfile(second, imported);
  const auto b = ResolveDevelopColorTransform(second);
  ASSERT_TRUE(b.ok);
  for (int i = 0; i < 9; ++i) {
    EXPECT_FLOAT_EQ(a.transform.camera_to_ap1[static_cast<std::size_t>(i)],
                    b.transform.camera_to_ap1[static_cast<std::size_t>(i)]);
  }
  EXPECT_NE(a.transform.camera_to_ap1[0], 1.0f);
  EXPECT_NE(a.transform.camera_to_ap1[1], 0.0f);
}

TEST(GpuDagModelGraph, DevelopColorTransformRejectsMissingOrSingularCameraMatrices) {
  DevelopPayload missing;
  const auto missing_result = ResolveDevelopColorTransform(missing);
  EXPECT_FALSE(missing_result.ok);
  EXPECT_EQ(missing_result.error, ColorTransformError::MissingCameraMatrices);

  auto singular = DualIlluminantPayload();
  singular.camera_profile.color_matrix_1 = {};
  singular.camera_profile.color_matrix_2 = {};
  const auto singular_result = ResolveDevelopColorTransform(singular);
  EXPECT_FALSE(singular_result.ok);
  EXPECT_EQ(singular_result.error, ColorTransformError::SingularCameraMatrix);
}

TEST(GpuDagModelGraph, DefaultPipelineDocumentStillRequiresBoundCameraProfile) {
  auto document = CreateDefaultPipelineDocument();
  ASSERT_NE(document.Develop(), nullptr);
  const auto result = ResolveDevelopColorTransform(document.Develop()->Params().Params());
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, ColorTransformError::MissingCameraMatrices);
}

TEST(GpuDagModelGraph, BindRgbWorkingSpaceCameraProfileResolvesWithoutRawContext) {
  DevelopPayload payload;
  EXPECT_FALSE(ResolveDevelopColorTransform(payload).ok);
  BindRgbWorkingSpaceCameraProfile(payload);
  EXPECT_TRUE(payload.camera_profile.color_matrices_valid);
  EXPECT_FALSE(payload.camera_profile.calibration_illuminants_valid);
  EXPECT_NEAR(payload.camera_profile.color_matrix_1[0], 3.2404542, 1e-6);
  EXPECT_NEAR(payload.camera_profile.color_matrix_2[0], 3.2404542, 1e-6);
  const auto result = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(result.ok);
  EXPECT_NE(result.transform.camera_to_ap1[0], 1.0f);
}

TEST(GpuDagModelGraph, DevelopCameraProfileJsonRoundTripPreservesMatricesAndAsShotNeutral) {
  auto document = CreateDefaultPipelineDocument();
  auto payload  = DualIlluminantPayload();
  payload.as_shot_cct  = 5123.0f;
  payload.as_shot_tint = -12.5f;
  document.Develop()->Params().ReplaceParams(payload);

  const auto restored = PipelineDocument::FromJson(document.ToJson());
  ASSERT_NE(restored.Develop(), nullptr);
  const auto roundtrip = restored.Develop()->Params().Params();
  EXPECT_EQ(roundtrip.camera_profile, payload.camera_profile);
  EXPECT_FLOAT_EQ(roundtrip.as_shot_cct, payload.as_shot_cct);
  EXPECT_FLOAT_EQ(roundtrip.as_shot_tint, payload.as_shot_tint);
  EXPECT_EQ(roundtrip.wb_mode, payload.wb_mode);
}

TEST(GpuDagModelGraph, BindDevelopCameraProfileWritesAsShotCctFromStoredNeutral) {
  RawRuntimeColorContext imported;
  imported.valid_                         = true;
  imported.color_matrices_valid_          = true;
  imported.calibration_illuminants_valid_ = true;
  imported.as_shot_neutral_valid_         = true;
  imported.color_matrix_1_cct_            = kLowCct;
  imported.color_matrix_2_cct_            = kHighCct;
  imported.as_shot_neutral_[0]            = 0.45;
  imported.as_shot_neutral_[1]            = 1.0;
  imported.as_shot_neutral_[2]            = 0.62;
  const auto profile = DualIlluminantPayload().camera_profile;
  for (int i = 0; i < 9; ++i) {
    imported.color_matrix_1_[i] = profile.color_matrix_1[static_cast<std::size_t>(i)];
    imported.color_matrix_2_[i] = profile.color_matrix_2[static_cast<std::size_t>(i)];
  }

  auto document = CreateDefaultPipelineDocument();
  gpu_dag_test::ApplyImportedCameraProfile(document, imported);
  const auto bound = document.Develop()->Params().Params();
  EXPECT_TRUE(bound.camera_profile.color_matrices_valid);
  EXPECT_GE(bound.as_shot_cct, 2000.0f);
  EXPECT_LE(bound.as_shot_cct, 15000.0f);
  EXPECT_GE(bound.as_shot_tint, -150.0f);
  EXPECT_LE(bound.as_shot_tint, 150.0f);
}

TEST(GpuDagModelGraph, DevelopColorTransformSolvesDaylightCctWhenAnalogBalanceIsFoldedIntoColorMatrix) {
  auto payload = DualIlluminantPayload();
  payload.wb_mode = "as_shot";
  payload.camera_profile.as_shot_neutral_valid = true;
  payload.camera_profile.as_shot_neutral       = {1.0, 0.999595, 0.998996};
  payload.camera_profile.forward_matrices_valid = false;
  payload.camera_profile.color_matrix_1 = {0.8784, -0.4791, 0.1177, -0.3468, 1.0693, 0.3213,
                                           0.0009, 0.0507, 0.7395};
  payload.camera_profile.color_matrix_2 = {0.746, -0.2365, -0.0588, -0.5687, 1.3442, 0.2474,
                                           -0.0624, 0.1156, 0.6584};

  const auto without_analog = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(without_analog.ok);
  EXPECT_LT(without_analog.transform.resolved_cct, 3000.0f);
  EXPECT_GT(std::abs(without_analog.transform.resolved_tint), 50.0f);

  const double analog_balance[3]     = {2.411133, 1.0, 1.62793};
  const double camera_calibration[9] = {1.0008, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.9523};
  ApplyAnalogBalanceAndCameraCalibration(payload.camera_profile.color_matrix_1.data(),
                                         analog_balance, camera_calibration);
  ApplyAnalogBalanceAndCameraCalibration(payload.camera_profile.color_matrix_2.data(),
                                         analog_balance, camera_calibration);

  const auto with_analog = ResolveDevelopColorTransform(payload);
  ASSERT_TRUE(with_analog.ok);
  EXPECT_GT(with_analog.transform.resolved_cct, 4800.0f);
  EXPECT_LT(with_analog.transform.resolved_cct, 6200.0f);
  EXPECT_LT(std::abs(with_analog.transform.resolved_tint), 40.0f);
}

}  // namespace

TEST(GpuDagModelGraph, BindImportedCameraProfileMatchesPreviousDevelopParameters) {
  for (const auto& fixture : gpu_dag_test::MakeImportedCameraProfileFixtures()) {
    SCOPED_TRACE(fixture.name_);
    std::ifstream stream(std::filesystem::path(ALCEDO_EXPECTED_JSON_ROOT) / fixture.expected_file_);
    ASSERT_TRUE(stream.is_open()) << fixture.expected_file_;
    const auto expected = nlohmann::json::parse(stream);

    auto       document = CreateDefaultPipelineDocument();
    BindImportedCameraProfile(document, fixture.context_);
    EXPECT_EQ(document.Develop()->Params().ToJson(), expected);
  }
}

TEST(GpuDagModelGraph, BindImportedCameraProfileKeepsUserEditsAndSkipsEqualWrite) {
  const auto fixtures = gpu_dag_test::MakeImportedCameraProfileFixtures();
  auto       document = CreateDefaultPipelineDocument();
  auto       payload  = document.Develop()->Params().Params();
  payload.wb_mode     = "custom";
  payload.custom_cct  = 4321.0f;
  payload.lens_maker  = "Fixture Lens Co";
  document.Develop()->Params().ReplaceParams(payload);

  BindImportedCameraProfile(document, fixtures.front().context_);
  const auto bound = document.Develop()->Params().Params();
  EXPECT_EQ(bound.wb_mode, "custom");
  EXPECT_FLOAT_EQ(bound.custom_cct, 4321.0f);
  EXPECT_EQ(bound.lens_maker, "Fixture Lens Co");
  EXPECT_TRUE(bound.camera_profile.color_matrices_valid);

  (void)document.Develop()->Params().TakeDirtyFields();
  BindImportedCameraProfile(document, fixtures.front().context_);
  EXPECT_FALSE(document.Develop()->Params().IsDirty());
  EXPECT_EQ(document.Develop()->Params().Params(), bound);
}

namespace {

auto Ap1ToXyzY(const std::array<double, 3>& rgb) -> double {
  return 0.2722287168 * rgb[0] + 0.6740817658 * rgb[1] + 0.0536895174 * rgb[2];
}

auto XyToAp1(const std::array<double, 2>& xy) -> std::array<double, 3> {
  const double x = xy[0] / xy[1];
  const double z = (1.0 - xy[0] - xy[1]) / xy[1];
  return {1.6410233797 * x - 0.3248032942 - 0.2364246952 * z,
          -0.6636628587 * x + 1.6153315917 + 0.0167563477 * z,
          0.0117218943 * x - 0.0082844420 + 0.9883948585 * z};
}

auto Apply(const std::array<float, 9>& m, const std::array<double, 3>& rgb)
    -> std::array<double, 3> {
  return {m[0] * rgb[0] + m[1] * rgb[1] + m[2] * rgb[2],
          m[3] * rgb[0] + m[4] * rgb[1] + m[5] * rgb[2],
          m[6] * rgb[0] + m[7] * rgb[1] + m[8] * rgb[2]};
}

}  // namespace

TEST(GradeCat02WhiteBalanceTest, AcesWhiteCctTintMatchesModelDefault) {
  const auto& white = AcesWhiteTemperatureTint();
  EXPECT_NEAR(white.cct, 6000.0, 1.0);
  EXPECT_NEAR(white.cct, kCat02DefaultTemperature, 1.0e-2);
  EXPECT_NEAR(white.tint, kCat02DefaultTint, 1.0e-3);
  const auto xy = WhiteBalanceTemperatureTintToXy(white.cct, white.tint);
  EXPECT_NEAR(xy[0], kAcesWhiteXy[0], 1.0e-5);
  EXPECT_NEAR(xy[1], kAcesWhiteXy[1], 1.0e-5);
}

TEST(GradeCat02WhiteBalanceTest, MatrixAtAcesWhiteIsIdentity) {
  const auto m = BuildAp1Cat02WhiteBalanceMatrix(kCat02DefaultTemperature, kCat02DefaultTint);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      EXPECT_NEAR(m[static_cast<std::size_t>(r * 3 + c)], r == c ? 1.0f : 0.0f, 1.0e-4f)
          << r << "," << c;
    }
  }
}

TEST(GradeCat02WhiteBalanceTest, MatrixNeutralizesSelectedIlluminantToAp1White) {
  for (const auto& [cct, tint] : {std::pair{3200.0, 0.0}, std::pair{4500.0, -25.0},
                                  std::pair{9000.0, 30.0}}) {
    const auto illuminant = XyToAp1(WhiteBalanceTemperatureTintToXy(cct, tint));
    const auto adapted    = Apply(BuildAp1Cat02WhiteBalanceMatrix(cct, tint), illuminant);
    const double luminance = Ap1ToXyzY(adapted);
    EXPECT_NEAR(adapted[0] / luminance, 1.0, 1.0e-4) << cct << " " << tint;
    EXPECT_NEAR(adapted[1] / luminance, 1.0, 1.0e-4) << cct << " " << tint;
    EXPECT_NEAR(adapted[2] / luminance, 1.0, 1.0e-4) << cct << " " << tint;
  }
}

TEST(GradeCat02WhiteBalanceTest, HigherTemperatureWarmsAp1NeutralLikeRawCustomWb) {
  const std::array<double, 3> grey{0.18, 0.18, 0.18};
  const auto warm = Apply(BuildAp1Cat02WhiteBalanceMatrix(8000.0, kCat02DefaultTint), grey);
  const auto cool = Apply(BuildAp1Cat02WhiteBalanceMatrix(4000.0, kCat02DefaultTint), grey);
  EXPECT_GT(warm[0], warm[2]);
  EXPECT_LT(cool[0], cool[2]);
}

TEST(GradeCat02WhiteBalanceTest, ModelDefaultsToAcesWhiteAndClampsToRawWbRanges) {
  Cat02WhiteBalanceModel model;
  EXPECT_TRUE(model.IsDefault());
  EXPECT_FLOAT_EQ(model.Temperature(), kCat02DefaultTemperature);
  EXPECT_FLOAT_EQ(model.Tint(), kCat02DefaultTint);
  model.ApplyUpdate(Cat02WhiteBalanceUpdate{std::nullopt, 50000.0f, -400.0f});
  EXPECT_FLOAT_EQ(model.Temperature(), kCat02TemperatureMax);
  EXPECT_FLOAT_EQ(model.Tint(), kCat02TintMin);
  EXPECT_FALSE(model.IsDefault());
  EXPECT_THROW(model.SetTemperature(std::numeric_limits<float>::quiet_NaN()),
               std::invalid_argument);
}

TEST(GradeCat02WhiteBalanceTest, JsonWithoutTemperatureTintLoadsAcesWhiteDefault) {
  Cat02WhiteBalanceModel model;
  model.ApplyUpdate(Cat02WhiteBalanceUpdate{std::nullopt, 3000.0f, 40.0f});
  model.LoadJson(nlohmann::json{{"enabled", true}, {"temperature_offset", 0.0}, {"tint_offset", 0.0}});
  EXPECT_TRUE(model.IsDefault());

  Cat02WhiteBalanceModel copy;
  model.ApplyUpdate(Cat02WhiteBalanceUpdate{false, 4100.0f, -12.0f});
  copy.LoadJson(model.ToJson());
  EXPECT_FALSE(copy.Enabled());
  EXPECT_FLOAT_EQ(copy.Temperature(), 4100.0f);
  EXPECT_FLOAT_EQ(copy.Tint(), -12.0f);
}

}  // namespace alcedo
