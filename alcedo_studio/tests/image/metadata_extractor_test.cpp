//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "edit/operators/basic/color_temp_op.hpp"
#include "image/dng_camera_matrix.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"
#include "utils/import/import_error_code.hpp"

namespace alcedo {
namespace {

auto BadDngSamplePath() -> std::filesystem::path {
  return std::filesystem::path(TEST_IMG_PATH) / "raw" / "bad_dng" / "bad_color_dng.dng";
}

auto HasselbladX2dSamplePath() -> std::filesystem::path {
  return std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "hasselblad" / "x2d" /
         "B0004841.dng";
}

auto SonyCctRegressionSamplePath() -> std::filesystem::path {
  return std::filesystem::path(TEST_IMG_PATH) / "raw" / "cct_test" / "_DSC8085.ARW";
}

auto SonyA7CiiConvertedDngPath() -> std::filesystem::path {
  return std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "sony" / "a7cii" /
         "ycbcr_compressed" / "DSC04739_dng.dng";
}

auto ResolveAsShotColorTemp(const RawRuntimeColorContext& ctx) -> OperatorParams {
  OperatorParams params;
  params.color_temp_enabled_ = true;
  params.PopulateRawMetadata(ctx);
  const nlohmann::json color_temp_params = {
      {"color_temp", {{"mode", "as_shot"}, {"cct", 6500.0}, {"tint", 0.0}}}};
  ColorTempOp op(color_temp_params);
  op.SetGlobalParams(params);
  return params;
}

void ExpectMatrixNear(const double* actual, const double (&expected)[9], const double epsilon) {
  ASSERT_NE(actual, nullptr);
  for (int i = 0; i < 9; ++i) {
    EXPECT_NEAR(actual[i], expected[i], epsilon) << "matrix index " << i;
  }
}

TEST(MetadataExtractorTest, AnalogBalanceAndCameraCalibrationComposeInDngSpecOrder) {
  double color_matrix[9] = {2.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 4.0};
  const double analog_balance[3]              = {2.0, 1.0, 0.5};
  const double camera_calibration[9]          = {1.5, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 2.0};
  ApplyAnalogBalanceAndCameraCalibration(color_matrix, analog_balance, camera_calibration);
  static constexpr double kExpected[9] = {6.0, 0.0, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 4.0};
  ExpectMatrixNear(color_matrix, kExpected, 1e-12);
}

TEST(MetadataExtractorTest, InvalidAnalogBalanceLeavesColorMatrixUnscaled) {
  double color_matrix[9] = {2.0, 0.1, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 4.0};
  const double analog_balance[3]     = {2.0, 0.0, 0.5};
  const double camera_calibration[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  ApplyAnalogBalanceAndCameraCalibration(color_matrix, analog_balance, camera_calibration);
  static constexpr double kExpected[9] = {2.0, 0.1, 0.0, 0.0, 3.0, 0.0, 0.0, 0.0, 4.0};
  ExpectMatrixNear(color_matrix, kExpected, 1e-12);
}

TEST(MetadataExtractorTest, CameraCalibrationSignaturesMustMatchToApply) {
  EXPECT_TRUE(CameraCalibrationSignaturesMatch("com.adobe", "com.adobe"));
  EXPECT_TRUE(CameraCalibrationSignaturesMatch("", ""));
  EXPECT_FALSE(CameraCalibrationSignaturesMatch("com.adobe", ""));
  EXPECT_FALSE(CameraCalibrationSignaturesMatch("", "com.adobe"));
}

TEST(MetadataExtractorTest, DngImportUsesEmbeddedColorMatrices) {
  const auto sample_path = BadDngSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image image(1, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  const auto& ctx = image.GetRawColorContext();
  EXPECT_TRUE(ctx.valid_);
  EXPECT_TRUE(ctx.color_matrices_valid_);
  EXPECT_TRUE(ctx.forward_matrices_valid_);
  EXPECT_TRUE(ctx.as_shot_neutral_valid_);
  EXPECT_TRUE(ctx.calibration_illuminants_valid_);
  EXPECT_EQ(ctx.camera_make_, "Phase One");
  EXPECT_EQ(ctx.camera_model_, "IQ4 150MP");
  EXPECT_NEAR(ctx.as_shot_neutral_[0], 0.30864197, 1e-6);
  EXPECT_NEAR(ctx.as_shot_neutral_[1], 1.0, 1e-6);
  EXPECT_NEAR(ctx.as_shot_neutral_[2], 0.6578947305, 1e-6);
  EXPECT_NEAR(ctx.color_matrix_1_cct_, 5503.0, 1e-6);
  EXPECT_NEAR(ctx.color_matrix_2_cct_, 7504.0, 1e-6);

  static constexpr double kExpectedCm1[9] = {
      0.3100999889, 0.0, 0.0,
      0.0, 1.0, 0.0,
      0.0, 0.0, 0.8062999844,
  };
  static constexpr double kExpectedCm2[9] = {
      0.2423000034, 0.0, 0.0,
      0.0, 0.9901000261, 0.0,
      0.0, 0.0, 0.7989000081,
  };
  static constexpr double kExpectedFm[9] = {
      0.8295999765, 0.0538000013, 0.08089999812,
      0.3136999902, 0.8399000167, -0.1536000069,
      0.0132999993, -0.4061000046, 1.217900038,
  };

  ExpectMatrixNear(ctx.color_matrix_1_, kExpectedCm1, 1e-4);
  ExpectMatrixNear(ctx.color_matrix_2_, kExpectedCm2, 1e-4);
  ExpectMatrixNear(ctx.forward_matrix_1_, kExpectedFm, 1e-4);
  ExpectMatrixNear(ctx.forward_matrix_2_, kExpectedFm, 1e-4);
}

TEST(MetadataExtractorTest, RawColorContextSurvivesExifJsonRoundTrip) {
  const auto sample_path = BadDngSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image source(2, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, source));
  ASSERT_TRUE(source.HasRawColorContext());

  const std::string persisted = source.ExifToJson();

  Image restored(3, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(restored.JsonToExif(persisted));
  ASSERT_TRUE(restored.HasRawColorContext());

  const auto& original = source.GetRawColorContext();
  const auto& roundtrip = restored.GetRawColorContext();
  EXPECT_EQ(roundtrip.camera_make_, original.camera_make_);
  EXPECT_EQ(roundtrip.camera_model_, original.camera_model_);
  EXPECT_EQ(roundtrip.color_matrices_valid_, original.color_matrices_valid_);
  EXPECT_EQ(roundtrip.forward_matrices_valid_, original.forward_matrices_valid_);
  EXPECT_EQ(roundtrip.as_shot_neutral_valid_, original.as_shot_neutral_valid_);
  EXPECT_EQ(roundtrip.calibration_illuminants_valid_, original.calibration_illuminants_valid_);
  EXPECT_DOUBLE_EQ(roundtrip.color_matrix_1_cct_, original.color_matrix_1_cct_);
  EXPECT_DOUBLE_EQ(roundtrip.color_matrix_2_cct_, original.color_matrix_2_cct_);

  for (int i = 0; i < 9; ++i) {
    EXPECT_DOUBLE_EQ(roundtrip.color_matrix_1_[i], original.color_matrix_1_[i]);
    EXPECT_DOUBLE_EQ(roundtrip.color_matrix_2_[i], original.color_matrix_2_[i]);
    EXPECT_DOUBLE_EQ(roundtrip.forward_matrix_1_[i], original.forward_matrix_1_[i]);
    EXPECT_DOUBLE_EQ(roundtrip.forward_matrix_2_[i], original.forward_matrix_2_[i]);
  }
  for (int i = 0; i < 3; ++i) {
    EXPECT_DOUBLE_EQ(roundtrip.as_shot_neutral_[i], original.as_shot_neutral_[i]);
  }
}

TEST(MetadataExtractorTest, ColorTempOpSupportsDngWithoutCamXyzWhenDngMetadataIsPresent) {
  const auto sample_path = BadDngSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image image(4, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  OperatorParams params;
  params.color_temp_enabled_ = true;
  params.PopulateRawMetadata(image.GetRawColorContext());

  for (float& value : params.raw_cam_xyz_) {
    value = 0.0f;
  }

  const nlohmann::json color_temp_params = {
      {"color_temp", {{"mode", "as_shot"}, {"cct", 6500.0}, {"tint", 0.0}}}};
  ColorTempOp op(color_temp_params);
  ASSERT_NO_THROW(op.SetGlobalParams(params));

  EXPECT_TRUE(params.color_temp_matrices_valid_);
  EXPECT_GT(params.color_temp_resolved_xy_[0], 0.0f);
  EXPECT_GT(params.color_temp_resolved_xy_[1], 0.0f);
  EXPECT_GE(params.color_temp_resolved_cct_, 2000.0f);
  EXPECT_LE(params.color_temp_resolved_cct_, 15000.0f);
}

TEST(MetadataExtractorTest, ColorTempOpUsesForwardMatrixForBadColorDngAsShot) {
  const auto sample_path = BadDngSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image image(5, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  OperatorParams params;
  params.color_temp_enabled_ = true;
  params.PopulateRawMetadata(image.GetRawColorContext());

  const nlohmann::json color_temp_params = {
      {"color_temp", {{"mode", "as_shot"}, {"cct", 6500.0}, {"tint", 0.0}}}};
  ColorTempOp op(color_temp_params);
  ASSERT_NO_THROW(op.SetGlobalParams(params));

  EXPECT_TRUE(params.color_temp_matrices_valid_);
  EXPECT_GT(params.color_temp_resolved_cct_, 4000.0f);
  EXPECT_LT(params.color_temp_resolved_cct_, 6000.0f);
  EXPECT_GT(std::abs(params.color_temp_cam_to_xyz_d50_[1]), 0.01f);
  EXPECT_GT(std::abs(params.color_temp_cam_to_xyz_d50_[3]), 0.1f);
  EXPECT_GT(std::abs(params.color_temp_cam_to_xyz_d50_[7]), 0.1f);

  static constexpr float kExpectedCameraToXyzD50[9] = {
      2.6879040f, 0.0538000f, 0.1229680f,
      1.0163880f, 0.8399000f, -0.2334720f,
      0.0430920f, -0.4061000f, 1.8512081f,
  };

  for (int i = 0; i < 9; ++i) {
    EXPECT_NEAR(params.color_temp_cam_to_xyz_d50_[i], kExpectedCameraToXyzD50[i], 1e-3f)
        << "matrix index " << i;
  }
}

// Hasselblad X2D HueSatMap tables are authored for ColorMatrix + CAT. Adobe Standard
// DNGs also embed those tables but still require ForwardMatrix; only Hasselblad drops FM.
TEST(MetadataExtractorTest, EmbeddedDngProfileTablesDisableHasselbladForwardMatrix) {
  const auto sample_path = HasselbladX2dSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image image(6, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  const auto& ctx = image.GetRawColorContext();
  EXPECT_TRUE(ctx.color_matrices_valid_);
  EXPECT_FALSE(ctx.forward_matrices_valid_);
  EXPECT_TRUE(ctx.calibration_illuminants_valid_);
  EXPECT_EQ(ctx.camera_make_, "Hasselblad");
  EXPECT_EQ(ctx.camera_model_, "X2D 100C-100c");

  OperatorParams params;
  params.color_temp_enabled_ = true;
  params.PopulateRawMetadata(ctx);

  const nlohmann::json color_temp_params = {
      {"color_temp", {{"mode", "as_shot"}, {"cct", 6500.0}, {"tint", 0.0}}}};
  ColorTempOp op(color_temp_params);
  ASSERT_NO_THROW(op.SetGlobalParams(params));

  EXPECT_TRUE(params.color_temp_matrices_valid_);
  EXPECT_GT(params.color_temp_resolved_cct_, 4900.0f);
  EXPECT_LT(params.color_temp_resolved_cct_, 5050.0f);
  EXPECT_GT(params.color_temp_cam_to_xyz_d50_[0], 1.5f);
  EXPECT_LT(params.color_temp_cam_to_xyz_d50_[2], 0.05f);
  EXPECT_LT(params.color_temp_cam_to_xyz_d50_[5], -0.2f);
  EXPECT_LT(params.color_temp_cam_to_xyz_d50_[7], -0.1f);
}

TEST(MetadataExtractorTest, SonyArwMakerNoteAsShotNeutralResolvesStableCct) {
  const auto sample_path = SonyCctRegressionSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample ARW not found: " << sample_path.string();
  }

  Image image(7, sample_path, ImageType::ARW);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  const auto& ctx = image.GetRawColorContext();
  ASSERT_TRUE(ctx.valid_);
  ASSERT_TRUE(ctx.as_shot_neutral_valid_);
  EXPECT_NEAR(ctx.cam_mul_[0], 1848.0f, 1e-3f);
  EXPECT_NEAR(ctx.cam_mul_[1], 1024.0f, 1e-3f);
  EXPECT_NEAR(ctx.cam_mul_[2], 2004.0f, 1e-3f);
  EXPECT_NEAR(ctx.as_shot_neutral_[0], 1024.0 / 1848.0, 1e-6);
  EXPECT_DOUBLE_EQ(ctx.as_shot_neutral_[1], 1.0);
  EXPECT_NEAR(ctx.as_shot_neutral_[2], 1024.0 / 2004.0, 1e-6);

  OperatorParams params;
  params.color_temp_enabled_ = true;
  params.PopulateRawMetadata(ctx);

  const nlohmann::json color_temp_params = {
      {"color_temp", {{"mode", "as_shot"}, {"cct", 6500.0}, {"tint", 0.0}}}};
  ColorTempOp op(color_temp_params);
  ASSERT_NO_THROW(op.SetGlobalParams(params));

  EXPECT_TRUE(params.color_temp_matrices_valid_);
  EXPECT_GT(params.color_temp_resolved_cct_, 3900.0f);
  EXPECT_LT(params.color_temp_resolved_cct_, 4050.0f);
  EXPECT_GT(params.color_temp_resolved_tint_, -20.0f);
  EXPECT_LT(params.color_temp_resolved_tint_, 0.0f);
}

TEST(MetadataExtractorTest, SonyAdobeDngFoldsAnalogBalanceIntoColorMatrixAndKeepsForwardMatrix) {
  const auto sample_path = SonyA7CiiConvertedDngPath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image image(8, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  const auto& ctx = image.GetRawColorContext();
  ASSERT_TRUE(ctx.color_matrices_valid_);
  ASSERT_TRUE(ctx.forward_matrices_valid_);
  ASSERT_TRUE(ctx.as_shot_neutral_valid_);
  EXPECT_NEAR(ctx.as_shot_neutral_[0], 1.0, 0.01);
  EXPECT_NEAR(ctx.as_shot_neutral_[1], 1.0, 0.01);
  EXPECT_NEAR(ctx.as_shot_neutral_[2], 1.0, 0.01);

  static constexpr double kUnbakedCm1[9] = {
      0.8784, -0.4791, 0.1177, -0.3468, 1.0693, 0.3213, 0.0009, 0.0507, 0.7395,
  };
  static constexpr double kAdobeFm1[9] = {
      0.4743, 0.3796, 0.1104, 0.2023, 0.7673, 0.0304, 0.0553, 0.0008, 0.769,
  };
  static constexpr double kAdobeFm2[9] = {
      0.5465, 0.2614, 0.1563, 0.3232, 0.6292, 0.0475, 0.1339, 0.0025, 0.6887,
  };
  ExpectMatrixNear(ctx.forward_matrix_1_, kAdobeFm1, 1e-3);
  ExpectMatrixNear(ctx.forward_matrix_2_, kAdobeFm2, 1e-3);

  const double analog_balance[3]     = {2.411133, 1.0, 1.62793};
  const double camera_calibration[9] = {1.0008, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.9523};
  double       expected_cm1[9]       = {
      0.8784, -0.4791, 0.1177, -0.3468, 1.0693, 0.3213, 0.0009, 0.0507, 0.7395,
  };
  ApplyAnalogBalanceAndCameraCalibration(expected_cm1, analog_balance, camera_calibration);
  ExpectMatrixNear(ctx.color_matrix_1_, expected_cm1, 1e-3);
  EXPECT_GT(ctx.color_matrix_1_[0], 1.8);
  EXPECT_GT(std::abs(ctx.color_matrix_1_[0] - kUnbakedCm1[0]), 0.5);

  const auto params = ResolveAsShotColorTemp(ctx);
  EXPECT_TRUE(params.color_temp_matrices_valid_);
  EXPECT_GT(params.color_temp_resolved_cct_, 4800.0f);
  EXPECT_LT(params.color_temp_resolved_cct_, 6200.0f);
  EXPECT_GT(params.color_temp_resolved_tint_, -40.0f);
  EXPECT_LT(params.color_temp_resolved_tint_, 40.0f);
}

TEST(MetadataExtractorTest, XmpSidecarIsRejectedAsUnsupportedImportFormat) {
  const auto dir = std::filesystem::temp_directory_path() / "alcedo_metadata_xmp_reject";
  std::filesystem::create_directories(dir);
  const auto xmp_path = dir / "sidecar.xmp";
  const auto xml_path = dir / "sidecar.xml";

  // Minimal packet Exiv2 recognizes as ImageType::xmp (metadata-only, 0x0).
  constexpr const char* kXmpPacket =
      "<?xpacket begin=\"\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
      "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
      " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
      "  <rdf:Description rdf:about=\"\"/>\n"
      " </rdf:RDF>\n"
      "</x:xmpmeta>\n"
      "<?xpacket end=\"w\"?>\n";

  {
    std::ofstream out(xmp_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out << kXmpPacket;
    ASSERT_TRUE(out.good());
  }
  {
    std::ofstream out(xml_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());
    out << kXmpPacket;
    ASSERT_TRUE(out.good());
  }

  for (const auto& path : {xmp_path, xml_path}) {
    Image image(99, path, ImageType::DEFAULT);
    try {
      MetadataExtractor::ExtractEXIF_ToImage(path, image);
      FAIL() << "Expected MetadataExtractionError for " << path.string();
    } catch (const MetadataExtractionError& e) {
      EXPECT_EQ(e.code(), ImportErrorCode::UNSUPPORTED_FORMAT) << path.string();
    }
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
}  // namespace alcedo
