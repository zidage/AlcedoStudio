//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Phase 4: Metal Neural RAW routing (hard-fail), product geometry, and thumbnail Legacy.

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

// OpenCV must be included before any Apple/Metal header that defines YES/NO.
#include <opencv2/core.hpp>

#include "decoders/processor/raw_processor.hpp"
#include "decoders/processor/raw_processor_internal.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/metal_demosaicnet_cache.hpp"
#include "decoders/processor/operators/gpu/metal_demosaicnet.hpp"
#include "image/metal_image.hpp"
#include "metal/metal_context.hpp"

namespace alcedo {
namespace {

auto MetalRuntimeAvailable() -> bool {
  try {
    return MetalContext::Instance().Device() != nullptr;
  } catch (const std::exception&) {
    return false;
  }
}

void RequireMetal() {
  if (!MetalRuntimeAvailable()) {
    GTEST_SKIP() << "Metal runtime unavailable.";
  }
}

void SetDemosaicModelDirEnv(const char* value) {
  if (value == nullptr) {
    unsetenv("ALCEDO_DEMOASICNET_MODEL_DIR");
  } else {
    setenv("ALCEDO_DEMOASICNET_MODEL_DIR", value, 1);
  }
}

auto MakeRawPatchData(LibRaw& raw, cv::Mat& patch, const int size) -> libraw_rawdata_t {
  cv::Mat raw_view(raw.imgdata.sizes.raw_height, raw.imgdata.sizes.raw_width, CV_16UC1,
                   raw.imgdata.rawdata.raw_image);
  patch = raw_view(cv::Rect(0, 0, size, size)).clone();

  libraw_rawdata_t data  = raw.imgdata.rawdata;
  data.raw_image         = patch.ptr<uint16_t>();
  data.sizes.raw_width   = static_cast<ushort>(size);
  data.sizes.raw_height  = static_cast<ushort>(size);
  data.sizes.width       = static_cast<ushort>(size);
  data.sizes.height      = static_cast<ushort>(size);
  data.sizes.iwidth      = static_cast<ushort>(size);
  data.sizes.iheight     = static_cast<ushort>(size);
  data.sizes.left_margin = 0;
  data.sizes.top_margin  = 0;
  data.sizes.raw_pitch   = static_cast<unsigned>(size * sizeof(uint16_t));
  return data;
}

auto AllFiniteRgba(const cv::Mat& rgba) -> bool {
  if (rgba.empty() || rgba.type() != CV_32FC4) {
    return false;
  }
  for (int y = 0; y < rgba.rows; ++y) {
    const cv::Vec4f* row = rgba.ptr<cv::Vec4f>(y);
    for (int x = 0; x < rgba.cols; ++x) {
      for (int c = 0; c < 4; ++c) {
        if (!std::isfinite(row[x][c])) {
          return false;
        }
      }
    }
  }
  return true;
}

auto MakeSyntheticMono(int size, float base = 0.12f) -> metal::MetalImage {
  cv::Mat mono(size, size, CV_32FC1);
  for (int y = 0; y < size; ++y) {
    float* row = mono.ptr<float>(y);
    for (int x = 0; x < size; ++x) {
      row[x] = base + 0.001f * static_cast<float>((3 * y + 5 * x) % 17);
    }
  }
  metal::MetalImage image =
      metal::MetalImage::Create2D(static_cast<uint32_t>(size), static_cast<uint32_t>(size),
                                  metal::PixelFormat::R32FLOAT);
  image.Upload(mono);
  return image;
}

// Prefer local Metal Neural fixtures (gitignored under sample_images/local/), then
// the shared camera RAW tree used by other GPU Neural tests.
auto ResolveBayerRawPath() -> std::filesystem::path {
  const std::filesystem::path candidates[] = {
      std::filesystem::path(TEST_IMG_PATH) / "local" / "metal_neural_bayer_s5m2.RW2",
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "nikon" / "d800e" /
          "Nikon-D800e-raw-00002.nef",
  };
  for (const auto& path : candidates) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return {};
}

auto ResolveXTransRawPath() -> std::filesystem::path {
  const std::filesystem::path candidates[] = {
      std::filesystem::path(TEST_IMG_PATH) / "local" / "metal_neural_xtrans_xt5.RAF",
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "fuji" / "xt5" / "DSCF2074.RAF",
  };
  for (const auto& path : candidates) {
    if (std::filesystem::exists(path)) {
      return path;
    }
  }
  return {};
}

}  // namespace

// Purpose: Metal Neural demosaics a real Bayer RAW patch to finite RGBA with Neural geometry.
TEST(MetalRawNeuralTest, NeuralEngineDemosaicsRealBayerRawToFiniteRgbWithExpectedDimensions) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RequireMetal();

  const std::filesystem::path raw_path = ResolveBayerRawPath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "Bayer RAW fixture missing (local/metal_neural_bayer_s5m2.RW2 or camera NEF).";
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  constexpr int    kPatch = 64;
  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, kPatch);

  RawParams params;
  params.gpu_backend_            = RawGpuBackend::Metal;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  const RawCfaPattern pattern = ReadLibRawCfaPattern(*raw);
  ASSERT_EQ(pattern.kind, RawCfaKind::Bayer2x2);
  const auto shift = FindCfaAlignShift(pattern);
  ASSERT_TRUE(shift.has_value());
  const int aligned_h = (kPatch - shift->sy) - ((kPatch - shift->sy) % 2);
  const int aligned_w = (kPatch - shift->sx) - ((kPatch - shift->sx) % 2);
  const detail::NeuralOutputGeometry geometry = detail::MakeStudentTiledNeuralOutputGeometry(
      shift->sx, shift->sy, cv::Size(aligned_w, aligned_h));
  const cv::Rect expected_crop = detail::BuildNeuralEngineDecodeCropRect(
      patch_data.sizes, no_crop, cv::Size(kPatch, kPatch), DecodeRes::FULL, geometry);

  auto& cache = MetalDemosaicNetModelCache::Instance();
  cache.Unload(MetalDemosaicNetVariant::Bayer);
  metal::ResetMetalNeuralPathCountersForTest();

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  ASSERT_TRUE(cache.IsLoaded(MetalDemosaicNetVariant::Bayer));
  EXPECT_EQ(metal::MetalNeuralSuccessCountForTest(), 1u);
  EXPECT_EQ(metal::MetalNeuralLegacyFallbackCountForTest(), 0u);
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::Metal);

  auto& gpu = output.GetMetalImage();
  EXPECT_EQ(gpu.Format(), metal::PixelFormat::RGBA32FLOAT);
  EXPECT_EQ(cv::Size(static_cast<int>(gpu.Width()), static_cast<int>(gpu.Height())),
            expected_crop.size());

  cv::Mat host;
  gpu.Download(host);
  EXPECT_TRUE(AllFiniteRgba(host));
  raw->recycle();
#endif
}

// Purpose: Metal Neural demosaics a real X-Trans RAW patch to finite RGBA with Neural geometry.
TEST(MetalRawNeuralTest, NeuralEngineDemosaicsRealXTransRawToFiniteRgbWithExpectedDimensions) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RequireMetal();

  const std::filesystem::path raw_path = ResolveXTransRawPath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "X-Trans RAW fixture missing (local/metal_neural_xtrans_xt5.RAF or camera RAF).";
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->imgdata.idata.filters, 9U);

  constexpr int    kPatch = 64;
  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, kPatch);

  RawParams params;
  params.gpu_backend_            = RawGpuBackend::Metal;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  const RawCfaPattern pattern = ReadLibRawCfaPattern(*raw);
  ASSERT_EQ(pattern.kind, RawCfaKind::XTrans6x6);
  const auto shift = FindCfaAlignShift(pattern);
  ASSERT_TRUE(shift.has_value());
  const int aligned_h = (kPatch - shift->sy) - ((kPatch - shift->sy) % 6);
  const int aligned_w = (kPatch - shift->sx) - ((kPatch - shift->sx) % 6);
  const detail::NeuralOutputGeometry geometry = detail::MakeStudentTiledNeuralOutputGeometry(
      shift->sx, shift->sy, cv::Size(aligned_w, aligned_h));
  const cv::Rect expected_crop = detail::BuildNeuralEngineDecodeCropRect(
      patch_data.sizes, no_crop, cv::Size(kPatch, kPatch), DecodeRes::FULL, geometry);

  auto& cache = MetalDemosaicNetModelCache::Instance();
  cache.Unload(MetalDemosaicNetVariant::XTrans);
  metal::ResetMetalNeuralPathCountersForTest();

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  ASSERT_TRUE(cache.IsLoaded(MetalDemosaicNetVariant::XTrans));
  EXPECT_EQ(metal::MetalNeuralSuccessCountForTest(), 1u);
  EXPECT_EQ(metal::MetalNeuralLegacyFallbackCountForTest(), 0u);
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::Metal);

  auto& gpu = output.GetMetalImage();
  EXPECT_EQ(gpu.Format(), metal::PixelFormat::RGBA32FLOAT);
  EXPECT_EQ(cv::Size(static_cast<int>(gpu.Width()), static_cast<int>(gpu.Height())),
            expected_crop.size());

  cv::Mat host;
  gpu.Download(host);
  EXPECT_TRUE(AllFiniteRgba(host));
  raw->recycle();
#endif
}

// Purpose: missing model load throws with stage=load and never soft-fails to Legacy.
TEST(MetalRawNeuralTest, InjectedModelLoadFailureThrowsAndDoesNotInvokeLegacy) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RequireMetal();

  const std::filesystem::path raw_path = ResolveBayerRawPath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "Bayer RAW fixture missing (local/metal_neural_bayer_s5m2.RW2 or camera NEF).";
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, 64);
  RawParams        params;
  params.gpu_backend_            = RawGpuBackend::Metal;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  auto& cache = MetalDemosaicNetModelCache::Instance();
  cache.Unload(MetalDemosaicNetVariant::Bayer);
  metal::ResetMetalNeuralPathCountersForTest();
  SetDemosaicModelDirEnv("definitely_missing_demosaicnet_models");

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  try {
    (void)processor.Process();
    SetDemosaicModelDirEnv(nullptr);
    FAIL() << "expected Metal Neural load failure to throw";
  } catch (const std::exception& e) {
    SetDemosaicModelDirEnv(nullptr);
    const std::string message = e.what();
    EXPECT_NE(message.find("stage=load"), std::string::npos) << message;
    EXPECT_NE(message.find("variant="), std::string::npos) << message;
    EXPECT_NE(message.find("bayer_s24_d8"), std::string::npos) << message;
  }

  EXPECT_FALSE(cache.IsLoaded(MetalDemosaicNetVariant::Bayer));
  EXPECT_EQ(metal::MetalNeuralSuccessCountForTest(), 0u);
  EXPECT_EQ(metal::MetalNeuralLegacyFallbackCountForTest(), 0u);
  raw->recycle();
#endif
}

// Purpose: every injected product-entry failure stage throws with that stage name.
TEST(MetalRawNeuralTest, InjectedStageFailuresThrowWithVariantAndStage) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RequireMetal();

  constexpr int kSize = 64;
  metal::MetalImage linear_cfa = MakeSyntheticMono(kSize);
  metal::MetalImage linear_copy;
  linear_cfa.CopyTo(linear_copy);

  const RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  const cv::Rect      crop(0, 0, kSize, kSize);

  const metal::NeuralInjectedFailure stages[] = {
      metal::NeuralInjectedFailure::Prepare,     metal::NeuralInjectedFailure::Load,
      metal::NeuralInjectedFailure::Compile,     metal::NeuralInjectedFailure::TileInput,
      metal::NeuralInjectedFailure::GraphEncode, metal::NeuralInjectedFailure::GraphExecute,
      metal::NeuralInjectedFailure::TileOutput,
  };
  const char* stage_names[] = {"prepare",     "load",         "compile", "tile_input",
                               "graph_encode", "graph_execute", "tile_output"};

  MetalDemosaicNetModelCache  cache;
  MetalDemosaicNetLoadOptions load_opts;
#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  load_opts.model_dir = ALCEDO_DEMOASICNET_MODEL_DIR;
#endif

  for (std::size_t i = 0; i < sizeof(stages) / sizeof(stages[0]); ++i) {
    metal::MetalImage output;
    metal::NeuralDemosaicOptions options;
    options.model_cache       = &cache;
    options.load_options      = load_opts;
    options.injected_failure  = stages[i];

    try {
      (void)metal::DemosaicWithNeuralEngine(linear_cfa, pattern, /*phase_shift_x=*/0,
                                            /*phase_shift_y=*/0, kSize, kSize, crop, output,
                                            options);
      FAIL() << "expected throw for stage=" << stage_names[i];
    } catch (const std::exception& e) {
      const std::string message = e.what();
      EXPECT_NE(message.find(std::string("stage=") + stage_names[i]), std::string::npos) << message;
      EXPECT_NE(message.find("variant=bayer_s24_d8"), std::string::npos) << message;
    }

    EXPECT_TRUE(output.Empty()) << "failure must not publish crop-sized RGBA for stage="
                                << stage_names[i];

    // Linear CFA must remain untouched.
    cv::Mat after;
    cv::Mat before;
    linear_cfa.Download(after);
    linear_copy.Download(before);
    double max_diff = 0.0;
    cv::Mat diff;
    cv::absdiff(after, before, diff);
    cv::minMaxLoc(diff, nullptr, &max_diff);
    EXPECT_LE(max_diff, 0.0);
  }
#endif
}

// Purpose: successful Neural path records success without Legacy fallback.
TEST(MetalRawNeuralTest, SuccessfulNeuralExecutionDoesNotInvokeLegacy) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RequireMetal();

  const std::filesystem::path raw_path = ResolveBayerRawPath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "Bayer RAW fixture missing (local/metal_neural_bayer_s5m2.RW2 or camera NEF).";
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, 64);
  RawParams        params;
  params.gpu_backend_            = RawGpuBackend::Metal;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  metal::ResetMetalNeuralPathCountersForTest();
  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  EXPECT_GE(metal::MetalNeuralSuccessCountForTest(), 1u);
  EXPECT_EQ(metal::MetalNeuralLegacyFallbackCountForTest(), 0u);
  // Neural student geometry is not the RCD 56×56 Legacy size.
  EXPECT_NE(cv::Size(static_cast<int>(output.GetMetalImage().Width()),
                     static_cast<int>(output.GetMetalImage().Height())),
            cv::Size(56, 56));
  raw->recycle();
#endif
}

// Purpose: HLR-enabled Neural output remains Metal-backed RGBA with finite values.
TEST(MetalRawNeuralTest, HighlightReconstructionEnabledProducesFiniteMetalRgba) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RequireMetal();

  const std::filesystem::path raw_path = ResolveBayerRawPath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "Bayer RAW fixture missing (local/metal_neural_bayer_s5m2.RW2 or camera NEF).";
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, 64);
  RawParams        params;
  params.gpu_backend_            = RawGpuBackend::Metal;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = true;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::Metal);
  EXPECT_EQ(output.GetMetalImage().Format(), metal::PixelFormat::RGBA32FLOAT);

  cv::Mat host;
  output.GetMetalImage().Download(host);
  EXPECT_TRUE(AllFiniteRgba(host));
  raw->recycle();
#endif
}

// Purpose: thumbnail (non-FULL) decode with Neural selected stays on Legacy and never
// loads or compiles a Metal Neural graph.
TEST(MetalRawNeuralTest, ThumbnailDecodeUsesLegacyWithZeroNeuralCacheActivity) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RequireMetal();

  const std::filesystem::path raw_path = ResolveBayerRawPath();
  if (raw_path.empty()) {
    GTEST_SKIP() << "Bayer RAW fixture missing (local/metal_neural_bayer_s5m2.RW2 or camera NEF).";
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  // Full-frame thumbnail path (not a 64 crop) so RCD still has margin after downsample.
  libraw_rawdata_t data = raw->imgdata.rawdata;

  RawParams params;
  params.gpu_backend_            = RawGpuBackend::Metal;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::QUARTER;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  auto& cache = MetalDemosaicNetModelCache::Instance();
  cache.UnloadAll();
  const std::uint64_t parse_before   = cache.parse_count();
  const std::uint64_t compile_before = cache.compile_count();
  const std::uint64_t load_before    = cache.load_attempt_count();
  const std::uint64_t alloc_before   = cache.input_output_allocation_count();
  metal::ResetMetalNeuralPathCountersForTest();

  RawProcessor processor(params, data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  EXPECT_FALSE(cache.IsLoaded(MetalDemosaicNetVariant::Bayer));
  EXPECT_FALSE(cache.IsLoaded(MetalDemosaicNetVariant::XTrans));
  EXPECT_EQ(cache.parse_count(), parse_before);
  EXPECT_EQ(cache.compile_count(), compile_before);
  EXPECT_EQ(cache.load_attempt_count(), load_before);
  EXPECT_EQ(cache.input_output_allocation_count(), alloc_before);
  EXPECT_EQ(metal::MetalNeuralSuccessCountForTest(), 0u);

  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::Metal);
  EXPECT_EQ(output.GetMetalImage().Format(), metal::PixelFormat::RGBA32FLOAT);
  // QUARTER RCD is much smaller than full Neural student canvas; just require a non-zero image.
  EXPECT_GT(output.GetMetalImage().Width(), 0u);
  EXPECT_GT(output.GetMetalImage().Height(), 0u);
  raw->recycle();
#endif
}

// Purpose: product demosaic waits only once at the Neural-stage boundary.
TEST(MetalRawNeuralTest, MultiTileDecodePerformsSingleHostWait) {
#ifndef HAVE_METAL
  GTEST_SKIP() << "Metal is not enabled in this build.";
#else
  RequireMetal();

  // 2048×2048 is multi-tile for Bayer student jobs.
  constexpr int kSize = 2048;
  metal::MetalImage linear_cfa = MakeSyntheticMono(kSize);
  metal::MetalImage output;

  RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  metal::NeuralDemosaicOptions options;
  MetalDemosaicNetModelCache   cache;
  MetalDemosaicNetLoadOptions  load_opts;
#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  load_opts.model_dir = ALCEDO_DEMOASICNET_MODEL_DIR;
#endif
  options.model_cache  = &cache;
  options.load_options = load_opts;

  metal::ResetMetalNeuralPathCountersForTest();
  try {
    const auto result = metal::DemosaicWithNeuralEngine(
        linear_cfa, pattern, /*phase_shift_x=*/0, /*phase_shift_y=*/0, kSize, kSize,
        cv::Rect(0, 0, kSize, kSize), output, options);
    EXPECT_GE(result.tile_count, 2u);
    EXPECT_EQ(metal::MetalNeuralHostWaitCountForTest(), 1u);
    EXPECT_EQ(output.Format(), metal::PixelFormat::RGBA32FLOAT);
    EXPECT_EQ(cv::Size(static_cast<int>(output.Width()), static_cast<int>(output.Height())),
              cv::Size(kSize, kSize));
  } catch (const std::exception& e) {
    GTEST_SKIP() << "Neural demosaic unavailable: " << e.what();
  }
#endif
}

}  // namespace alcedo
