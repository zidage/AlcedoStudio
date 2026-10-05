//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "image/metadata_extractor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "image/dng_camera_matrix.hpp"
#include "image/image.hpp"
#include "image/raster_color_description.hpp"
#include "support/non_raw_import_files.hpp"
#include "utils/import/import_error_code.hpp"

namespace alcedo {
namespace {

auto RasterFixturePath(const char* name) -> std::filesystem::path {
  return std::filesystem::path(ALCEDO_RASTER_FIXTURE_DIR) / name;
}

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

/// Bind the imported camera profile to a Develop payload in as-shot mode and resolve its
/// camera transform, as import and the GPU DAG Develop pass do.
auto ResolveAsShotDevelopTransform(const RawRuntimeColorContext& ctx) -> ColorTransformResult {
  DevelopPayload develop;
  BindDevelopCameraProfile(develop, ctx);
  develop.wb_mode = "as_shot";
  return ResolveDevelopColorTransform(develop);
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

TEST(MetadataExtractorTest,
     AsShotDevelopTransformSupportsDngWithoutCamXyzWhenDngMetadataIsPresent) {
  const auto sample_path = BadDngSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image image(4, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  RawRuntimeColorContext ctx = image.GetRawColorContext();
  for (float& value : ctx.cam_xyz_) {
    value = 0.0f;
  }

  const auto result = ResolveAsShotDevelopTransform(ctx);
  ASSERT_TRUE(result.ok) << ColorTransformErrorMessage(result.error);
  EXPECT_GE(result.transform.resolved_cct, 2000.0f);
  EXPECT_LE(result.transform.resolved_cct, 15000.0f);
}

TEST(MetadataExtractorTest, AsShotDevelopTransformUsesForwardMatrixForBadColorDng) {
  const auto sample_path = BadDngSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image image(5, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  const auto& ctx    = image.GetRawColorContext();
  const auto  result = ResolveAsShotDevelopTransform(ctx);
  ASSERT_TRUE(result.ok) << ColorTransformErrorMessage(result.error);
  const auto& cam_to_xyz_d50 = result.transform.camera_to_xyz_d50;
  EXPECT_GT(result.transform.resolved_cct, 4000.0f);
  EXPECT_LT(result.transform.resolved_cct, 6000.0f);
  EXPECT_GT(std::abs(cam_to_xyz_d50[1]), 0.01f);
  EXPECT_GT(std::abs(cam_to_xyz_d50[3]), 0.1f);
  EXPECT_GT(std::abs(cam_to_xyz_d50[7]), 0.1f);

  const double d50[3] = {.34567 / .35850, 1.0, (1 - .34567 - .35850) / .35850};
  const double maximum =
      std::max({ctx.as_shot_neutral_[0], ctx.as_shot_neutral_[1], ctx.as_shot_neutral_[2]});
  for (int r = 0; r < 3; ++r) {
    double neutral = 0;
    for (int c = 0; c < 3; ++c)
      neutral +=
          cam_to_xyz_d50[static_cast<std::size_t>(r * 3 + c)] * ctx.as_shot_neutral_[c] / maximum;
    EXPECT_NEAR(neutral, d50[r], 2e-4);
  }
}

// Hasselblad X2D HueSatMap tables are authored for ColorMatrix + CAT. Adobe Standard
// DNGs also embed those tables but still require ForwardMatrix; only Hasselblad drops FM.
TEST(MetadataExtractorTest, EmbeddedDngProfileTablesPreserveHasselbladForwardMatrix) {
  const auto sample_path = HasselbladX2dSamplePath();
  if (!std::filesystem::exists(sample_path)) {
    GTEST_SKIP() << "Sample DNG not found: " << sample_path.string();
  }

  Image image(6, sample_path, ImageType::DNG);
  ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(sample_path, image));
  ASSERT_TRUE(image.HasRawColorContext());

  const auto& ctx = image.GetRawColorContext();
  EXPECT_TRUE(ctx.color_matrices_valid_);
  EXPECT_TRUE(ctx.forward_matrices_valid_);
  ASSERT_TRUE(ctx.dng_profile_);
  EXPECT_FALSE(ctx.dng_profile_->hue_sat_map_1.entries.empty());
  EXPECT_TRUE(ctx.calibration_illuminants_valid_);
  EXPECT_EQ(ctx.camera_make_, "Hasselblad");
  EXPECT_EQ(ctx.camera_model_, "X2D 100C-100c");

  const auto result = ResolveAsShotDevelopTransform(ctx);
  ASSERT_TRUE(result.ok) << ColorTransformErrorMessage(result.error);
  const auto& cam_to_xyz_d50 = result.transform.camera_to_xyz_d50;
  EXPECT_GT(result.transform.resolved_cct, 4900.0f);
  EXPECT_LT(result.transform.resolved_cct, 5050.0f);
  const double d50[3] = {.34567 / .35850, 1.0, (1 - .34567 - .35850) / .35850};
  const double maximum =
      std::max({ctx.as_shot_neutral_[0], ctx.as_shot_neutral_[1], ctx.as_shot_neutral_[2]});
  for (int r = 0; r < 3; ++r) {
    double neutral = 0;
    for (int c = 0; c < 3; ++c)
      neutral +=
          cam_to_xyz_d50[static_cast<std::size_t>(r * 3 + c)] * ctx.as_shot_neutral_[c] / maximum;
    EXPECT_NEAR(neutral, d50[r], 2e-4);
  }
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

  const auto result = ResolveAsShotDevelopTransform(ctx);
  ASSERT_TRUE(result.ok) << ColorTransformErrorMessage(result.error);
  EXPECT_GT(result.transform.resolved_cct, 3900.0f);
  EXPECT_LT(result.transform.resolved_cct, 4050.0f);
  EXPECT_GT(result.transform.resolved_tint, -20.0f);
  EXPECT_LT(result.transform.resolved_tint, 0.0f);
}

TEST(MetadataExtractorTest, SonyAdobeDngPreservesTaggedMatrixAndSeparateAnalogBalance) {
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
  ExpectMatrixNear(ctx.color_matrix_1_, kUnbakedCm1, 1e-6);
  ASSERT_TRUE(ctx.dng_profile_);
  for (int i = 0; i < 3; ++i)
    EXPECT_NEAR(ctx.dng_profile_->analog_balance[i], analog_balance[i], 1e-5);
  for (int i = 0; i < 9; ++i)
    EXPECT_NEAR(ctx.dng_profile_->camera_calibration_1[i], camera_calibration[i], 1e-5);
  const auto result = ResolveAsShotDevelopTransform(ctx);
  ASSERT_TRUE(result.ok) << ColorTransformErrorMessage(result.error);
  EXPECT_GT(result.transform.resolved_cct, 4800.0f);
  EXPECT_LT(result.transform.resolved_cct, 6200.0f);
  EXPECT_GT(result.transform.resolved_tint, -40.0f);
  EXPECT_LT(result.transform.resolved_tint, 40.0f);
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

// Import decides RAW or raster from the content (raster_image_input_plan.md, section 8):
// rasters import whatever their extension says, with a color description and no RAW context.
TEST(MetadataExtractorTest, RasterFilesImportByContentWhateverTheExtension) {
  const auto dir = std::filesystem::temp_directory_path() / "alcedo_metadata_raster_import";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  struct RasterCase {
    std::string file_name_;
    std::string encoded_as_;
    ImageType   type_;
  };
  const std::vector<RasterCase> cases = {{"photo.jpg", ".jpg", ImageType::JPEG},
                                         {"scan.tif", ".tif", ImageType::TIFF},
                                         {"graphic.png", ".png", ImageType::PNG},
                                         {"renamed_jpeg.nef", ".jpg", ImageType::JPEG},
                                         {"renamed_tiff.dng", ".tif", ImageType::TIFF}};
  for (const auto& raster : cases) {
    const auto path = dir / raster.file_name_;
    test_support::WriteRgbRaster(path, raster.encoded_as_);
    Image image(7, path, ImageType::DEFAULT);
    ASSERT_NO_THROW(MetadataExtractor::ExtractEXIF_ToImage(path, image)) << raster.file_name_;
    EXPECT_EQ(image.image_type_, raster.type_) << raster.file_name_;
    EXPECT_FALSE(image.HasRawColorContext()) << raster.file_name_;
    ASSERT_TRUE(image.HasRasterColorDescription()) << raster.file_name_;
    // The writer carries no color tag, except that some PNG writers (libpng on macOS) add an
    // sRGB chunk. Both resolve to sRGB.
    const auto& description = image.GetRasterColorDescription();
    EXPECT_TRUE(description.origin_ == RasterColorOrigin::DefaultSrgb ||
                description.origin_ == RasterColorOrigin::PngSrgbChunk)
        << raster.file_name_;
    EXPECT_EQ(description.primaries_xy_, color::GamutPrimariesXy(color::ColorGamutId::Rec709))
        << raster.file_name_;
    EXPECT_EQ(description.transfer_[0].kind_, RasterTransferKind::SrgbPiecewise)
        << raster.file_name_;
    const auto display = image.ExifDisplayToJson();
    EXPECT_EQ(display.value("ImageWidth", 0u), 32u) << raster.file_name_;
    EXPECT_EQ(display.value("ImageHeight", 0u), 24u) << raster.file_name_;
  }

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

// The vcpkg Exiv2 build has no XMP toolkit, so the rating comes from EXIF for RAW and raster
// files alike.
TEST(MetadataExtractorTest, RasterJpegKeepsIccDescriptionAndReadsRatingFromExif) {
  const auto dir = std::filesystem::temp_directory_path() / "alcedo_metadata_raster_rating";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  const auto path = dir / "display_p3.jpg";
  std::filesystem::copy_file(RasterFixturePath("display_p3_icc_8bit.jpg"), path);
  {
    auto exiv = Exiv2::ImageFactory::open(path.string());
    exiv->readMetadata();
    exiv->exifData()["Exif.Image.Rating"] = static_cast<uint16_t>(4);
    exiv->writeMetadata();
  }

  Image image(8, path, ImageType::DEFAULT);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  EXPECT_EQ(image.image_type_, ImageType::JPEG);
  ASSERT_TRUE(image.HasRasterColorDescription());
  EXPECT_EQ(image.GetRasterColorDescription().origin_, RasterColorOrigin::IccMatrixShaper);
  EXPECT_NEAR(image.GetRasterColorDescription().primaries_xy_[0],
              color::GamutPrimariesXy(color::ColorGamutId::P3D65)[0], 2e-3f);
  EXPECT_EQ(image.ExifDisplayToJson().value("Rating", 0), 4);
  EXPECT_FALSE(image.ExifDisplayToJson().value("IsHDR", false));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

TEST(MetadataExtractorTest, ExrIsSceneLinearAndHdr) {
  const auto path = RasterFixturePath("chromaticities_p3_half.exr");
  Image      image(9, path, ImageType::DEFAULT);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  EXPECT_EQ(image.image_type_, ImageType::EXR);
  ASSERT_TRUE(image.HasRasterColorDescription());
  EXPECT_EQ(image.GetRasterColorDescription().referral_, RasterReferral::SceneLinear);
  EXPECT_TRUE(image.ExifDisplayToJson().value("IsHDR", false));
}

TEST(MetadataExtractorTest, PqPngIsHdrAndSrgbPngIsNot) {
  const auto pq_path = RasterFixturePath("cicp_rec2020_pq_16bit.png");
  Image      pq(10, pq_path, ImageType::DEFAULT);
  MetadataExtractor::ExtractEXIF_ToImage(pq_path, pq);
  EXPECT_TRUE(pq.ExifDisplayToJson().value("IsHDR", false));

  const auto srgb_path = RasterFixturePath("srgb_chunk.png");
  Image      srgb(11, srgb_path, ImageType::DEFAULT);
  MetadataExtractor::ExtractEXIF_ToImage(srgb_path, srgb);
  EXPECT_EQ(srgb.image_type_, ImageType::PNG);
  EXPECT_FALSE(srgb.ExifDisplayToJson().value("IsHDR", false));
}

TEST(MetadataExtractorTest, OrientationSixSwapsDisplayDimensions) {
  const auto path = RasterFixturePath("srgb_orientation6_8bit.jpg");
  Image      image(12, path, ImageType::DEFAULT);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  const auto display = image.ExifDisplayToJson();
  EXPECT_LT(display.value("ImageWidth", 0u), display.value("ImageHeight", 0u))
      << "the fixture is stored landscape and displays portrait";
}

TEST(MetadataExtractorTest, CmykJpegIsUnsupportedFormat) {
  const auto path = RasterFixturePath("cmyk_icc.jpg");
  Image      image(13, path, ImageType::DEFAULT);
  try {
    MetadataExtractor::ExtractEXIF_ToImage(path, image);
    FAIL() << "Expected MetadataExtractionError for a CMYK JPEG";
  } catch (const MetadataExtractionError& e) {
    EXPECT_EQ(e.code(), ImportErrorCode::UNSUPPORTED_FORMAT);
  }
  EXPECT_FALSE(image.HasRasterColorDescription());
}

// Section 7.4: rows written before this change, and RAW rows written after it, carry no raster
// key. The key round-trips for raster rows and is not part of the display JSON.
TEST(MetadataExtractorTest, ImageRowsWithoutRasterKeyLoadAsRaw) {
  Image image(14, std::filesystem::path("legacy.ARW"), ImageType::DEFAULT);
  image.JsonToExif(R"({"Make":"Sony","Model":"ILCE-7CM2","ImageWidth":7008,"ImageHeight":4672})");
  EXPECT_FALSE(image.HasRasterColorDescription());
  EXPECT_EQ(image.image_type_, ImageType::DEFAULT);
}

TEST(MetadataExtractorTest, RasterKeyIsOmittedFromRawImageMetadataJson) {
  Image raw(15, std::filesystem::path("camera.ARW"), ImageType::DEFAULT);
  raw.SetRawColorContext(RawRuntimeColorContext{});
  const auto raw_json = nlohmann::json::parse(raw.ExifToJson());
  EXPECT_FALSE(raw_json.contains("RasterColorDescription"));

  const auto path = RasterFixturePath("display_p3_icc_8bit.jpg");
  Image      raster(16, path, ImageType::DEFAULT);
  MetadataExtractor::ExtractEXIF_ToImage(path, raster);
  const auto raster_json = nlohmann::json::parse(raster.ExifToJson());
  ASSERT_TRUE(raster_json.contains("RasterColorDescription"));
  EXPECT_FALSE(raster_json.contains("RawRuntimeColorContext"));
  EXPECT_FALSE(raster.ExifDisplayToJson().contains("RasterColorDescription"));

  Image reloaded(16, path, ImageType::JPEG);
  reloaded.JsonToExif(raster.ExifToJson());
  ASSERT_TRUE(reloaded.HasRasterColorDescription());
  EXPECT_EQ(RasterColorDescriptionToJson(reloaded.GetRasterColorDescription()),
            RasterColorDescriptionToJson(raster.GetRasterColorDescription()));
  EXPECT_FALSE(reloaded.ExifDisplayToJson().contains("RasterColorDescription"));
}

}  // namespace
}  // namespace alcedo
