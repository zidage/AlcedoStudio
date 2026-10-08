//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Content classification, the raster decoder, and the raster develop pass list
// (raster_image_input_plan.md, sections 6.1 and 6.2). Fixtures come from
// tests/resources/raster/generate_raster_fixtures.py.

#include "edit/input/raster_input_loader.hpp"

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "image/image_content_class.hpp"
#include "image/raster_color_description.hpp"

namespace alcedo {
namespace {

auto ReadFile(const std::filesystem::path& path) -> std::vector<std::byte> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    ADD_FAILURE() << "missing file " << path.string();
    return {};
  }
  const std::vector<char> chars((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

auto Fixture(const std::string& name) -> std::vector<std::byte> {
  return ReadFile(std::filesystem::path(ALCEDO_RASTER_FIXTURE_DIR) / name);
}

auto Load(const std::string& name, RasterFileKind kind, DecodeRes res = DecodeRes::FULL)
    -> PreparedRawInput {
  return RasterInputLoader::LoadEncoded(Fixture(name), res, kind);
}

template <typename T>
auto Pixel(const PreparedRawInput& input, std::uint32_t x, std::uint32_t y, int channel) -> T {
  const auto* row =
      input.pixels.bytes.get() + static_cast<std::size_t>(y) * input.pixels.stride_bytes;
  T value{};
  std::memcpy(&value, row + (static_cast<std::size_t>(x) * 4 + channel) * sizeof(T), sizeof(T));
  return value;
}

/// Little-endian TIFF with one IFD: 1x1 RGB8 and, optionally, a DNGVersion tag.
auto MinimalTiff(bool dng_version) -> std::vector<std::byte> {
  std::vector<std::uint8_t> b = {'I', 'I', 42, 0, 8, 0, 0, 0};
  struct Entry {
    std::uint16_t tag_, type_;
    std::uint32_t count_, value_;
  };
  std::vector<Entry> entries = {{256, 3, 1, 1}, {257, 3, 1, 1}, {258, 3, 1, 8},
                                {259, 3, 1, 1}, {262, 3, 1, 2}, {273, 4, 1, 0},
                                {277, 3, 1, 3}, {278, 3, 1, 1}, {279, 4, 1, 3}};
  if (dng_version) {
    entries.push_back({50706, 1, 4, 0x00000401});
  }
  const std::uint32_t data_offset = 8 + 2 + static_cast<std::uint32_t>(entries.size()) * 12 + 4;
  for (auto& entry : entries) {
    if (entry.tag_ == 273) {
      entry.value_ = data_offset;
    }
  }
  auto put16 = [&](std::uint16_t v) {
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
  };
  auto put32 = [&](std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
  };
  put16(static_cast<std::uint16_t>(entries.size()));
  for (const auto& entry : entries) {
    put16(entry.tag_);
    put16(entry.type_);
    put32(entry.count_);
    put32(entry.value_);
  }
  put32(0);
  b.insert(b.end(), {10, 20, 30});
  std::vector<std::byte> bytes(b.size());
  std::memcpy(bytes.data(), b.data(), b.size());
  return bytes;
}

TEST(RasterInputLoaderTest, TiffMagicWithoutCameraMakeClassifiesAsRaster) {
  EXPECT_EQ(ClassifyImageContent(Fixture("uncompressed_rgb16_no_make.tif")),
            ImageContentClass::Tiff);
  EXPECT_EQ(ClassifyImageContent(MinimalTiff(false)), ImageContentClass::Tiff);
}

TEST(RasterInputLoaderTest, NikonCfaWithoutColorMatrixClassifiesAndDecodesAsRaw) {
  const auto bytes = Fixture("nikon_cfa_without_color_matrix.tif");
  auto       raw   = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_buffer(bytes.data(), bytes.size()), LIBRAW_SUCCESS);
  ASSERT_GT(raw->imgdata.idata.raw_count, 0u);
  ASSERT_STREQ(raw->imgdata.idata.make, "Nikon");
  for (const auto& row : raw->imgdata.color.cam_xyz) {
    for (const float value : row) {
      ASSERT_FLOAT_EQ(value, 0.0f);
    }
  }
  EXPECT_EQ(ClassifyImageContent(bytes), ImageContentClass::Raw);
  EXPECT_FALSE(RasterFileKindFor(ClassifyImageContent(bytes)).has_value());
  const auto input = LoadEncodedImage(bytes, DecodeRes::FULL);
  EXPECT_EQ(input.input_kind, RawInputKind::BayerRaw);
  EXPECT_EQ(input.host_extent, (Extent2D{32, 24}));
  EXPECT_EQ(input.pixels.format, HostPixelFormat::U16Cfa);
  EXPECT_EQ(Pixel<std::uint16_t>(input, 0, 0, 0), 1000);
}

TEST(RasterInputLoaderTest, NikonRgbTiffWithCameraMetadataClassifiesAndDecodesAsRaster) {
  const auto bytes = Fixture("nikon_rgb_with_camera_metadata.tif");
  EXPECT_EQ(ClassifyImageContent(bytes), ImageContentClass::Tiff);
  const auto input = LoadEncodedImage(bytes, DecodeRes::FULL);
  EXPECT_EQ(input.input_kind, RawInputKind::RasterRgb);
  EXPECT_EQ(input.host_extent, (Extent2D{32, 24}));
  EXPECT_EQ(input.pixels.format, HostPixelFormat::U16Rgba);
  EXPECT_EQ(Pixel<std::uint16_t>(input, 0, 0, 0), 1000);
}

TEST(RasterInputLoaderTest, DngClassifiesAsRaw) {
  EXPECT_EQ(ClassifyImageContent(MinimalTiff(true)), ImageContentClass::Raw);
  const std::filesystem::path root(ALCEDO_CI_RAW_FIXTURE_ROOT);
  bool                        checked_dng       = false;
  bool                        checked_arw       = false;
  bool                        checked_real_file = false;
  if (std::filesystem::exists(root)) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      const auto ext     = entry.path().extension().string();
      // The first DNG and the first ARW keep the test short; LibRaw opens each file.
      bool&      checked = ext == ".dng" ? checked_dng : checked_arw;
      if ((ext == ".dng" || ext == ".ARW") && !checked) {
        checked = true;
        EXPECT_EQ(ClassifyImageContent(ReadFile(entry.path())), ImageContentClass::Raw)
            << entry.path().string();
        checked_real_file = true;
      }
    }
  }
  if (!checked_real_file) {
    GTEST_SKIP() << "CI RAW fixtures are not checked out; only the synthetic DNG was checked.";
  }
}

TEST(RasterInputLoaderTest, MagicNumbersClassifyJpegPngExrAndLeaveOtherContentUnknown) {
  EXPECT_EQ(ClassifyImageContent(Fixture("srgb_icc_8bit.jpg")), ImageContentClass::Jpeg);
  EXPECT_EQ(ClassifyImageContent(Fixture("srgb_chunk.png")), ImageContentClass::Png);
  EXPECT_EQ(ClassifyImageContent(Fixture("no_chromaticities_float.exr")),
            ImageContentClass::OpenExr);
  const std::vector<std::byte> other(64, std::byte{0x42});
  EXPECT_EQ(ClassifyImageContent(other), ImageContentClass::Unknown);
  EXPECT_EQ(RasterFileKindFor(ImageContentClass::Png), RasterFileKind::Png);
  EXPECT_FALSE(RasterFileKindFor(ImageContentClass::Raw).has_value());
}

TEST(RasterInputLoaderTest, JpegDecodesAtNativeDepthAndScalesWithDecodeRes) {
  const auto full = Load("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  EXPECT_EQ(full.input_kind, RawInputKind::RasterRgb);
  EXPECT_EQ(full.pixels.format, HostPixelFormat::U8Rgba);
  EXPECT_EQ(full.host_extent, (Extent2D{16, 12}));
  EXPECT_EQ(full.develop_output_extent, (Extent2D{16, 12}));
  EXPECT_EQ(full.downsample_passes, 0);
  EXPECT_EQ(full.CompileSource().kind, DevelopInputKind::Raster);

  const auto half = Load("srgb_icc_8bit.jpg", RasterFileKind::Jpeg, DecodeRes::HALF);
  EXPECT_EQ(half.host_extent, (Extent2D{8, 6}));
  EXPECT_EQ(half.develop_output_extent, (Extent2D{8, 6}));
  EXPECT_EQ(half.full_reference_extent, (Extent2D{16, 12}));
  EXPECT_EQ(half.downsample_passes, 1);
  EXPECT_NE(half.source_key.downsample_passes, full.source_key.downsample_passes);
  EXPECT_EQ(full.source_key.input_kind, RawInputKind::RasterRgb);
}

TEST(RasterInputLoaderTest, SixteenBitPngAndFloatExrKeepTheirDepthAtFullSize) {
  const auto png = Load("cicp_rec2020_pq_16bit.png", RasterFileKind::Png, DecodeRes::QUARTER);
  EXPECT_EQ(png.pixels.format, HostPixelFormat::U16Rgba);
  // PNG, TIFF and OpenEXR decode at full size; GeometryResample scales in linear light.
  EXPECT_EQ(png.host_extent, (Extent2D{16, 12}));
  EXPECT_EQ(png.downsample_passes, 0);
  EXPECT_EQ(Pixel<std::uint16_t>(png, 0, 0, 3), 65535);

  const auto exr = Load("chromaticities_p3_half.exr", RasterFileKind::OpenExr);
  EXPECT_EQ(exr.pixels.format, HostPixelFormat::F32Rgba);
  // The generator writes R = x / 15, G = y / 11 (half precision).
  EXPECT_NEAR(Pixel<float>(exr, 15, 0, 0), 1.0f, 1e-3f);
  EXPECT_NEAR(Pixel<float>(exr, 0, 11, 1), 1.0f, 1e-3f);
  EXPECT_FLOAT_EQ(Pixel<float>(exr, 3, 3, 3), 1.0f);

  const auto tiff = Load("float32_no_icc.tif", RasterFileKind::Tiff);
  EXPECT_EQ(tiff.pixels.format, HostPixelFormat::F32Rgba);
}

TEST(RasterInputLoaderTest, GrayIsCopiedToRgbAndAlphaIsDiscarded) {
  const auto gray = Load("gray_gamma22.png", RasterFileKind::Png);
  EXPECT_EQ(gray.pixels.format, HostPixelFormat::U8Rgba);
  for (std::uint32_t x = 0; x < 16; x += 5) {
    EXPECT_EQ(Pixel<std::uint8_t>(gray, x, 4, 0), Pixel<std::uint8_t>(gray, x, 4, 1));
    EXPECT_EQ(Pixel<std::uint8_t>(gray, x, 4, 0), Pixel<std::uint8_t>(gray, x, 4, 2));
  }

  const auto rgba = Load("rgba_with_alpha.png", RasterFileKind::Png);
  const auto rgb  = Load("rgb_same_as_rgba_with_alpha.png", RasterFileKind::Png);
  ASSERT_EQ(rgba.pixels.ByteCount(), rgb.pixels.ByteCount());
  EXPECT_EQ(std::memcmp(rgba.pixels.bytes.get(), rgb.pixels.bytes.get(), rgb.pixels.ByteCount()),
            0);
  EXPECT_EQ(Pixel<std::uint8_t>(rgba, 0, 0, 3), 255);

  const auto palette = Load("palette_untagged.png", RasterFileKind::Png);
  EXPECT_EQ(palette.pixels.format, HostPixelFormat::U8Rgba);
  EXPECT_EQ(Pixel<std::uint8_t>(palette, 1, 0, 0), 80);  // palette entry 1 = (80, 175, 128)
}

TEST(RasterInputLoaderTest, ExifOrientationSixRotatesTheDevelopExtent) {
  const auto rotated = Load("srgb_orientation6_8bit.jpg", RasterFileKind::Jpeg);
  EXPECT_EQ(rotated.sensor.orientation_flip, 6);
  EXPECT_EQ(rotated.host_extent, (Extent2D{16, 12}));
  EXPECT_EQ(rotated.develop_output_extent, (Extent2D{12, 16}));
  EXPECT_EQ(rotated.full_reference_extent, (Extent2D{12, 16}));
}

TEST(RasterInputLoaderTest, MirroredOrientationIsAppliedToTheHostPlane) {
  const auto mirrored = Load("srgb_mirrored_orientation2.png", RasterFileKind::Png);
  const auto plain    = Load("srgb_chunk.png", RasterFileKind::Png);
  EXPECT_EQ(mirrored.sensor.orientation_flip, 0);
  for (std::uint32_t x = 0; x < 16; ++x) {
    EXPECT_EQ(Pixel<std::uint8_t>(mirrored, x, 5, 0), Pixel<std::uint8_t>(plain, 15 - x, 5, 0));
  }
}

TEST(RasterInputLoaderTest, LutBasedIccTiffIsConvertedToLinearRec2020OnTheHost) {
  const auto bytes       = Fixture("lut_based_rgb_icc.tif");
  const auto description = ResolveRasterColorDescription(bytes, RasterFileKind::Tiff);
  const auto input = RasterInputLoader::LoadEncoded(bytes, DecodeRes::FULL, RasterFileKind::Tiff);
  EXPECT_TRUE(input.raster_lut_icc_converted);
  EXPECT_EQ(input.raster_icc_sha256, description.icc_sha256_);
  EXPECT_EQ(input.pixels.format, HostPixelFormat::F32Rgba);
  // The profile is sRGB in a LUT; pixel (15, 0) has codes (1, 0, 15/26) (generator gradient).
  const float r = 1.0f, g = 0.0f;
  const float b     = std::pow((15.0f / 26.0f + 0.055f) / 1.055f, 2.4f);
  // Linear sRGB to linear Rec.2020 (BT.2087).
  const float r2020 = 0.6274f * r + 0.3293f * g + 0.0433f * b;
  const float g2020 = 0.0691f * r + 0.9195f * g + 0.0114f * b;
  const float b2020 = 0.0164f * r + 0.0880f * g + 0.8956f * b;
  // The 9-point CLUT interpolates the sRGB matrix; allow 2 percent of full scale.
  EXPECT_NEAR(Pixel<float>(input, 15, 0, 0), r2020, 0.02f);
  EXPECT_NEAR(Pixel<float>(input, 15, 0, 1), g2020, 0.02f);
  EXPECT_NEAR(Pixel<float>(input, 15, 0, 2), b2020, 0.02f);

  const auto plain = Load("rec2020_icc_16bit.tif", RasterFileKind::Tiff);
  EXPECT_FALSE(plain.raster_lut_icc_converted);
  EXPECT_EQ(plain.pixels.format, HostPixelFormat::U16Rgba);
}

TEST(RasterInputLoaderTest, CmykJpegPixelsAreRejected) {
  EXPECT_THROW((void)Load("cmyk_icc.jpg", RasterFileKind::Jpeg), std::runtime_error);
}

TEST(RasterInputLoaderTest, LoadEncodedImageRoutesRasterContentToTheRasterLoader) {
  const auto jpeg = LoadEncodedImage(Fixture("srgb_icc_8bit.jpg"), DecodeRes::FULL);
  EXPECT_EQ(jpeg.input_kind, RawInputKind::RasterRgb);
  const auto tiff = LoadEncodedImage(Fixture("uncompressed_rgb16_no_make.tif"), DecodeRes::FULL);
  EXPECT_EQ(tiff.input_kind, RawInputKind::RasterRgb);
  // Unknown content goes to LibRaw, which reports its own error; no retry.
  const std::vector<std::byte> other(64, std::byte{0x42});
  EXPECT_THROW((void)LoadEncodedImage(other, DecodeRes::FULL), std::runtime_error);
}

TEST(RasterInputLoaderTest, RasterJpegCompilesUploadRgbAndDisplayToAp1WithoutCameraToAp1) {
  const auto input    = Load("srgb_icc_8bit.jpg", RasterFileKind::Jpeg);
  auto       document = CreateDefaultRasterPipelineDocument(
      ResolveRasterColorDescription(Fixture("srgb_icc_8bit.jpg"), RasterFileKind::Jpeg));
  const auto plan = GraphCompiler::Compile(document, input.CompileSource(), RenderRequest{});
  EXPECT_TRUE(plan.Contains(GpuPassKind::UploadRgb));
  EXPECT_TRUE(plan.Contains(GpuPassKind::Lens));
  EXPECT_TRUE(plan.Contains(GpuPassKind::GeometryResample));
  EXPECT_TRUE(plan.Contains(GpuPassKind::DisplayToAp1));
  for (const auto absent : {GpuPassKind::CameraToAp1, GpuPassKind::UploadRaw,
                            GpuPassKind::Linearize, GpuPassKind::CfaClamp, GpuPassKind::Demosaic,
                            GpuPassKind::HighlightRecover, GpuPassKind::InverseCamMulPack}) {
    EXPECT_FALSE(plan.Contains(absent)) << GpuPassKindName(absent);
  }
  EXPECT_LT(plan.IndexOf(GpuPassKind::GeometryResample), plan.IndexOf(GpuPassKind::DisplayToAp1));
  EXPECT_LT(plan.IndexOf(GpuPassKind::DisplayToAp1), plan.IndexOf(GpuPassKind::Drt));
}

}  // namespace
}  // namespace alcedo
