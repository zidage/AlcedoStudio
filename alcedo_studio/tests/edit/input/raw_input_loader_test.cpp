//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/input/raw_input_loader.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include <pthread.h>
#endif

#include "decoders/processor/raw_rgb_normalization.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/graph_compiler.hpp"
#include "prepared_raw_test_support.hpp"

namespace alcedo {
namespace {

auto ReadBytes(const std::filesystem::path& path) -> std::vector<std::byte> {
  std::ifstream input(path, std::ios::binary);
  if (!input) return {};
  const std::vector<char> chars((std::istreambuf_iterator<char>(input)),
                                std::istreambuf_iterator<char>());
  std::vector<std::byte>  bytes(chars.size());
  std::memcpy(bytes.data(), chars.data(), chars.size());
  return bytes;
}

#if defined(__APPLE__)
struct ProductWorkerRawLoadContext {
  const std::vector<std::byte>*   encoded = nullptr;
  std::optional<PreparedRawInput> prepared;
  std::exception_ptr              failure;
};

auto LoadEncodedOnProductWorkerStack(void* opaque) noexcept -> void* {
  auto& context = *static_cast<ProductWorkerRawLoadContext*>(opaque);
  try {
    context.prepared = RawInputLoader::LoadEncoded(*context.encoded, DecodeRes::FULL);
  } catch (...) {
    context.failure = std::current_exception();
  }
  return nullptr;
}
#endif

TEST(GpuDagRawInput, RawInputLoaderUnpacksBeforePipelineBuild) {
  const auto pattern  = gpu_dag_test::MakeRggbPattern();
  const auto prepared = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);

  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});

  EXPECT_FALSE(prepared.host_extent.Empty());
  EXPECT_FALSE(plan.Contains(GpuPassKind::UploadRgb));
  EXPECT_TRUE(plan.Contains(GpuPassKind::UploadRaw));
  for (const auto& pass : plan.passes) {
    const char* name = GpuPassKindName(pass.kind);
    EXPECT_EQ(std::string(name).find("LibRaw"), std::string::npos);
    EXPECT_EQ(std::string(name).find("DecodeRes"), std::string::npos);
  }
}

TEST(GpuDagRawInput, RawInputLoaderDownsampleUpdatesCfaPatternAndPhase) {
  const auto xtrans = gpu_dag_test::MakeXTransPattern();
  const auto full   = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, xtrans), xtrans, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  const auto half = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, xtrans), xtrans, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::HALF);

  EXPECT_EQ(full.host_extent, (Extent2D{64, 64}));
  EXPECT_EQ(half.host_extent, (Extent2D{32, 32}));
  EXPECT_EQ(half.downsample_passes, 1);
  EXPECT_NE(
      std::memcmp(full.cfa_pattern.xtrans_pattern.raw_fc, half.cfa_pattern.xtrans_pattern.raw_fc,
                  sizeof(full.cfa_pattern.xtrans_pattern.raw_fc)),
      0);

  const auto bayer      = gpu_dag_test::MakeRggbPattern();
  const auto bayer_full = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, bayer), bayer, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  const auto bayer_half = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, bayer), bayer, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::HALF);
  EXPECT_EQ(bayer_half.host_extent, (Extent2D{32, 32}));
  EXPECT_EQ(std::memcmp(bayer_full.cfa_pattern.bayer_pattern.raw_fc,
                        bayer_half.cfa_pattern.bayer_pattern.raw_fc,
                        sizeof(bayer_full.cfa_pattern.bayer_pattern.raw_fc)),
            0);
}

TEST(GpuDagRawInput, PreparedRawInputKeepsFullReferenceExtentAcrossDecodeRes) {
  const auto pattern = gpu_dag_test::MakeRggbPattern();
  const auto full    = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::FULL);
  const auto half = RawInputLoader::FromUnpackedCfa(
      gpu_dag_test::MakeU16CfaPlane(64, 64, pattern), pattern, gpu_dag_test::DefaultLinearization(),
      gpu_dag_test::FullSensor(64, 64), DecodeRes::HALF);

  EXPECT_EQ(full.full_reference_extent, half.full_reference_extent);
  EXPECT_NE(full.develop_output_extent, half.develop_output_extent);
  EXPECT_EQ(full.full_reference_extent, full.develop_output_extent);
}

TEST(GpuDagRawInput, DirectRgbInputBypassesLibRawAndEntersDevelopEndpoint) {
  auto prepared = RawInputLoader::FromDirectRgb(gpu_dag_test::MakeF32RgbaPlane(48, 32),
                                                gpu_dag_test::FullSensor(48, 32));
  EXPECT_EQ(prepared.input_kind, RawInputKind::DebayeredRgb);
  EXPECT_EQ(prepared.CompileSource().kind, DevelopInputKind::DirectRgb);

  auto       document = CreateDefaultPipelineDocument();
  const auto plan     = GraphCompiler::Compile(document, prepared.CompileSource(), RenderRequest{});
  EXPECT_TRUE(plan.Contains(GpuPassKind::UploadRgb));
  EXPECT_FALSE(plan.Contains(GpuPassKind::UploadRaw));
  EXPECT_FALSE(plan.Contains(GpuPassKind::Linearize));
  EXPECT_FALSE(plan.Contains(GpuPassKind::Demosaic));
}

TEST(GpuDagRawInput, SonyDecodedRgbMapsBlackAndWhiteWithoutRepeatingWhiteBalance) {
  auto raw = std::make_unique<LibRaw>();
  auto& color = raw->imgdata.color;
  color.as_shot_wb_applied = LIBRAW_ASWB_APPLIED | LIBRAW_ASWB_SONY;
  color.black              = 1024;
  color.maximum            = 17536;
  for (int c = 0; c < 4; ++c) {
    color.linear_max[c] = 15360;
    color.cam_mul[c]    = c == 0 ? 2468.0f : 1024.0f;
  }
  for (int channels : {3, 4}) {
    SCOPED_TRACE(channels);
    // Use a non-contiguous ROI to also exercise row stride handling.
    cv::Mat      storage(2, 7, CV_MAKETYPE(CV_16U, channels), cv::Scalar::all(0));
    cv::Mat      source     = storage(cv::Rect(1, 0, 5, 2));
    const ushort samples[]  = {921, 1024, 9280, 17536, 19313};
    const float  expected[] = {0.0f, 0.0f, 0.5f, 1.0f, 18289.0f / 16512.0f};
    for (int y = 0; y < source.rows; ++y) {
      for (int x = 0; x < source.cols; ++x) {
        for (int c = 0; c < channels; ++c) source.ptr<ushort>(y)[x * channels + c] = samples[x];
      }
    }
    const auto result = raw_norm::ConvertUnpackedRgbToFloat(source, color);
    for (int y = 0; y < source.rows; ++y) {
      for (int x = 0; x < source.cols; ++x) {
        for (int c = 0; c < 3; ++c) {
          EXPECT_NEAR(result.ptr<float>(y)[x * channels + c], expected[x], 1e-6f);
        }
      }
    }
  }
}

TEST(GpuDagRawInput, SonyDecodedRgbRejectsInvalidRangeAndHonorsChannelBlackOffsets) {
  auto  raw                = std::make_unique<LibRaw>();
  auto& color              = raw->imgdata.color;
  color.as_shot_wb_applied = LIBRAW_ASWB_APPLIED | LIBRAW_ASWB_SONY;
  color.black              = 100;
  color.maximum            = 1100;
  color.cblack[1]          = 200;
  cv::Mat    source(1, 1, CV_16UC3, cv::Scalar(600, 700, 1100));
  const auto result = raw_norm::ConvertUnpackedRgbToFloat(source, color);
  EXPECT_FLOAT_EQ(result.at<cv::Vec3f>(0, 0)[0], 0.5f);
  EXPECT_FLOAT_EQ(result.at<cv::Vec3f>(0, 0)[1], 0.5f);
  EXPECT_FLOAT_EQ(result.at<cv::Vec3f>(0, 0)[2], 1.0f);
  color.maximum = 300;
  EXPECT_THROW(raw_norm::ConvertUnpackedRgbToFloat(source, color), std::runtime_error);
  color.maximum = 0;
  EXPECT_THROW(raw_norm::ConvertUnpackedRgbToFloat(source, color), std::runtime_error);
}

TEST(GpuDagRawInput, OtherIntegerRgbKeepsExistingFullRangeConversion) {
  auto raw                              = std::make_unique<LibRaw>();
  raw->imgdata.color.as_shot_wb_applied = LIBRAW_ASWB_APPLIED | LIBRAW_ASWB_CANON;
  cv::Mat    source(1, 1, CV_16UC3, cv::Scalar(0, 32768, 65535));
  const auto result = raw_norm::ConvertUnpackedRgbToFloat(source, raw->imgdata.color);
  EXPECT_FLOAT_EQ(result.at<cv::Vec3f>(0, 0)[0], 0.0f);
  EXPECT_NEAR(result.at<cv::Vec3f>(0, 0)[1], 32768.0f / 65535.0f, 1e-7f);
  EXPECT_FLOAT_EQ(result.at<cv::Vec3f>(0, 0)[2], 1.0f);
}

TEST(GpuDagRawInput, SonyA7CiiYcbcrFileLoadsNormalizedRgbAtFullResolution) {
  // The large camera fixture is optional; do not add private RAW files to source control.
  const char* override_path = std::getenv("ALCEDO_SONY_YCBCR_RAW");
  const auto  path          = override_path ? std::filesystem::path(override_path)
                                            : std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                        "sony" / "a7cii" / "ycbcr_compressed" / "DSC04739.ARW";
  if (!override_path && !std::filesystem::exists(path)) {
    GTEST_SKIP() << "Sony YCbCr fixture is missing: " << path;
  }
  const auto encoded = ReadBytes(path);
  ASSERT_FALSE(encoded.empty());
  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_buffer(const_cast<std::byte*>(encoded.data()), encoded.size()),
            LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
  ASSERT_NE(raw->imgdata.rawdata.color4_image, nullptr);
  ASSERT_EQ(raw->imgdata.color.as_shot_wb_applied, LIBRAW_ASWB_APPLIED | LIBRAW_ASWB_SONY);
  ASSERT_EQ(raw->imgdata.color.black, 1024U);
  ASSERT_EQ(raw->imgdata.color.maximum, 17536U);

  const auto prepared = RawInputLoader::LoadEncoded(encoded, DecodeRes::FULL);
  ASSERT_EQ(prepared.input_kind, RawInputKind::DebayeredRgb);
  ASSERT_EQ(prepared.pixels.format, HostPixelFormat::F32Rgba);
  EXPECT_EQ(prepared.linearization.apply_as_shot_wb, 0);
  // DSC04739's default image crop is (8, 4, 4608, 3072), inside the decode area.
  ASSERT_EQ(prepared.host_extent.width, 4608U);
  ASSERT_EQ(prepared.host_extent.height, 3072U);
  const auto* output = reinterpret_cast<const float*>(prepared.pixels.bytes.get());
  for (unsigned y = 0; y < prepared.host_extent.height; y += 17) {
    for (unsigned x = 0; x < prepared.host_extent.width; x += 19) {
      const auto* sample =
          raw->imgdata.rawdata
              .color4_image[(y + 4) * raw->imgdata.sizes.raw_width + x + 8];
      const auto* pixel = output + y * (prepared.pixels.stride_bytes / sizeof(float)) + x * 4;
      for (int c = 0; c < 3; ++c) {
        const float expected = std::max(0.0f, (static_cast<float>(sample[c]) - 1024.0f) / 16512.0f);
        EXPECT_NEAR(pixel[c], expected, 1e-6f);
        EXPECT_TRUE(std::isfinite(pixel[c]));
      }
      EXPECT_FLOAT_EQ(pixel[3], 1.0f);
    }
  }
}

TEST(GpuDagRawInput, UnsupportedCfaDoesNotProducePreparedRawInput) {
  RawCfaPattern bad;
  bad.kind                    = RawCfaKind::Bayer2x2;
  bad.bayer_pattern.raw_fc[0] = 0;
  bad.bayer_pattern.raw_fc[1] = 0;
  bad.bayer_pattern.raw_fc[2] = 0;
  bad.bayer_pattern.raw_fc[3] = 0;
  EXPECT_THROW(
      {
        auto prepared = RawInputLoader::FromUnpackedCfa(
            gpu_dag_test::MakeU16CfaPlane(16, 16, bad), bad, gpu_dag_test::DefaultLinearization(),
            gpu_dag_test::FullSensor(16, 16), DecodeRes::FULL);
        (void)prepared;
      },
      std::runtime_error);

  std::byte empty{};
  EXPECT_FALSE(
      RawInputLoader::TryLoadEncoded(std::span<const std::byte>(&empty, 0), DecodeRes::FULL)
          .has_value());
}

TEST(GpuDagRawInput, InputHeadersDoNotIncludeGpuOrImageBuffer) {
  const auto root = std::filesystem::path{ALCEDO_INPUT_HEADER_ROOT};
  ASSERT_TRUE(std::filesystem::exists(root));
  for (const auto& entry : std::filesystem::directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.path().extension() != ".hpp") {
      continue;
    }
    std::ifstream input(entry.path());
    std::string   line;
    while (std::getline(input, line)) {
      EXPECT_EQ(line.find("image_buffer.hpp"), std::string::npos) << entry.path();
      EXPECT_EQ(line.find("cuda_runtime"), std::string::npos) << entry.path();
    }
  }
}

TEST(GpuDagRawInput, EncodedDngCarriesOpcodeList3WarpIntoPreparedInput) {
  const auto root = std::filesystem::path{ALCEDO_CI_RAW_FIXTURE_ROOT};
  const auto path = root / "tag @ryanbreitkreutz - free raws from @signatureeditsco - DSC06683.dng";
  const auto encoded = ReadBytes(path);
  ASSERT_FALSE(encoded.empty()) << path;

  const auto prepared = RawInputLoader::LoadEncoded(encoded, DecodeRes::FULL);
  ASSERT_TRUE(prepared.dng_warp_rectilinear.has_value());
  EXPECT_GT(prepared.dng_warp_rectilinear->coefficient_set_count, 0U);
  EXPECT_TRUE(prepared.color_context.dng_warp_rectilinear_present_);
  EXPECT_NE(prepared.source_key.dng_warp_hash, 0U);
}

#if defined(TEST_IMG_PATH)
TEST(GpuDagRawInput, LoadEncodedUnpacksLinearDngAsDirectRgbAndHonorsDecodeRes) {
  const auto path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "linear_dng" / "mfzoty.dng";
  const auto encoded = ReadBytes(path);
  if (encoded.empty()) {
    GTEST_SKIP() << "Sample linear DNG is missing: " << path.string();
  }

  const auto full    = RawInputLoader::LoadEncoded(encoded, DecodeRes::FULL);
  const auto eighth  = RawInputLoader::LoadEncoded(encoded, DecodeRes::EIGHTH);
  EXPECT_EQ(full.input_kind, RawInputKind::DebayeredRgb);
  EXPECT_EQ(full.pixels.format, HostPixelFormat::F32Rgba);
  EXPECT_EQ(full.CompileSource().kind, DevelopInputKind::DirectRgb);
  EXPECT_EQ(eighth.downsample_passes, 3);
  EXPECT_EQ(eighth.full_reference_extent, full.full_reference_extent);
  EXPECT_LT(eighth.host_extent.width, full.host_extent.width);
  EXPECT_FALSE(eighth.host_extent.Empty());
}
#endif

#if defined(__APPLE__)
TEST(GpuDagRawInput, EncodedRawLoadCompletesOnProductWorkerStack) {
  const auto root = std::filesystem::path{ALCEDO_CI_RAW_FIXTURE_ROOT};
  const auto path = root / "tag @ryanbreitkreutz - free raws from @signatureeditsco - DSC06683.dng";
  const auto encoded = ReadBytes(path);
  ASSERT_FALSE(encoded.empty()) << path;

  pthread_attr_t attributes;
  ASSERT_EQ(pthread_attr_init(&attributes), 0);
  constexpr std::size_t kProductWorkerStackBytes = 512U * 1024U;
  const int set_stack_result = pthread_attr_setstacksize(&attributes, kProductWorkerStackBytes);
  if (set_stack_result != 0) {
    pthread_attr_destroy(&attributes);
    FAIL() << "pthread_attr_setstacksize failed: " << set_stack_result;
  }

  ProductWorkerRawLoadContext context{.encoded = &encoded};
  pthread_t                   thread;
  const int                   create_result =
      pthread_create(&thread, &attributes, LoadEncodedOnProductWorkerStack, &context);
  pthread_attr_destroy(&attributes);
  ASSERT_EQ(create_result, 0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);

  if (context.failure) {
    try {
      std::rethrow_exception(context.failure);
    } catch (const std::exception& error) {
      FAIL() << error.what();
    } catch (...) {
      FAIL() << "RawInputLoader failed with a non-standard exception";
    }
  }
  ASSERT_TRUE(context.prepared.has_value());
  EXPECT_FALSE(context.prepared->host_extent.Empty());
}
#endif

}  // namespace
}  // namespace alcedo
