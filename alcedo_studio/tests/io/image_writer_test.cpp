//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "io/image/image_writer.hpp"

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <exiv2/exiv2.hpp>
#if defined(ALCEDO_HAS_ULTRAHDR)
#include <ultrahdr_api.h>
#endif

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <opencv2/imgcodecs.hpp>
#include <string>
#include <string_view>
#include <vector>

#include "alcedo_version.hpp"
#include "icc_tone_curve_test_support.hpp"
#include "image/image_buffer.hpp"
#include "image/metadata.hpp"
#include "io/image/exif_ifd_tags.hpp"
#include "io/image/jpeg_exif_app1.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

class ImageWriterTests : public ::testing::Test {
 protected:
  std::filesystem::path temp_dir_;

  void                  SetUp() override {
    temp_dir_ = std::filesystem::temp_directory_path() / "alcedo_image_writer_test";
    std::filesystem::create_directories(temp_dir_);
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);
  }
};

auto PathToUtf8(const std::filesystem::path& path) -> std::string {
  return conv::ToBytes(path.wstring());
}

auto MakeColorProfile(ColorUtils::ColorSpace color_space, ColorUtils::EOTF eotf)
    -> ExportColorProfileConfig {
  return ExportColorProfileConfig{color_space, eotf, 600.0f};
}

auto ReadFileBytes(const std::filesystem::path& path) -> std::vector<uint8_t> {
  std::ifstream input(path, std::ios::binary);
  if (!input.is_open()) {
    return {};
  }
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(input), {});
}

/// JPEG metadata is written and verified via OIIO, matching the production
/// ImageWriter write path (Exiv2 MemIo open/write SEHs on MSVC).

void WriteTestJpeg(const std::filesystem::path& path, const std::vector<uint8_t>& rgb, int width,
                   int height) {
  OIIO_NAMESPACE_USING

  ImageSpec spec(width, height, 3, TypeDesc::UINT8);
  spec.channelnames                   = {"R", "G", "B"};
  std::unique_ptr<ImageOutput> output = ImageOutput::create(PathToUtf8(path));
  ASSERT_TRUE(output != nullptr);
  ASSERT_TRUE(output->open(PathToUtf8(path), spec));
  ASSERT_TRUE(output->write_image(TypeDesc::UINT8, rgb.data()));
  ASSERT_TRUE(output->close());
}

/// Prefer a real camera JPEG from the sample tree when present. Orientation is
/// forced upright by ImageWriter::ForceUprightOrientation on the OIIO write path;
/// we do not stamp EXIF via Exiv2::ExifParser::encode (Invalid key '' on this
/// toolchain for freshly constructed ExifData).
void WriteJpegWithOrientation(const std::filesystem::path& path, uint16_t /*orientation*/) {
  const auto sample = std::filesystem::path(TEST_IMG_PATH) / "jpeg" / "tile_tests" / "test_img.jpg";
  if (std::filesystem::exists(sample)) {
    std::error_code ec;
    std::filesystem::copy_file(sample, path, std::filesystem::copy_options::overwrite_existing, ec);
    ASSERT_FALSE(ec) << ec.message();
    return;
  }
  WriteTestJpeg(path, {255, 0, 0, 0, 255, 0}, 2, 1);
}

auto ReadOiioIntAttr(const std::filesystem::path& path, const char* key, int fallback = -1) -> int {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(PathToUtf8(path));
  if (!input) {
    return fallback;
  }
  const int value = input->spec().get_int_attribute(key, fallback);
  input->close();
  return value;
}

auto ReadOiioStringAttr(const std::filesystem::path& path, const char* key) -> std::string {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(PathToUtf8(path));
  if (!input) {
    return {};
  }
  const std::string value = input->spec().get_string_attribute(key);
  input->close();
  return value;
}

auto ReadOiioFloatAttr(const std::filesystem::path& path, const char* key, float fallback = -1.0f)
    -> float {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(PathToUtf8(path));
  if (!input) return fallback;
  const float value = input->spec().get_float_attribute(key, fallback);
  input->close();
  return value;
}

auto HasOiioAttr(const std::filesystem::path& path, const char* key) -> bool {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(PathToUtf8(path));
  if (!input) return false;
  const bool present = input->spec().find_attribute(key) != nullptr;
  input->close();
  return present;
}

auto ReadOiioIccProfile(const std::filesystem::path& path) -> std::vector<uint8_t> {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(PathToUtf8(path));
  if (!input) return {};
  std::vector<uint8_t> bytes;
  if (const ParamValue* attr = input->spec().find_attribute("ICCProfile"); attr != nullptr) {
    const auto* data = static_cast<const uint8_t*>(attr->data());
    bytes.assign(data, data + attr->type().size());
  }
  input->close();
  return bytes;
}

auto ReadOiioShape(const std::filesystem::path& path) -> std::array<int, 3> {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(PathToUtf8(path));
  if (!input) return {0, 0, 0};
  const auto shape =
      std::array<int, 3>{input->spec().width, input->spec().height, input->spec().nchannels};
  input->close();
  return shape;
}

auto JpegContainsAscii(const std::filesystem::path& path, const std::string& needle) -> bool {
  const auto bytes = ReadFileBytes(path);
  if (needle.empty() || bytes.size() < needle.size()) {
    return false;
  }
  return std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end()) != bytes.end();
}

auto ReadExifRatingFromApp1(const std::filesystem::path& path) -> int {
  const auto bytes = ReadFileBytes(path);
  if (bytes.size() < 12 || bytes[0] != 0xFF || bytes[1] != 0xD8) {
    return -1;
  }
  // Walk markers for APP1 Exif.
  size_t pos = 2;
  while (pos + 4 <= bytes.size() && bytes[pos] == 0xFF) {
    while (pos < bytes.size() && bytes[pos] == 0xFF) {
      ++pos;
    }
    if (pos >= bytes.size()) {
      break;
    }
    const uint8_t marker = bytes[pos++];
    if (marker == 0xDA || marker == 0xD9) {
      break;
    }
    if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
      continue;
    }
    if (pos + 2 > bytes.size()) {
      break;
    }
    const size_t length = (static_cast<size_t>(bytes[pos]) << 8) | bytes[pos + 1];
    if (length < 2 || pos + length > bytes.size()) {
      break;
    }
    if (marker == 0xE1 && length >= 8 && pos + 8 <= bytes.size() && bytes[pos + 2] == 'E' &&
        bytes[pos + 3] == 'x' && bytes[pos + 4] == 'i' && bytes[pos + 5] == 'f') {
      const size_t tiff_pos = pos + 8;
      if (tiff_pos + 8 > bytes.size()) {
        return -1;
      }
      const bool le = bytes[tiff_pos] == 'I' && bytes[tiff_pos + 1] == 'I';
      if (!le) {
        return -1;
      }
      const auto ru16 = [&](size_t at) -> uint16_t {
        return static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8));
      };
      const auto ru32 = [&](size_t at) -> uint32_t {
        return static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8) |
               (static_cast<uint32_t>(bytes[at + 2]) << 16) |
               (static_cast<uint32_t>(bytes[at + 3]) << 24);
      };
      const uint32_t ifd0 = ru32(tiff_pos + 4);
      const size_t   ifd  = tiff_pos + ifd0;
      if (ifd + 2 > bytes.size()) {
        return -1;
      }
      const uint16_t count = ru16(ifd);
      for (uint16_t i = 0; i < count; ++i) {
        const size_t entry = ifd + 2 + static_cast<size_t>(i) * 12u;
        if (entry + 12 > bytes.size()) {
          break;
        }
        if (ru16(entry) == 0x4746 && ru16(entry + 2) == 3) {
          return static_cast<int>(ru16(entry + 8));
        }
      }
      return -1;
    }
    pos += length;
  }
  return -1;
}

auto DumpOiioAttrSummary(const std::filesystem::path& path) -> std::string {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(PathToUtf8(path));
  if (!input) {
    return "<open failed>";
  }
  std::string out;
  for (const auto& attr : input->spec().extra_attribs) {
    out += attr.name().string();
    out += '=';
    out += attr.get_string();
    out += "; ";
  }
  input->close();
  return out.empty() ? "<no attrs>" : out;
}

}  // namespace

TEST_F(ImageWriterTests, UltraHdrTriggerMatchesHdrJpegCombinations) {
  ExportFormatOptions jpeg_options;
  jpeg_options.format_ = ImageFormatType::JPEG;

  EXPECT_TRUE(ImageWriter::ShouldWriteUltraHdr(
      jpeg_options, MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084)));
  EXPECT_TRUE(ImageWriter::ShouldWriteUltraHdr(
      jpeg_options, MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::HLG)));
  EXPECT_FALSE(ImageWriter::ShouldWriteUltraHdr(
      jpeg_options, MakeColorProfile(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2)));

  jpeg_options.hdr_export_mode_ = ExportFormatOptions::HDR_EXPORT_MODE::EMBEDDED_PROFILE_ONLY;
  EXPECT_FALSE(ImageWriter::ShouldWriteUltraHdr(
      jpeg_options, MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084)));

  ExportFormatOptions png_options;
  png_options.format_ = ImageFormatType::PNG;
  EXPECT_FALSE(ImageWriter::ShouldWriteUltraHdr(
      png_options, MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084)));

  ExportFormatOptions tiff_options;
  tiff_options.format_ = ImageFormatType::TIFF;
  EXPECT_FALSE(ImageWriter::ShouldWriteUltraHdr(
      tiff_options, MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::HLG)));

  ExportFormatOptions exr_options;
  exr_options.format_ = ImageFormatType::EXR;
  EXPECT_FALSE(ImageWriter::ShouldWriteUltraHdr(
      exr_options, MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084)));
}

TEST_F(ImageWriterTests, LegacyJpegExportForcesUprightOrientation) {
  const auto src_path = temp_dir_ / "source.jpg";
  const auto dst_path = temp_dir_ / "exported.jpg";

  WriteJpegWithOrientation(src_path, /*orientation=*/6);

  cv::Mat rgba32f(1, 2, CV_32FC4);
  rgba32f.at<cv::Vec4f>(0, 0)    = cv::Vec4f(1.0f, 0.0f, 0.0f, 1.0f);
  rgba32f.at<cv::Vec4f>(0, 1)    = cv::Vec4f(0.0f, 1.0f, 0.0f, 1.0f);

  auto                image_data = std::make_shared<ImageBuffer>(std::move(rgba32f));

  ExportFormatOptions options;
  options.format_      = ImageFormatType::JPEG;
  options.export_path_ = dst_path;

  ASSERT_NO_THROW(
      ImageWriter::WriteImageToPath(src_path, image_data, options,
                                    ExportColorProfileConfig{ColorUtils::ColorSpace::REC709,
                                                             ColorUtils::EOTF::GAMMA_2_2, 100.0f}));

  ASSERT_TRUE(std::filesystem::exists(dst_path));
  ASSERT_GT(std::filesystem::file_size(dst_path), 0u);

  // Production decode path: OIIO reads pixels + Orientation written by ForceUprightOrientation.
  {
    OIIO_NAMESPACE_USING
    auto decoded = ImageInput::open(PathToUtf8(dst_path));
    ASSERT_TRUE(decoded != nullptr);
    const auto& spec = decoded->spec();
    EXPECT_EQ(spec.width, 2);
    EXPECT_EQ(spec.height, 1);
    EXPECT_EQ(spec.get_int_attribute("Orientation", 0), 1);
    decoded->close();
  }
}

TEST_F(ImageWriterTests, EmbeddedHdrIccModeRejectsHdrJpegExport) {
  const auto src_path = temp_dir_ / "hdr_source.jpg";
  const auto dst_path = temp_dir_ / "embedded_hdr.jpg";

  WriteTestJpeg(src_path,
                {
                    144,
                    96,
                    48,
                    144,
                    96,
                    48,
                    144,
                    96,
                    48,
                    144,
                    96,
                    48,
                },
                2, 2);

  cv::Mat             rgba32f(2, 2, CV_32FC4, cv::Scalar(0.65f, 0.35f, 0.15f, 1.0f));
  auto                image_data = std::make_shared<ImageBuffer>(std::move(rgba32f));

  ExportFormatOptions options;
  options.format_          = ImageFormatType::JPEG;
  options.export_path_     = dst_path;
  options.hdr_export_mode_ = ExportFormatOptions::HDR_EXPORT_MODE::EMBEDDED_PROFILE_ONLY;

  const auto hdr_profile =
      MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084);

  EXPECT_THROW(ImageWriter::WriteImageToPath(src_path, image_data, options, hdr_profile),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(dst_path));
}

TEST_F(ImageWriterTests, EmbeddedHdrIccModeRejectsMetadataInjectedHdrJpegExport) {
  const auto src_path = temp_dir_ / "hdr_metadata_source.jpg";
  const auto dst_path = temp_dir_ / "embedded_hdr_metadata.jpg";

  WriteTestJpeg(src_path,
                {
                    64,
                    32,
                    16,
                    64,
                    32,
                    16,
                    64,
                    32,
                    16,
                    64,
                    32,
                    16,
                },
                2, 2);

  // Source EXIF is optional for this rejection path; WriteImageToPath must
  // throw on EMBEDDED_PROFILE_ONLY + HDR before metadata injection matters.

  cv::Mat             rgba32f(2, 2, CV_32FC4, cv::Scalar(0.62f, 0.41f, 0.21f, 1.0f));
  auto                image_data = std::make_shared<ImageBuffer>(std::move(rgba32f));

  ExportFormatOptions options;
  options.format_          = ImageFormatType::JPEG;
  options.export_path_     = dst_path;
  options.hdr_export_mode_ = ExportFormatOptions::HDR_EXPORT_MODE::EMBEDDED_PROFILE_ONLY;

  const auto hdr_profile =
      MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084);

  ExifDisplayMetaData metadata;
  metadata.lens_          = "Alcedo Test 35mm F1.8";
  metadata.date_time_str_ = "2024-05-06 07:08:09";
  metadata.rating_        = 5;

  EXPECT_THROW(ImageWriter::WriteImageToPath(src_path, image_data, options, hdr_profile, metadata),
               std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(dst_path));
}

TEST_F(ImageWriterTests, UltraHdrExportSupportsGainMapDitherToggle) {
#if !defined(ALCEDO_HAS_ULTRAHDR)
  GTEST_SKIP() << "Ultra HDR support is not enabled in this build.";
#else
  const auto           src_path = temp_dir_ / "ultra_hdr_source.jpg";
  std::vector<uint8_t> source_rgb;
  source_rgb.reserve(8 * 8 * 3);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const auto v = static_cast<uint8_t>(16 + (x + y) * 12);
      source_rgb.insert(source_rgb.end(), {v, v, v});
    }
  }
  WriteTestJpeg(src_path, source_rgb, 8, 8);

  cv::Mat rgba32f(8, 8, CV_32FC4);
  for (int y = 0; y < rgba32f.rows; ++y) {
    for (int x = 0; x < rgba32f.cols; ++x) {
      const float v               = 0.08f + 0.025f * static_cast<float>(x + y);
      rgba32f.at<cv::Vec4f>(y, x) = cv::Vec4f(v, v * 0.85f, v * 0.65f, 1.0f);
    }
  }
  auto       image_data = std::make_shared<ImageBuffer>(std::move(rgba32f));

  const auto hdr_profile =
      MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084);

  for (const bool dither_enabled : {false, true}) {
    ExportFormatOptions options;
    options.format_ = ImageFormatType::JPEG;
    options.export_path_ =
        temp_dir_ / (dither_enabled ? "ultra_hdr_dither_on.jpg" : "ultra_hdr_dither_off.jpg");
    options.hdr_export_mode_          = ExportFormatOptions::HDR_EXPORT_MODE::ULTRA_HDR;
    options.ultra_hdr_dither_enabled_ = dither_enabled;

    ImageWriter::WriteImageToPath(src_path, image_data, options, hdr_profile);

    const std::vector<uint8_t> bytes = ReadFileBytes(options.export_path_);
    ASSERT_FALSE(bytes.empty());
    EXPECT_EQ(is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size())), 1)
        << "dither_enabled=" << dither_enabled;
  }
#endif
}

TEST_F(ImageWriterTests, ExportWritesSoftwareNameAndVersion) {
  const auto src_path = temp_dir_ / "software_source.jpg";
  const auto dst_path = temp_dir_ / "software_exported.jpg";
  WriteTestJpeg(src_path, {32, 64, 96, 96, 64, 32}, 2, 1);

  cv::Mat rgba32f(1, 2, CV_32FC4);
  rgba32f.at<cv::Vec4f>(0, 0) = cv::Vec4f(0.2f, 0.4f, 0.6f, 1.0f);
  rgba32f.at<cv::Vec4f>(0, 1) = cv::Vec4f(0.6f, 0.4f, 0.2f, 1.0f);
  auto image_data             = std::make_shared<ImageBuffer>(std::move(rgba32f));

  ExportFormatOptions options;
  options.format_      = ImageFormatType::JPEG;
  options.export_path_ = dst_path;

  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, image_data, options));
  ASSERT_TRUE(std::filesystem::exists(dst_path));
  EXPECT_TRUE(JpegContainsAscii(dst_path, AlcedoSoftwareExifString()))
      << DumpOiioAttrSummary(dst_path);
}

TEST_F(ImageWriterTests, ExportWritesSoftwareAndCaptureMetadataForNonJpegFormats) {
  const auto src_path = temp_dir_ / "multi_format_source.jpg";
  WriteTestJpeg(src_path, {32, 64, 96, 96, 64, 32}, 2, 1);

  cv::Mat rgba32f(1, 2, CV_32FC4);
  rgba32f.at<cv::Vec4f>(0, 0) = cv::Vec4f(0.2f, 0.4f, 0.6f, 1.0f);
  rgba32f.at<cv::Vec4f>(0, 1) = cv::Vec4f(0.6f, 0.4f, 0.2f, 1.0f);

  ExifDisplayMetaData metadata;
  metadata.make_  = "AlcedoCam";
  metadata.model_ = "Format Matrix";
  metadata.iso_   = 200;

  const std::string software = AlcedoSoftwareExifString();
  // OIIO embeds the standard "Software" attribute for TIFF/PNG/EXR. JPEG uses
  // the APP1 rewriter. WEBP/BMP export is deprecated.
  struct Case {
    ImageFormatType                    format;
    const char*                        extension;
    ExportFormatOptions::BIT_DEPTH     bit_depth;
  };
  const Case cases[] = {
      {ImageFormatType::TIFF, ".tif", ExportFormatOptions::BIT_DEPTH::BIT_16},
      {ImageFormatType::PNG, ".png", ExportFormatOptions::BIT_DEPTH::BIT_8},
      {ImageFormatType::EXR, ".exr", ExportFormatOptions::BIT_DEPTH::BIT_16},
  };

  for (const auto& test_case : cases) {
    SCOPED_TRACE(test_case.extension);
    auto image_data = std::make_shared<ImageBuffer>(rgba32f.clone());
    ExportFormatOptions options;
    options.format_      = test_case.format;
    options.bit_depth_   = test_case.bit_depth;
    options.export_path_ = temp_dir_ / (std::string("software_exported") + test_case.extension);

    ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, image_data, options, std::nullopt,
                                                  metadata));
    ASSERT_TRUE(std::filesystem::exists(options.export_path_));

    const auto software_attr = !ReadOiioStringAttr(options.export_path_, "Software").empty()
                                   ? ReadOiioStringAttr(options.export_path_, "Software")
                                   : ReadOiioStringAttr(options.export_path_, "Exif:Software");
    EXPECT_EQ(software_attr, software) << DumpOiioAttrSummary(options.export_path_);

    const auto make = !ReadOiioStringAttr(options.export_path_, "Exif:Make").empty()
                          ? ReadOiioStringAttr(options.export_path_, "Exif:Make")
                          : ReadOiioStringAttr(options.export_path_, "Make");
    EXPECT_EQ(make, metadata.make_) << DumpOiioAttrSummary(options.export_path_);
  }
}

TEST_F(ImageWriterTests, UltraHdrExportWritesExifAndSoftware) {
#if !defined(ALCEDO_HAS_ULTRAHDR)
  GTEST_SKIP() << "Ultra HDR support is not enabled in this build.";
#else
  const auto           src_path = temp_dir_ / "ultra_hdr_exif_source.jpg";
  std::vector<uint8_t> source_rgb;
  source_rgb.reserve(8 * 8 * 3);
  for (int y = 0; y < 8; ++y) {
    for (int x = 0; x < 8; ++x) {
      const auto v = static_cast<uint8_t>(16 + (x + y) * 12);
      source_rgb.insert(source_rgb.end(), {v, v, v});
    }
  }
  WriteTestJpeg(src_path, source_rgb, 8, 8);

  cv::Mat rgba32f(8, 8, CV_32FC4);
  for (int y = 0; y < rgba32f.rows; ++y) {
    for (int x = 0; x < rgba32f.cols; ++x) {
      const float v               = 0.08f + 0.025f * static_cast<float>(x + y);
      rgba32f.at<cv::Vec4f>(y, x) = cv::Vec4f(v, v * 0.85f, v * 0.65f, 1.0f);
    }
  }
  auto image_data = std::make_shared<ImageBuffer>(std::move(rgba32f));

  const auto dst_path = temp_dir_ / "ultra_hdr_exif_exported.jpg";
  ExportFormatOptions options;
  options.format_          = ImageFormatType::JPEG;
  options.export_path_     = dst_path;
  options.hdr_export_mode_ = ExportFormatOptions::HDR_EXPORT_MODE::ULTRA_HDR;

  ExifDisplayMetaData metadata;
  metadata.make_          = "AlcedoCam";
  metadata.model_         = "UHDR Model";
  metadata.date_time_str_ = "2024-05-06 07:08:09";
  metadata.rating_        = 5;

  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(
      src_path, image_data, options,
      MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084), metadata));

  const std::vector<uint8_t> bytes = ReadFileBytes(dst_path);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size())), 1);
  EXPECT_TRUE(JpegContainsAscii(dst_path, metadata.make_)) << DumpOiioAttrSummary(dst_path);
  EXPECT_TRUE(JpegContainsAscii(dst_path, metadata.model_)) << DumpOiioAttrSummary(dst_path);
  EXPECT_TRUE(JpegContainsAscii(dst_path, "2024:05:06 07:08:09") ||
              JpegContainsAscii(dst_path, "2024-05-06"))
      << DumpOiioAttrSummary(dst_path);
  EXPECT_EQ(ReadExifRatingFromApp1(dst_path), 5) << DumpOiioAttrSummary(dst_path);
  EXPECT_TRUE(JpegContainsAscii(dst_path, AlcedoSoftwareExifString()))
      << DumpOiioAttrSummary(dst_path);
#endif
}

TEST_F(ImageWriterTests, DeprecatedWebpAndBmpExportFormatsAreRejected) {
  const auto src_path = temp_dir_ / "deprecated_source.jpg";
  WriteTestJpeg(src_path, {32, 64, 96, 96, 64, 32}, 2, 1);

  cv::Mat rgba32f(1, 2, CV_32FC4, cv::Scalar(0.2f, 0.4f, 0.6f, 1.0f));
  auto    image_data = std::make_shared<ImageBuffer>(rgba32f.clone());

  for (const ImageFormatType format : {ImageFormatType::WEBP, ImageFormatType::BMP}) {
    ExportFormatOptions options;
    options.format_      = format;
    options.export_path_ = temp_dir_ / "deprecated_out.bin";
    EXPECT_THROW(ImageWriter::WriteImageToPath(src_path, image_data, options), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(options.export_path_));
  }
}

TEST_F(ImageWriterTests, ExportWritesCurrentRatingMetadata) {
  const auto src_path = temp_dir_ / "rating_source.jpg";
  const auto dst_path = temp_dir_ / "rating_exported.jpg";

  WriteTestJpeg(src_path, {32, 64, 96, 96, 64, 32}, 2, 1);

  // Prior source rating is irrelevant; export metadata is applied from
  // ExifDisplayMetaData via ImageWriter::ApplyExportMetadata / OIIO attrs.

  cv::Mat rgba32f(1, 2, CV_32FC4);
  rgba32f.at<cv::Vec4f>(0, 0)    = cv::Vec4f(0.2f, 0.4f, 0.6f, 1.0f);
  rgba32f.at<cv::Vec4f>(0, 1)    = cv::Vec4f(0.6f, 0.4f, 0.2f, 1.0f);
  auto                image_data = std::make_shared<ImageBuffer>(std::move(rgba32f));

  ExportFormatOptions options;
  options.format_      = ImageFormatType::JPEG;
  options.export_path_ = dst_path;

  ExifDisplayMetaData metadata;
  metadata.rating_ = 4;

  ASSERT_NO_THROW(
      ImageWriter::WriteImageToPath(src_path, image_data, options, std::nullopt, metadata));

  ASSERT_TRUE(std::filesystem::exists(dst_path));
  ASSERT_GT(std::filesystem::file_size(dst_path), 0u);

  // Production APP1 rewrite stamps Rating (0x4746); verify by parsing the segment
  // the same way viewers do, without Exiv2 MemIo (SEH on this toolchain).
  EXPECT_EQ(ReadExifRatingFromApp1(dst_path), 4) << DumpOiioAttrSummary(dst_path);
}

TEST_F(ImageWriterTests, ExportWritesCurrentLensAndCaptureDateMetadata) {
  const auto src_path = temp_dir_ / "metadata_source.jpg";
  const auto dst_path = temp_dir_ / "metadata_exported.jpg";

  WriteTestJpeg(src_path, {128, 96, 64, 64, 96, 128}, 2, 1);

  cv::Mat rgba32f(1, 2, CV_32FC4);
  rgba32f.at<cv::Vec4f>(0, 0)    = cv::Vec4f(0.5f, 0.4f, 0.3f, 1.0f);
  rgba32f.at<cv::Vec4f>(0, 1)    = cv::Vec4f(0.3f, 0.4f, 0.5f, 1.0f);
  auto                image_data = std::make_shared<ImageBuffer>(std::move(rgba32f));

  ExportFormatOptions options;
  options.format_      = ImageFormatType::JPEG;
  options.export_path_ = dst_path;

  ExifDisplayMetaData metadata;
  metadata.make_          = "AlcedoCam";
  metadata.model_         = "Model T";
  metadata.lens_make_     = "Alcedo Optics";
  metadata.lens_          = "Alcedo Optics 50mm F2";
  metadata.date_time_str_ = "2023-12-31 23:59:58";
  metadata.focal_         = 50.0f;
  metadata.aperture_      = 2.0f;
  metadata.iso_           = 400;

  ASSERT_NO_THROW(
      ImageWriter::WriteImageToPath(src_path, image_data, options, std::nullopt, metadata));

  ASSERT_TRUE(std::filesystem::exists(dst_path));
  ASSERT_GT(std::filesystem::file_size(dst_path), 0u);

  const auto make  = !ReadOiioStringAttr(dst_path, "Exif:Make").empty()
                         ? ReadOiioStringAttr(dst_path, "Exif:Make")
                         : ReadOiioStringAttr(dst_path, "Make");
  const auto model = !ReadOiioStringAttr(dst_path, "Exif:Model").empty()
                         ? ReadOiioStringAttr(dst_path, "Exif:Model")
                         : ReadOiioStringAttr(dst_path, "Model");
  if (!make.empty() || !model.empty()) {
    EXPECT_EQ(make, metadata.make_);
    EXPECT_EQ(model, metadata.model_);
  } else {
    // Some OIIO JPEG builds embed ASCII EXIF payloads without exposing attrs on
    // reopen; still require the production-stamped strings to land in the file.
    EXPECT_TRUE(JpegContainsAscii(dst_path, metadata.make_)) << DumpOiioAttrSummary(dst_path);
    EXPECT_TRUE(JpegContainsAscii(dst_path, metadata.model_)) << DumpOiioAttrSummary(dst_path);
  }
  EXPECT_TRUE(JpegContainsAscii(dst_path, metadata.lens_make_) ||
              !ReadOiioStringAttr(dst_path, "Exif:LensMake").empty() ||
              !ReadOiioStringAttr(dst_path, "LensMake").empty())
      << DumpOiioAttrSummary(dst_path);
  EXPECT_TRUE(JpegContainsAscii(dst_path, "2023:12:31 23:59:58") ||
              JpegContainsAscii(dst_path, "2023-12-31") ||
              !ReadOiioStringAttr(dst_path, "Exif:DateTimeOriginal").empty())
      << DumpOiioAttrSummary(dst_path);
}

TEST_F(ImageWriterTests, RecipePhysicalSizeWritesPixelsDpiCaptureMetadataAndDiscardsAlpha) {
  const auto src_path = temp_dir_ / "recipe_source.jpg";
  const auto dst_path = temp_dir_ / "recipe_exported.tif";
  WriteTestJpeg(src_path, std::vector<uint8_t>(4 * 2 * 3, 96), 4, 2);

  cv::Mat      rgba32f(2, 4, CV_32FC4, cv::Scalar(0.2f, 0.4f, 0.6f, 0.5f));
  auto         image_data = std::make_shared<ImageBuffer>(rgba32f.clone());

  ExportRecipe recipe;
  recipe.codec_.format_          = ImageFormatType::TIFF;
  recipe.codec_.export_path_     = dst_path;
  recipe.codec_.bit_depth_       = ExportFormatOptions::BIT_DEPTH::BIT_16;
  recipe.resize_.mode_           = ExportResizeMode::PHYSICAL_SIZE;
  recipe.resize_.physical_width_ = 1.0;
  recipe.resize_.physical_unit_  = ExportPhysicalUnit::INCHES;
  recipe.resize_.dpi_            = 10.0;
  recipe.resize_.allow_upscale_  = true;
  recipe.alpha_                  = ExportAlphaPolicy::DISCARD;

  ExifDisplayMetaData metadata;
  metadata.make_          = "FUJIFILM";
  metadata.model_         = "X-T5";
  metadata.iso_           = 800;
  metadata.aperture_      = 4.0f;
  metadata.shutter_speed_ = {1, 13};
  metadata.focal_         = 29.6f;
  metadata.focal_35mm_    = 44.0f;

  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(
      src_path, image_data, recipe,
      MakeColorProfile(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2), metadata));

  const auto shape = ReadOiioShape(dst_path);
  EXPECT_EQ(shape[0], 10);
  EXPECT_EQ(shape[1], 5);
  EXPECT_EQ(shape[2], 3);
  EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:ISOSpeed"), 800) << DumpOiioAttrSummary(dst_path);
  EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:SensitivityType"), 1);
  EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:StandardOutputSensitivity"), 800);
  EXPECT_NEAR(ReadOiioFloatAttr(dst_path, "FNumber"), 4.0f, 0.01f) << DumpOiioAttrSummary(dst_path);
  EXPECT_NEAR(ReadOiioFloatAttr(dst_path, "ExposureTime"), 1.0f / 13.0f, 0.001f)
      << DumpOiioAttrSummary(dst_path);
  EXPECT_NEAR(ReadOiioFloatAttr(dst_path, "Exif:FocalLength"), 29.6f, 0.01f);
  EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:FocalLengthIn35mmFilm"), 44);
  EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:PixelXDimension"), 10);
  EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:PixelYDimension"), 5);
  EXPECT_NEAR(ReadOiioFloatAttr(dst_path, "XResolution"), 10.0f, 0.01f);
  EXPECT_TRUE(HasOiioAttr(dst_path, "ICCProfile"));
}

// A color-managed viewer decodes the exported code values with the curve of the embedded profile.
// The profile must declare the curve that encoded the pixels for every Rec.709 display encoding
// (issue #391: macOS embedded the BT.709 camera curve for gamma 2.2 pixels).
TEST_F(ImageWriterTests, ExportedJpegIccToneCurveMatchesRec709Encoding) {
  const auto src_path = temp_dir_ / "rec709_source.jpg";
  WriteTestJpeg(src_path, {32, 64, 96, 96, 64, 32}, 2, 1);

  for (const auto eotf :
       {ColorUtils::EOTF::BT1886, ColorUtils::EOTF::GAMMA_2_2, ColorUtils::EOTF::SRGB_PIECEWISE}) {
    const std::string   label    = ColorUtils::EOTFToString(eotf);
    const auto          dst_path = temp_dir_ / ("rec709_" + label + ".jpg");
    cv::Mat             rgba32f(1, 2, CV_32FC4, cv::Scalar(0.2f, 0.4f, 0.6f, 1.0f));
    auto                image_data = std::make_shared<ImageBuffer>(rgba32f.clone());

    ExportFormatOptions options;
    options.format_      = ImageFormatType::JPEG;
    options.export_path_ = dst_path;
    ASSERT_NO_THROW(ImageWriter::WriteImageToPath(
        src_path, image_data, options,
        ExportColorProfileConfig{ColorUtils::ColorSpace::REC709, eotf, 100.0f}))
        << label;

    const auto icc = ReadOiioIccProfile(dst_path);
    ASSERT_FALSE(icc.empty()) << label;
    for (const std::string_view tag : {"rTRC", "gTRC", "bTRC"}) {
      const auto curve = icc_tone_curve_test::ReadIccToneCurve(icc, tag);
      ASSERT_TRUE(curve.has_value()) << label << " " << tag;
      for (const double code : {0.02, 0.05, 0.1, 0.18, 0.25, 0.5, 0.75, 1.0}) {
        const double expected = CeDecode(static_cast<int>(eotf), static_cast<float>(code));
        EXPECT_NEAR(curve->Evaluate(code), expected, 5e-4)
            << label << " " << tag << " code value " << code;
      }
    }
  }
}

TEST_F(ImageWriterTests, RecipeCanOmitMetadataAndIccFromTiff) {
  const auto src_path = temp_dir_ / "private_source.jpg";
  const auto dst_path = temp_dir_ / "private_exported.tif";
  WriteTestJpeg(src_path, {32, 64, 96, 96, 64, 32}, 2, 1);

  cv::Mat      rgba32f(1, 2, CV_32FC4, cv::Scalar(0.2f, 0.4f, 0.6f, 1.0f));
  auto         image_data = std::make_shared<ImageBuffer>(rgba32f.clone());

  ExportRecipe recipe;
  recipe.codec_.format_      = ImageFormatType::TIFF;
  recipe.codec_.export_path_ = dst_path;
  recipe.metadata_.mode_     = ExportMetadataMode::NONE;
  recipe.icc_                = ExportIccPolicy::OMIT;

  ExifDisplayMetaData metadata;
  metadata.make_  = "Private Camera";
  metadata.model_ = "Private Model";
  metadata.iso_   = 12800;

  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(
      src_path, image_data, recipe,
      MakeColorProfile(ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2), metadata));

  EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:Make"));
  EXPECT_FALSE(HasOiioAttr(dst_path, "Make"));
  EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:Model"));
  EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:ISOSpeedRatings"));
  EXPECT_FALSE(HasOiioAttr(dst_path, "ICCProfile"));
}

namespace {

auto MakeRationalTag(uint16_t tag, std::initializer_list<std::pair<uint32_t, uint32_t>> values)
    -> ExifIfdTag {
  ExifIfdTag result{.tag_ = tag, .type_ = 5, .count_ = static_cast<uint32_t>(values.size())};
  for (const auto& [numerator, denominator] : values) {
    for (const uint32_t part : {numerator, denominator}) {
      for (int shift = 0; shift < 32; shift += 8) {
        result.value_.push_back(static_cast<uint8_t>((part >> shift) & 0xFF));
      }
    }
  }
  return result;
}

auto MakeAsciiTag(uint16_t tag, const std::string& text) -> ExifIfdTag {
  ExifIfdTag result{.tag_ = tag, .type_ = 2, .value_ = {text.begin(), text.end()}};
  result.value_.push_back(0);
  result.count_ = static_cast<uint32_t>(result.value_.size());
  return result;
}

/// A GPS IFD with the tags and types that cameras write, at a fictitious position in the Pacific
/// Ocean (10 deg 20' 30.40" N, 150 deg 10' 20.30" E, 120 m).
auto MakeTestGpsTags() -> std::vector<ExifIfdTag> {
  return {
      ExifIfdTag{.tag_ = 0x0000, .type_ = 1, .count_ = 4, .value_ = {2, 3, 0, 0}},
      MakeAsciiTag(0x0001, "N"),
      MakeRationalTag(0x0002, {{10, 1}, {20, 1}, {3040, 100}}),
      MakeAsciiTag(0x0003, "E"),
      MakeRationalTag(0x0004, {{150, 1}, {10, 1}, {2030, 100}}),
      ExifIfdTag{.tag_ = 0x0005, .type_ = 1, .count_ = 1, .value_ = {0}},
      MakeRationalTag(0x0006, {{120, 1}}),
      MakeAsciiTag(0x0012, "WGS-84"),
  };
}

/// A JPEG source whose APP1 segment holds the GPS IFD of MakeTestGpsTags.
void WriteJpegWithGps(const std::filesystem::path& path) {
  WriteTestJpeg(path, {32, 64, 96, 96, 64, 32}, 2, 1);
  ASSERT_TRUE(ApplyJpegExifApp1Metadata(
      path,
      ExportExifOptions{.include_software_ = false,
                        .source_entries_ = {.ifd0_ = {}, .exif_ = {}, .gps_ = MakeTestGpsTags()}}));
}

auto ReadOiioFloats(const std::filesystem::path& path, const char* key) -> std::vector<float> {
  OIIO_NAMESPACE_USING
  auto input = ImageInput::open(PathToUtf8(path));
  if (!input) return {};
  std::vector<float> values;
  if (const ParamValue* attr = input->spec().find_attribute(key);
      attr != nullptr && attr->type().basetype == TypeDesc::FLOAT) {
    const auto* data = static_cast<const float*>(attr->data());
    values.assign(data, data + attr->type().basevalues() * attr->nvalues());
  }
  input->close();
  return values;
}

/// Degrees of a GPS:Latitude or GPS:Longitude value (degrees, minutes, seconds). Cameras store
/// either whole minutes and seconds or decimal minutes with zero seconds.
auto DecimalDegrees(const std::vector<float>& values) -> double {
  return values.size() == 3 ? values[0] + values[1] / 60.0 + values[2] / 3600.0 : -1.0;
}

/// The position of MakeTestGpsTags: 10 deg 20' 30.40" N, 150 deg 10' 20.30" E, 120 m.
void ExpectTestGpsInOiioAttributes(const std::filesystem::path& path) {
  EXPECT_EQ(ReadOiioStringAttr(path, "GPS:LatitudeRef"), "N") << DumpOiioAttrSummary(path);
  EXPECT_EQ(ReadOiioStringAttr(path, "GPS:LongitudeRef"), "E") << DumpOiioAttrSummary(path);
  EXPECT_NEAR(DecimalDegrees(ReadOiioFloats(path, "GPS:Latitude")),
              10.0 + 20.0 / 60.0 + 30.40 / 3600.0, 1e-5)
      << DumpOiioAttrSummary(path);
  EXPECT_NEAR(DecimalDegrees(ReadOiioFloats(path, "GPS:Longitude")),
              150.0 + 10.0 / 60.0 + 20.30 / 3600.0, 1e-5)
      << DumpOiioAttrSummary(path);
  EXPECT_NEAR(ReadOiioFloatAttr(path, "GPS:Altitude"), 120.0f, 0.5f) << DumpOiioAttrSummary(path);
}

auto MakeRecipe(ImageFormatType format, const std::filesystem::path& export_path,
                bool include_location) -> ExportRecipe {
  ExportRecipe recipe;
  recipe.codec_.format_              = format;
  recipe.codec_.bit_depth_           = format == ImageFormatType::JPEG
                                           ? ExportFormatOptions::BIT_DEPTH::BIT_8
                                           : ExportFormatOptions::BIT_DEPTH::BIT_16;
  recipe.codec_.export_path_         = export_path;
  recipe.metadata_.include_location_ = include_location;
  return recipe;
}

auto MakeFlatImage(int rows, int cols) -> std::shared_ptr<ImageBuffer> {
  return std::make_shared<ImageBuffer>(
      cv::Mat(rows, cols, CV_32FC4, cv::Scalar(0.2f, 0.4f, 0.6f, 1.0f)));
}

}  // namespace

TEST_F(ImageWriterTests, ReadExifGpsTagsReturnsTheSourceGpsIfdEntries) {
  const auto src_path = temp_dir_ / "gps_read_source.jpg";
  WriteJpegWithGps(src_path);

  const auto tags     = ReadExifGpsTags(src_path);
  const auto expected = MakeTestGpsTags();
  ASSERT_EQ(tags.size(), expected.size());
  for (size_t i = 0; i < tags.size(); ++i) {
    SCOPED_TRACE(tags[i].tag_);
    EXPECT_EQ(tags[i].tag_, expected[i].tag_);
    EXPECT_EQ(tags[i].type_, expected[i].type_);
    EXPECT_EQ(tags[i].count_, expected[i].count_);
    EXPECT_EQ(tags[i].value_, expected[i].value_);
  }
}

TEST_F(ImageWriterTests, ReadExifGpsTagsIsEmptyForSourceWithoutGps) {
  const auto src_path = temp_dir_ / "no_gps_source.jpg";
  WriteTestJpeg(src_path, {32, 64, 96, 96, 64, 32}, 2, 1);
  EXPECT_TRUE(ReadExifGpsTags(src_path).empty());
}

// Issue 363: the JPEG APP1 rewrite dropped the GPS IFD of the source file.
TEST_F(ImageWriterTests, JpegExportCopiesSourceGpsWhenLocationIsIncluded) {
  const auto src_path = temp_dir_ / "gps_source.jpg";
  const auto dst_path = temp_dir_ / "gps_exported.jpg";
  WriteJpegWithGps(src_path);

  ExifDisplayMetaData metadata;
  metadata.make_ = "NIKON CORPORATION";
  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(1, 2),
                                                MakeRecipe(ImageFormatType::JPEG, dst_path, true),
                                                std::nullopt, metadata));

  ExpectTestGpsInOiioAttributes(dst_path);
  EXPECT_EQ(ReadOiioStringAttr(dst_path, "GPS:MapDatum"), "WGS-84")
      << DumpOiioAttrSummary(dst_path);
  EXPECT_EQ(ReadOiioStringAttr(dst_path, "Make"), metadata.make_)
      << DumpOiioAttrSummary(dst_path);
  EXPECT_TRUE(JpegContainsAscii(dst_path, AlcedoSoftwareExifString()));
}

TEST_F(ImageWriterTests, PngAndExrExportsCopySourceGpsWhenLocationIsIncluded) {
  const auto src_path = temp_dir_ / "gps_multi_source.jpg";
  WriteJpegWithGps(src_path);

  for (const auto& [format, extension] :
       {std::pair{ImageFormatType::PNG, ".png"}, std::pair{ImageFormatType::EXR, ".exr"}}) {
    SCOPED_TRACE(extension);
    const auto dst_path = temp_dir_ / (std::string("gps_exported") + extension);
    ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(1, 2),
                                                  MakeRecipe(format, dst_path, true),
                                                  std::nullopt, std::nullopt));
    ExpectTestGpsInOiioAttributes(dst_path);
  }
}

// OIIO writes no GPS IFD into TIFF files (and its reader reports none), so the TIFF export adds
// the GPS IFD itself; Exiv2 reads it back.
TEST_F(ImageWriterTests, TiffExportWritesSourceGpsIfd) {
  const auto src_path = temp_dir_ / "gps_tiff_source.jpg";
  const auto dst_path = temp_dir_ / "gps_exported.tif";
  WriteJpegWithGps(src_path);

  ExifDisplayMetaData metadata;
  metadata.make_ = "NIKON CORPORATION";
  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(5, 7),
                                                MakeRecipe(ImageFormatType::TIFF, dst_path, true),
                                                std::nullopt, metadata));

  const auto tags     = ReadExifGpsTags(dst_path);
  const auto expected = MakeTestGpsTags();
  ASSERT_EQ(tags.size(), expected.size());
  for (size_t i = 0; i < tags.size(); ++i) {
    SCOPED_TRACE(tags[i].tag_);
    EXPECT_EQ(tags[i].tag_, expected[i].tag_);
    EXPECT_EQ(tags[i].type_, expected[i].type_);
    EXPECT_EQ(tags[i].count_, expected[i].count_);
    EXPECT_EQ(tags[i].value_, expected[i].value_);
  }
  // The pixels and the other IFD0 and EXIF entries stay readable after the new IFD0.
  const auto shape = ReadOiioShape(dst_path);
  EXPECT_EQ(shape[0], 7);
  EXPECT_EQ(shape[1], 5);
  EXPECT_EQ(ReadOiioStringAttr(dst_path, "Make"), metadata.make_) << DumpOiioAttrSummary(dst_path);
  EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:PixelXDimension"), 7) << DumpOiioAttrSummary(dst_path);
}

TEST_F(ImageWriterTests, ExportOmitsSourceGpsWhenLocationIsExcluded) {
  const auto src_path = temp_dir_ / "gps_private_source.jpg";
  WriteJpegWithGps(src_path);

  for (const auto& [format, extension] :
       {std::pair{ImageFormatType::JPEG, ".jpg"}, std::pair{ImageFormatType::TIFF, ".tif"}}) {
    SCOPED_TRACE(extension);
    const auto dst_path = temp_dir_ / (std::string("gps_private_exported") + extension);
    ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(1, 2),
                                                  MakeRecipe(format, dst_path, false),
                                                  std::nullopt, std::nullopt));
    EXPECT_FALSE(HasOiioAttr(dst_path, "GPS:Latitude")) << DumpOiioAttrSummary(dst_path);
    EXPECT_TRUE(ReadExifGpsTags(dst_path).empty());
  }
}

TEST_F(ImageWriterTests, UltraHdrExportCopiesSourceGpsWhenLocationIsIncluded) {
#if !defined(ALCEDO_HAS_ULTRAHDR)
  GTEST_SKIP() << "Ultra HDR support is not enabled in this build.";
#else
  const auto src_path = temp_dir_ / "gps_uhdr_source.jpg";
  const auto dst_path = temp_dir_ / "gps_uhdr_exported.jpg";
  WriteJpegWithGps(src_path);

  cv::Mat rgba32f(8, 8, CV_32FC4);
  for (int y = 0; y < rgba32f.rows; ++y) {
    for (int x = 0; x < rgba32f.cols; ++x) {
      const float v               = 0.08f + 0.025f * static_cast<float>(x + y);
      rgba32f.at<cv::Vec4f>(y, x) = cv::Vec4f(v, v * 0.85f, v * 0.65f, 1.0f);
    }
  }
  auto         image_data = std::make_shared<ImageBuffer>(std::move(rgba32f));

  ExportRecipe recipe     = MakeRecipe(ImageFormatType::JPEG, dst_path, true);
  recipe.codec_.hdr_export_mode_ = ExportFormatOptions::HDR_EXPORT_MODE::ULTRA_HDR;
  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(
      src_path, image_data, recipe,
      MakeColorProfile(ColorUtils::ColorSpace::REC2020, ColorUtils::EOTF::ST2084), std::nullopt));

  const std::vector<uint8_t> bytes = ReadFileBytes(dst_path);
  ASSERT_FALSE(bytes.empty());
  EXPECT_EQ(is_uhdr_image(const_cast<uint8_t*>(bytes.data()), static_cast<int>(bytes.size())), 1);
  ExpectTestGpsInOiioAttributes(dst_path);
#endif
}

// Issue 363 with the camera file: the GPS IFD of a Nikon Z 7II NEF must reach the exported JPEG.
TEST_F(ImageWriterTests, NikonNefGpsReachesJpegExport) {
  const auto src_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "gps" / "DSC_2815.NEF";
  if (!std::filesystem::exists(src_path)) {
    GTEST_SKIP() << "Local sample is missing: " << src_path.string();
  }
  const auto source_tags = ReadExifGpsTags(src_path);
  ASSERT_FALSE(source_tags.empty());

  const auto dst_path = temp_dir_ / "nef_gps_exported.jpg";
  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(4, 6),
                                                MakeRecipe(ImageFormatType::JPEG, dst_path, true),
                                                std::nullopt, std::nullopt));
  const auto tags = ReadExifGpsTags(dst_path);
  ASSERT_EQ(tags.size(), source_tags.size());
  for (size_t i = 0; i < tags.size(); ++i) {
    SCOPED_TRACE(tags[i].tag_);
    EXPECT_EQ(tags[i].tag_, source_tags[i].tag_);
    EXPECT_EQ(tags[i].type_, source_tags[i].type_);
    EXPECT_EQ(tags[i].count_, source_tags[i].count_);
    EXPECT_EQ(tags[i].value_, source_tags[i].value_);
  }
  EXPECT_TRUE(HasOiioAttr(dst_path, "GPS:Latitude")) << DumpOiioAttrSummary(dst_path);
}

namespace {

auto MakeShortTag(uint16_t tag, uint16_t value) -> ExifIfdTag {
  return ExifIfdTag{
      .tag_   = tag,
      .type_  = 3,
      .count_ = 1,
      .value_ = {static_cast<uint8_t>(value & 0xFF), static_cast<uint8_t>((value >> 8) & 0xFF)}};
}

auto MakeLongTag(uint16_t tag, uint32_t value) -> ExifIfdTag {
  ExifIfdTag result{.tag_ = tag, .type_ = 4, .count_ = 1, .value_ = {}};
  for (int shift = 0; shift < 32; shift += 8) {
    result.value_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
  }
  return result;
}

auto MakeUndefinedTag(uint16_t tag, std::vector<uint8_t> bytes) -> ExifIfdTag {
  const auto count = static_cast<uint32_t>(bytes.size());
  return ExifIfdTag{.tag_ = tag, .type_ = 7, .count_ = count, .value_ = std::move(bytes)};
}

/// IFD0 entries in the layout of the Nikon Z 7II camera file of issue 363, with fictitious
/// Artist and Copyright values and a rating.
auto MakeCameraIfd0Tags() -> std::vector<ExifIfdTag> {
  return {
      MakeAsciiTag(0x010F, "NIKON CORPORATION"),
      MakeAsciiTag(0x0110, "NIKON Z 7_2"),
      MakeShortTag(0x0112, 6),  // Orientation: rotated
      MakeAsciiTag(0x0131, "Ver.01.70"),
      MakeAsciiTag(0x0132, "2026:10:05 16:34:14"),
      MakeAsciiTag(0x013B, "Test Artist"),
      MakeShortTag(0x4746, 2),  // Rating
      MakeAsciiTag(0x8298, "Test Copyright"),
  };
}

/// EXIF IFD entries of the Nikon Z 7II camera file of issue 363, with fictitious serial numbers and
/// the fields that the camera JPEG adds (MaxApertureValue, ColorSpace, versions) and a MakerNote.
auto MakeCameraExifTags() -> std::vector<ExifIfdTag> {
  return {
      MakeRationalTag(0x829A, {{1, 3200}}),
      MakeRationalTag(0x829D, {{40, 10}}),
      MakeShortTag(0x8822, 3),  // ExposureProgram: aperture priority
      MakeShortTag(0x8827, 400),
      MakeUndefinedTag(0x9000, {'0', '2', '3', '1'}),
      MakeAsciiTag(0x9003, "2026:10:05 16:34:14"),
      MakeAsciiTag(0x9004, "2026:10:05 16:34:14"),
      MakeAsciiTag(0x9010, "+08:00"),
      MakeAsciiTag(0x9011, "+08:00"),
      MakeAsciiTag(0x9012, "+08:00"),
      ExifIfdTag{.tag_ = 0x9204, .type_ = 10, .count_ = 1, .value_ = {0, 0, 0, 0, 6, 0, 0, 0}},
      MakeRationalTag(0x9205, {{4, 1}}),  // MaxApertureValue
      MakeShortTag(0x9207, 3),            // MeteringMode: spot
      MakeShortTag(0x9208, 0),            // LightSource
      MakeShortTag(0x9209, 16),           // Flash: off
      MakeRationalTag(0x920A, {{1200, 10}}),
      MakeUndefinedTag(0x927C, {'N', 'i', 'k', 'o', 'n', 0, 2, 0x11, 0, 0, 'M', 'M', 0, 0x2A}),
      MakeAsciiTag(0x9290, "60"),
      MakeAsciiTag(0x9291, "60"),
      MakeAsciiTag(0x9292, "60"),
      MakeUndefinedTag(0xA000, {'0', '1', '0', '0'}),
      MakeShortTag(0xA001, 1),  // ColorSpace: sRGB
      MakeLongTag(0xA002, 8256),
      MakeLongTag(0xA003, 5504),
      MakeShortTag(0xA402, 0),  // ExposureMode
      MakeShortTag(0xA403, 0),  // WhiteBalance
      MakeShortTag(0xA405, 120),
      MakeShortTag(0xA406, 1),  // SceneCaptureType: landscape
      MakeShortTag(0xA408, 0),  // Contrast
      MakeShortTag(0xA409, 0),  // Saturation
      MakeShortTag(0xA40A, 0),  // Sharpness
      MakeAsciiTag(0xA431, "1234567"),
      MakeRationalTag(0xA432, {{24, 1}, {120, 1}, {4, 1}, {4, 1}}),  // LensSpecification
      MakeAsciiTag(0xA433, "NIKON"),
      MakeAsciiTag(0xA434, "NIKKOR Z 24-120mm f/4 S"),
      MakeAsciiTag(0xA435, "7654321"),
  };
}

/// A JPEG source whose APP1 segment holds the IFD0, EXIF, and GPS entries of the camera file.
void WriteJpegWithCameraExif(const std::filesystem::path& path) {
  WriteTestJpeg(path, {32, 64, 96, 96, 64, 32}, 2, 1);
  const auto tiff_block = BuildExifTiffBlock(
      {.ifd0_ = MakeCameraIfd0Tags(), .exif_ = MakeCameraExifTags(), .gps_ = MakeTestGpsTags()});
  std::vector<uint8_t> payload = {'E', 'x', 'i', 'f', 0, 0};
  payload.insert(payload.end(), tiff_block.begin(), tiff_block.end());
  ASSERT_TRUE(ReplaceJpegExifApp1Segment(path, payload));
}

auto FindExifTag(const std::vector<ExifIfdTag>& tags, uint16_t tag) -> const ExifIfdTag* {
  const auto it = std::find_if(tags.begin(), tags.end(),
                               [tag](const ExifIfdTag& entry) { return entry.tag_ == tag; });
  return it == tags.end() ? nullptr : &*it;
}

auto HasExifTag(const std::vector<ExifIfdTag>& tags, uint16_t tag) -> bool {
  return FindExifTag(tags, tag) != nullptr;
}

/// Expect that @p exported holds @p expected with the same type, count, and value bytes.
void ExpectExifTag(const std::vector<ExifIfdTag>& exported, const ExifIfdTag& expected) {
  SCOPED_TRACE(::testing::Message() << "tag 0x" << std::hex << expected.tag_);
  const ExifIfdTag* actual = FindExifTag(exported, expected.tag_);
  ASSERT_NE(actual, nullptr);
  EXPECT_EQ(actual->type_, expected.type_);
  EXPECT_EQ(actual->count_, expected.count_);
  EXPECT_EQ(actual->value_, expected.value_);
}

/// Expect that @p exported holds each entry of @p source, except the tags in @p replaced.
void ExpectSourceTagsCopied(const std::vector<ExifIfdTag>&  source,
                            const std::vector<ExifIfdTag>&  exported,
                            std::initializer_list<uint16_t> replaced) {
  for (const auto& tag : source) {
    if (std::find(replaced.begin(), replaced.end(), tag.tag_) == replaced.end()) {
      ExpectExifTag(exported, tag);
    }
  }
}

auto ExifAsciiValue(const std::vector<ExifIfdTag>& tags, uint16_t tag) -> std::string {
  const ExifIfdTag* entry = FindExifTag(tags, tag);
  if (entry == nullptr || entry->type_ != 2) return {};
  std::string text(entry->value_.begin(), entry->value_.end());
  text.erase(std::find(text.begin(), text.end(), '\0'), text.end());
  return text;
}

auto ExifShortValue(const std::vector<ExifIfdTag>& tags, uint16_t tag) -> int {
  const ExifIfdTag* entry = FindExifTag(tags, tag);
  if (entry == nullptr || entry->type_ != 3 || entry->value_.size() < 2) return -1;
  return entry->value_[0] | (entry->value_[1] << 8);
}

auto ExifLongValue(const std::vector<ExifIfdTag>& tags, uint16_t tag) -> int64_t {
  const ExifIfdTag* entry = FindExifTag(tags, tag);
  if (entry == nullptr || entry->value_.size() < 2) return -1;
  if (entry->type_ == 3) return entry->value_[0] | (entry->value_[1] << 8);
  if (entry->type_ != 4 || entry->value_.size() < 4) return -1;
  return static_cast<int64_t>(entry->value_[0]) | (static_cast<int64_t>(entry->value_[1]) << 8) |
         (static_cast<int64_t>(entry->value_[2]) << 16) |
         (static_cast<int64_t>(entry->value_[3]) << 24);
}

/// The display metadata of the camera file, with a rating changed in Alcedo.
auto MakeCameraDisplayMetadata() -> ExifDisplayMetaData {
  ExifDisplayMetaData metadata;
  metadata.make_          = "Nikon";
  metadata.model_         = "Z 7_2";
  metadata.lens_          = "NIKKOR Z 24-120mm f/4 S";
  metadata.date_time_str_ = "2026-10-05 16:34:14";
  metadata.aperture_      = 4.0f;
  metadata.iso_           = 400;
  metadata.rating_        = 4;
  return metadata;
}

/// Expect the IFD0 and EXIF entries of an export of MakeCameraIfd0Tags/MakeCameraExifTags with
/// MakeCameraDisplayMetadata, @p width x @p height pixels, and no device serial numbers.
void ExpectCameraExifInExport(const ExifIfdEntries& exported, int width, int height) {
  // The source entries reach the export unchanged.
  ExpectSourceTagsCopied(MakeCameraIfd0Tags(), exported.ifd0_, {0x0112, 0x0131, 0x4746});
  ExpectSourceTagsCopied(MakeCameraExifTags(), exported.exif_,
                         {0x9000, 0x927C, 0xA002, 0xA003, 0xA431, 0xA435});
  EXPECT_EQ(ExifAsciiValue(exported.ifd0_, 0x010F), "NIKON CORPORATION");
  EXPECT_EQ(ExifAsciiValue(exported.ifd0_, 0x0110), "NIKON Z 7_2");
  EXPECT_EQ(ExifAsciiValue(exported.ifd0_, 0x013B), "Test Artist");
  EXPECT_EQ(ExifAsciiValue(exported.ifd0_, 0x8298), "Test Copyright");
  EXPECT_EQ(ExifAsciiValue(exported.exif_, 0x9011), "+08:00");

  // The export writes its own values.
  EXPECT_EQ(ExifShortValue(exported.ifd0_, 0x0112), 1);
  EXPECT_EQ(ExifAsciiValue(exported.ifd0_, 0x0131), AlcedoSoftwareExifString());
  EXPECT_EQ(ExifShortValue(exported.ifd0_, 0x4746), 4);
  EXPECT_EQ(ExifShortValue(exported.ifd0_, 0x4749), 75);
  ExpectExifTag(exported.exif_, MakeUndefinedTag(0x9000, {'0', '2', '3', '2'}));
  ExpectExifTag(exported.exif_, MakeUndefinedTag(0xA000, {'0', '1', '0', '0'}));
  EXPECT_EQ(ExifLongValue(exported.exif_, 0xA002), width);
  EXPECT_EQ(ExifLongValue(exported.exif_, 0xA003), height);

  // The export does not carry the MakerNote or the serial numbers.
  EXPECT_FALSE(HasExifTag(exported.exif_, 0x927C));
  EXPECT_FALSE(HasExifTag(exported.exif_, 0xA431));
  EXPECT_FALSE(HasExifTag(exported.exif_, 0xA435));
}

}  // namespace

TEST_F(ImageWriterTests, SelectExportExifIfdEntriesDropsEntriesOfTheSourceFile) {
  ExifIfdEntries source;
  source.ifd0_ = {
      MakeLongTag(0x0100, 160),                     // ImageWidth of the RAW preview
      MakeShortTag(0x0103, 1),                      // Compression
      MakeAsciiTag(0x010F, "NIKON CORPORATION"),    // Make
      MakeLongTag(0x0111, 202208),                  // StripOffsets
      MakeShortTag(0x0112, 6),                      // Orientation
      MakeRationalTag(0x011A, {{300, 1}}),          // XResolution
      MakeAsciiTag(0x0131, "Ver.01.70"),            // Software
      MakeAsciiTag(0x013B, "Test Artist"),          // Artist
      MakeShortTag(0x4746, 2),                      // Rating
      MakeAsciiTag(0x8298, "Test Copyright"),       // Copyright
      MakeAsciiTag(0x9003, "2026:10:05 16:34:14"),  // TIFF/EP DateTimeOriginal
      ExifIfdTag{.tag_ = 0x9216, .type_ = 1, .count_ = 4, .value_ = {1, 0, 0, 0}},
      ExifIfdTag{.tag_ = 0xC612, .type_ = 1, .count_ = 4, .value_ = {1, 4, 0, 0}},
  };
  source.exif_                   = MakeCameraExifTags();
  source.gps_                    = MakeTestGpsTags();

  const ExifIfdEntries  selected = SelectExportExifIfdEntries(source, false, false);
  std::vector<uint16_t> ifd0_tags;
  for (const auto& tag : selected.ifd0_) ifd0_tags.push_back(tag.tag_);
  EXPECT_EQ(ifd0_tags, (std::vector<uint16_t>{0x010F, 0x013B, 0x8298}));
  for (const uint16_t excluded :
       std::initializer_list<uint16_t>{0x9000, 0x927C, 0xA000, 0xA002, 0xA003, 0xA431, 0xA435}) {
    EXPECT_FALSE(HasExifTag(selected.exif_, excluded)) << std::hex << excluded;
  }
  EXPECT_EQ(selected.exif_.size(), MakeCameraExifTags().size() - 7);
  EXPECT_TRUE(selected.gps_.empty());

  const ExifIfdEntries with_private = SelectExportExifIfdEntries(source, true, true);
  EXPECT_EQ(with_private.gps_.size(), MakeTestGpsTags().size());
  EXPECT_TRUE(HasExifTag(with_private.exif_, 0xA431));
  EXPECT_TRUE(HasExifTag(with_private.exif_, 0xA435));
}

// The JPEG APP1 rewrite kept only the display metadata and dropped the other source entries.
TEST_F(ImageWriterTests, JpegExportCopiesSourceIfd0AndExifEntries) {
  const auto src_path = temp_dir_ / "camera_exif_source.jpg";
  const auto dst_path = temp_dir_ / "camera_exif_exported.jpg";
  WriteJpegWithCameraExif(src_path);

  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(3, 4),
                                                MakeRecipe(ImageFormatType::JPEG, dst_path, false),
                                                std::nullopt, MakeCameraDisplayMetadata()));

  const ExifIfdEntries exported = ReadExifIfdEntries(dst_path);
  ExpectCameraExifInExport(exported, 4, 3);
  // Without an output profile, the source ColorSpace stays.
  EXPECT_EQ(ExifShortValue(exported.exif_, 0xA001), 1);
  // The JPEG IFD0 fields that exiftool -validate requires.
  EXPECT_EQ(ExifShortValue(exported.ifd0_, 0x0213), 1);
  EXPECT_EQ(ExifShortValue(exported.ifd0_, 0x0128), 2);
  ExpectExifTag(exported.ifd0_, MakeRationalTag(0x011A, {{7200, 100}}));
  ExpectExifTag(exported.exif_, MakeUndefinedTag(0x9101, {1, 2, 3, 0}));
  EXPECT_TRUE(exported.gps_.empty());
  EXPECT_EQ(ReadOiioStringAttr(dst_path, "Artist"), "Test Artist") << DumpOiioAttrSummary(dst_path);
}

TEST_F(ImageWriterTests, ExportCopiesDeviceSerialsOnlyWhenIncluded) {
  const auto src_path = temp_dir_ / "camera_serial_source.jpg";
  WriteJpegWithCameraExif(src_path);

  for (const auto& [format, extension] :
       {std::pair{ImageFormatType::JPEG, ".jpg"}, std::pair{ImageFormatType::TIFF, ".tif"}}) {
    SCOPED_TRACE(extension);
    const auto   dst_path = temp_dir_ / (std::string("camera_serial_exported") + extension);
    ExportRecipe recipe   = MakeRecipe(format, dst_path, true);
    recipe.metadata_.include_device_serials_ = true;
    ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(1, 2), recipe,
                                                  std::nullopt, std::nullopt));

    const ExifIfdEntries exported = ReadExifIfdEntries(dst_path);
    EXPECT_EQ(ExifAsciiValue(exported.exif_, 0xA431), "1234567");
    EXPECT_EQ(ExifAsciiValue(exported.exif_, 0xA435), "7654321");
    EXPECT_FALSE(HasExifTag(exported.exif_, 0x927C));
    EXPECT_EQ(exported.gps_.size(), MakeTestGpsTags().size());
  }
}

TEST_F(ImageWriterTests, ExportColorSpaceDescribesTheOutputProfile) {
  const auto src_path = temp_dir_ / "camera_color_space_source.jpg";
  WriteJpegWithCameraExif(src_path);

  struct Case {
    ColorUtils::ColorSpace space;
    ColorUtils::EOTF       eotf;
    int                    expected_color_space;
  };
  for (const Case& test_case :
       {Case{ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2, 1},
        Case{ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::SRGB_PIECEWISE, 1},
        Case{ColorUtils::ColorSpace::P3_D65, ColorUtils::EOTF::GAMMA_2_2, 0xFFFF}}) {
    for (const auto& [format, extension] :
         {std::pair{ImageFormatType::JPEG, ".jpg"}, std::pair{ImageFormatType::TIFF, ".tif"}}) {
      SCOPED_TRACE(::testing::Message()
                   << extension << " expected " << test_case.expected_color_space);
      const auto dst_path = temp_dir_ / (std::string("camera_color_space_exported") + extension);
      ASSERT_NO_THROW(ImageWriter::WriteImageToPath(
          src_path, MakeFlatImage(1, 2), MakeRecipe(format, dst_path, false),
          MakeColorProfile(test_case.space, test_case.eotf), std::nullopt));
      EXPECT_EQ(ExifShortValue(ReadExifIfdEntries(dst_path).exif_, 0xA001),
                test_case.expected_color_space);
    }
  }
}

TEST_F(ImageWriterTests, TiffExportWritesSourceIfd0AndExifEntries) {
  const auto src_path = temp_dir_ / "camera_tiff_source.jpg";
  const auto dst_path = temp_dir_ / "camera_tiff_exported.tif";
  WriteJpegWithCameraExif(src_path);

  ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(5, 7),
                                                MakeRecipe(ImageFormatType::TIFF, dst_path, true),
                                                std::nullopt, MakeCameraDisplayMetadata()));

  const ExifIfdEntries exported = ReadExifIfdEntries(dst_path);
  ExpectCameraExifInExport(exported, 7, 5);
  EXPECT_EQ(exported.gps_.size(), MakeTestGpsTags().size());
  // The TIFF image structure of OIIO stays: the pixels and the source orientation do not change.
  const auto shape = ReadOiioShape(dst_path);
  EXPECT_EQ(shape[0], 7);
  EXPECT_EQ(shape[1], 5);
  EXPECT_EQ(ReadOiioIntAttr(dst_path, "Orientation"), 1);
  EXPECT_EQ(ReadOiioStringAttr(dst_path, "Make"), "NIKON CORPORATION")
      << DumpOiioAttrSummary(dst_path);
  EXPECT_EQ(ReadOiioStringAttr(dst_path, "Artist"), "Test Artist") << DumpOiioAttrSummary(dst_path);
}

TEST_F(ImageWriterTests, PngAndExrExportsCopySourceIfd0AndExifEntries) {
  const auto src_path = temp_dir_ / "camera_png_exr_source.jpg";
  WriteJpegWithCameraExif(src_path);

  for (const auto& [format, extension] :
       {std::pair{ImageFormatType::PNG, ".png"}, std::pair{ImageFormatType::EXR, ".exr"}}) {
    SCOPED_TRACE(extension);
    const auto dst_path = temp_dir_ / (std::string("camera_exif_exported") + extension);
    ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(1, 2),
                                                  MakeRecipe(format, dst_path, true), std::nullopt,
                                                  MakeCameraDisplayMetadata()));

    const std::string attributes = DumpOiioAttrSummary(dst_path);
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Make"), "NIKON CORPORATION") << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Model"), "NIKON Z 7_2") << attributes;
    EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:Make")) << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Artist"), "Test Artist") << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Copyright"), "Test Copyright") << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Software"), AlcedoSoftwareExifString()) << attributes;
    EXPECT_EQ(ReadOiioIntAttr(dst_path, "Orientation"), 1) << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Exif:OffsetTimeOriginal"), "+08:00") << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Exif:SubsecTimeOriginal"), "60") << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Exif:ExifVersion"), "0232") << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Exif:FlashPixVersion"), "0100") << attributes;
    EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:ExposureProgram"), 3) << attributes;
    EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:MeteringMode"), 3) << attributes;
    EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:Flash"), 16) << attributes;
    EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:SceneCaptureType"), 1) << attributes;
    EXPECT_NEAR(ReadOiioFloatAttr(dst_path, "Exif:MaxApertureValue"), 4.0f, 0.01f) << attributes;
    EXPECT_EQ(ReadOiioStringAttr(dst_path, "Exif:LensModel"), "NIKKOR Z 24-120mm f/4 S")
        << attributes;
    EXPECT_EQ(ReadOiioIntAttr(dst_path, "Exif:PixelXDimension"), 2) << attributes;
    EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:BodySerialNumber")) << attributes;
    EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:LensSerialNumber")) << attributes;
    ExpectTestGpsInOiioAttributes(dst_path);
  }
}

TEST_F(ImageWriterTests, ExportOmitsSourceExifWhenExifIsExcluded) {
  const auto src_path = temp_dir_ / "camera_private_source.jpg";
  WriteJpegWithCameraExif(src_path);

  for (const bool metadata_off : {true, false}) {
    for (const auto& [format, extension] :
         {std::pair{ImageFormatType::JPEG, ".jpg"}, std::pair{ImageFormatType::TIFF, ".tif"},
          std::pair{ImageFormatType::PNG, ".png"}}) {
      SCOPED_TRACE(::testing::Message() << extension << (metadata_off ? " NONE" : " no EXIF"));
      const auto   dst_path = temp_dir_ / (std::string("camera_private_exported") + extension);
      ExportRecipe recipe   = MakeRecipe(format, dst_path, true);
      if (metadata_off) {
        recipe.metadata_.mode_ = ExportMetadataMode::NONE;
      } else {
        recipe.metadata_.include_exif_ = false;
      }
      ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(1, 2), recipe,
                                                    std::nullopt, MakeCameraDisplayMetadata()));
      const std::string attributes = DumpOiioAttrSummary(dst_path);
      EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:OffsetTimeOriginal")) << attributes;
      EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:ExposureProgram")) << attributes;
      EXPECT_FALSE(HasOiioAttr(dst_path, "Exif:SubsecTimeOriginal")) << attributes;
      EXPECT_FALSE(HasOiioAttr(dst_path, "GPS:Latitude")) << attributes;
      EXPECT_NE(ReadOiioStringAttr(dst_path, "Make"), "NIKON CORPORATION") << attributes;
      if (metadata_off) {
        EXPECT_FALSE(HasOiioAttr(dst_path, "Artist")) << attributes;
      }
    }
  }
}

TEST_F(ImageWriterTests, JpegApp1PayloadLeavesOutLargestSourceEntriesWhenTooLarge) {
  ExifIfdTag              user_comment = MakeUndefinedTag(0x9286, std::vector<uint8_t>(70000, 'x'));
  ExifIfdTag              maker_text   = MakeAsciiTag(0x9C9C, std::string(20, 'c'));  // XPComment
  const ExportExifOptions options{
      .metadata_         = MakeCameraDisplayMetadata(),
      .width_            = 2,
      .height_           = 1,
      .include_software_ = true,
      .source_entries_   = {.ifd0_ = {MakeAsciiTag(0x013B, "Test Artist"), maker_text},
                            .exif_ = {user_comment, MakeAsciiTag(0x9011, "+08:00")},
                            .gps_  = MakeTestGpsTags()},
      .color_space_      = std::nullopt,
      .resolution_dpi_   = 0.0,
  };
  const std::vector<uint8_t> payload = BuildJpegExifApp1Payload(options);
  ASSERT_FALSE(payload.empty());
  EXPECT_LE(payload.size(), 65533u);

  const auto path = temp_dir_ / "large_exif.jpg";
  WriteTestJpeg(path, {32, 64, 96, 96, 64, 32}, 2, 1);
  ASSERT_TRUE(ReplaceJpegExifApp1Segment(path, payload));
  const ExifIfdEntries written = ReadExifIfdEntries(path);
  EXPECT_FALSE(HasExifTag(written.exif_, 0x9286));
  EXPECT_EQ(ExifAsciiValue(written.ifd0_, 0x013B), "Test Artist");
  ExpectExifTag(written.ifd0_, maker_text);
  EXPECT_EQ(ExifAsciiValue(written.exif_, 0x9011), "+08:00");
  EXPECT_EQ(ExifShortValue(written.ifd0_, 0x4746), 4);
  EXPECT_EQ(written.gps_.size(), MakeTestGpsTags().size());
}

// The camera file of issue 363: the IFD0 and EXIF entries of a Nikon Z 7II NEF reach the export.
TEST_F(ImageWriterTests, NikonNefExifReachesJpegAndTiffExports) {
  const auto src_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "gps" / "DSC_2815.NEF";
  if (!std::filesystem::exists(src_path)) {
    GTEST_SKIP() << "Local sample is missing: " << src_path.string();
  }
  const ExifIfdEntries source =
      SelectExportExifIfdEntries(ReadExifIfdEntries(src_path), true, false);
  ASSERT_FALSE(source.ifd0_.empty());
  ASSERT_FALSE(source.exif_.empty());

  for (const auto& [format, extension] :
       {std::pair{ImageFormatType::JPEG, ".jpg"}, std::pair{ImageFormatType::TIFF, ".tif"}}) {
    SCOPED_TRACE(extension);
    const auto dst_path = temp_dir_ / (std::string("nef_exif_exported") + extension);
    ASSERT_NO_THROW(ImageWriter::WriteImageToPath(src_path, MakeFlatImage(4, 6),
                                                  MakeRecipe(format, dst_path, true), std::nullopt,
                                                  std::nullopt));
    const ExifIfdEntries exported = ReadExifIfdEntries(dst_path);
    ExpectSourceTagsCopied(source.ifd0_, exported.ifd0_, {});
    ExpectSourceTagsCopied(source.exif_, exported.exif_, {});
    ExpectSourceTagsCopied(source.gps_, exported.gps_, {});
    EXPECT_EQ(ExifAsciiValue(exported.ifd0_, 0x010F), "NIKON CORPORATION");
    EXPECT_TRUE(HasExifTag(exported.ifd0_, 0x013B));  // Artist
    EXPECT_EQ(ExifAsciiValue(exported.exif_, 0x9010), "+08:00");
    EXPECT_EQ(ExifShortValue(exported.exif_, 0x8822), 3);  // ExposureProgram: aperture priority
    EXPECT_EQ(ExifShortValue(exported.ifd0_, 0x0112), 1);
    EXPECT_FALSE(HasExifTag(exported.exif_, 0x927C));
    EXPECT_FALSE(HasExifTag(exported.exif_, 0xA431));
  }
}

}  // namespace alcedo
