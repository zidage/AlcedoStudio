//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Phase 6: OpenCL Neural RAW routing, same-backend Legacy fallback, and product geometry.

#include <gtest/gtest.h>
#include <libraw/libraw.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include <opencv2/core.hpp>

#include "decoders/processor/nn/demosaicnet_preprocess_common.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_cache.hpp"
#include "decoders/processor/operators/gpu/opencl_demosaicnet.hpp"
#include "decoders/processor/raw_processor.hpp"
#include "decoders/processor/raw_processor_internal.hpp"
#include "image/opencl_image.hpp"
#include "opencl/nn/convolution.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

auto EnsureOpenCl() -> bool {
  if (TryPrepareOpenClRuntime()) {
    return true;
  }
  return OpenClContext::Instance().IsInitialized();
}

void RequireOpenCl() {
  if (!EnsureOpenCl()) {
    const std::string error = OpenClContext::Instance().LastInitializationError();
    GTEST_SKIP() << (error.empty() ? "OpenCL runtime unavailable." : error);
  }
}

void SetDemosaicModelDirEnv(const char* value) {
#if defined(_WIN32)
  _putenv_s("ALCEDO_DEMOASICNET_MODEL_DIR", value == nullptr ? "" : value);
#else
  if (value == nullptr) {
    unsetenv("ALCEDO_DEMOASICNET_MODEL_DIR");
  } else {
    setenv("ALCEDO_DEMOASICNET_MODEL_DIR", value, 1);
  }
#endif
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

}  // namespace

// Purpose: OpenCL Neural demosaics a real Bayer RAW patch to finite RGBA with Neural geometry.
TEST(OpenClRawNeuralTest, NeuralEngineDemosaicsRealBayerRawToFiniteRgbWithExpectedDimensions) {
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#else
  RequireOpenCl();

  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  if (!std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "Bayer RAW fixture missing: " << raw_path.string();
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  constexpr int    kPatch = 64;
  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, kPatch);

  RawParams params;
  params.gpu_backend_            = RawGpuBackend::OpenCL;
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

  auto& cache = OpenClDemosaicNetModelCache::Instance();
  cache.Unload(OpenClDemosaicNetVariant::Bayer);
  OpenCL::ResetOpenClNeuralPathCountersForTest();

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  ASSERT_TRUE(cache.IsLoaded(OpenClDemosaicNetVariant::Bayer));
  EXPECT_EQ(OpenCL::OpenClNeuralSuccessCountForTest(), 1u);
  EXPECT_EQ(OpenCL::OpenClNeuralLegacyFallbackCountForTest(), 0u);
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::OpenCL);

  auto& gpu = output.GetOpenClImage();
  EXPECT_EQ(gpu.Type(), CV_32FC4);
  EXPECT_EQ(cv::Size(gpu.Width(), gpu.Height()), expected_crop.size());

  cv::Mat host;
  gpu.Download(host);
  EXPECT_TRUE(AllFiniteRgba(host));
  raw->recycle();
#endif
}

// Purpose: OpenCL Neural demosaics a real X-Trans RAW patch to finite RGBA with Neural geometry.
TEST(OpenClRawNeuralTest, NeuralEngineDemosaicsRealXTransRawToFiniteRgbWithExpectedDimensions) {
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#else
  RequireOpenCl();

  const std::filesystem::path raw_path =
      std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" / "fuji" / "xt5" / "DSCF2074.RAF";
  if (!std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "X-Trans RAW fixture missing: " << raw_path.string();
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->imgdata.idata.filters, 9U);

  constexpr int    kPatch = 64;
  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, kPatch);

  RawParams params;
  params.gpu_backend_            = RawGpuBackend::OpenCL;
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

  auto& cache = OpenClDemosaicNetModelCache::Instance();
  cache.Unload(OpenClDemosaicNetVariant::XTrans);
  OpenCL::ResetOpenClNeuralPathCountersForTest();

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  ASSERT_TRUE(cache.IsLoaded(OpenClDemosaicNetVariant::XTrans));
  EXPECT_EQ(OpenCL::OpenClNeuralSuccessCountForTest(), 1u);
  EXPECT_EQ(OpenCL::OpenClNeuralLegacyFallbackCountForTest(), 0u);
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::OpenCL);

  auto& gpu = output.GetOpenClImage();
  EXPECT_EQ(gpu.Type(), CV_32FC4);
  EXPECT_EQ(cv::Size(gpu.Width(), gpu.Height()), expected_crop.size());

  cv::Mat host;
  gpu.Download(host);
  EXPECT_TRUE(AllFiniteRgba(host));
  raw->recycle();
#endif
}

// Purpose: injected model-load failure soft-fails to OpenCL Legacy and never leaves OpenCL.
TEST(OpenClRawNeuralTest, InjectedModelLoadFailureFallsBackToOpenClLegacyAndDoesNotEnterCuda) {
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#else
  RequireOpenCl();

  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  if (!std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "Bayer RAW fixture missing: " << raw_path.string();
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, 64);
  RawParams        params;
  params.gpu_backend_            = RawGpuBackend::OpenCL;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  auto& cache = OpenClDemosaicNetModelCache::Instance();
  cache.Unload(OpenClDemosaicNetVariant::Bayer);
  OpenCL::ResetOpenClNeuralPathCountersForTest();
  SetDemosaicModelDirEnv("definitely_missing_demosaicnet_models");

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();
  SetDemosaicModelDirEnv(nullptr);

  EXPECT_FALSE(cache.IsLoaded(OpenClDemosaicNetVariant::Bayer));
  EXPECT_EQ(OpenCL::OpenClNeuralLegacyFallbackCountForTest(), 1u);
  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::OpenCL);
  // RCD Legacy on 64×64 loses a 4-pixel border on each side → 56×56.
  EXPECT_EQ(cv::Size(output.GetOpenClImage().Width(), output.GetOpenClImage().Height()),
            cv::Size(56, 56));
  raw->recycle();
#endif
}

// Purpose: injected enqueue failure falls back to OpenCL Legacy from untouched linear CFA.
TEST(OpenClRawNeuralTest, InjectedEnqueueFailureFallsBackToOpenClLegacyFromUntouchedLinearCfa) {
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#else
  RequireOpenCl();

  // Build a synthetic linear CFA large enough for Neural prepare, then inject enqueue failure.
  constexpr int kSize = 64;
  cv::Mat       mono(kSize, kSize, CV_32FC1);
  for (int y = 0; y < kSize; ++y) {
    float* row = mono.ptr<float>(y);
    for (int x = 0; x < kSize; ++x) {
      row[x] = 0.15f + 0.001f * static_cast<float>((3 * y + 5 * x) % 17);
    }
  }

  opencl::OpenClImage linear_cfa;
  linear_cfa.Upload(mono);
  // Snapshot for post-failure comparison: Neural must not mutate the caller's CFA.
  opencl::OpenClImage linear_copy;
  linear_cfa.CopyTo(linear_copy);

  RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  opencl::OpenClImage rgb_rgba;
  OpenCL::OpenClNeuralDemosaicOptions options;
  options.injected_failure = OpenCL::OpenClNeuralInjectedFailure::Enqueue;

  OpenClDemosaicNetModelCache cache;
  // Ensure a real model would load so the injected enqueue stage is the one that fires.
  OpenClDemosaicNetLoadOptions load_opts;
#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  load_opts.model_dir = ALCEDO_DEMOASICNET_MODEL_DIR;
#endif
  options.model_cache  = &cache;
  options.load_options = load_opts;

  // If models are missing, load failure is also a valid soft-fail path; prefer enqueue inject.
  const auto result = OpenCL::DemosaicWithNeuralEngine(linear_cfa, pattern, rgb_rgba, options);
  EXPECT_FALSE(result.succeeded);
  EXPECT_FALSE(result.error.empty());
  EXPECT_NE(result.error.find("stage="), std::string::npos);
  EXPECT_TRUE(rgb_rgba.Empty()) << "soft-fail must leave destination untouched";

  // Original linear CFA content must still match the pre-Neural snapshot.
  cv::Mat after;
  cv::Mat before;
  cv::Mat diff;
  linear_cfa.Download(after);
  linear_copy.Download(before);
  ASSERT_EQ(after.size(), before.size());
  ASSERT_EQ(after.type(), before.type());
  double max_diff = 0.0;
  cv::absdiff(after, before, diff);
  cv::minMaxLoc(diff, nullptr, &max_diff);
  EXPECT_LE(max_diff, 0.0);
#endif
}

// Purpose: successful Neural path records success without Legacy fallback counter.
TEST(OpenClRawNeuralTest, SuccessfulNeuralExecutionDoesNotInvokeLegacy) {
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#else
  RequireOpenCl();

  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  if (!std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "Bayer RAW fixture missing: " << raw_path.string();
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, 64);
  RawParams        params;
  params.gpu_backend_            = RawGpuBackend::OpenCL;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = false;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  OpenCL::ResetOpenClNeuralPathCountersForTest();
  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  EXPECT_GE(OpenCL::OpenClNeuralSuccessCountForTest(), 1u);
  EXPECT_EQ(OpenCL::OpenClNeuralLegacyFallbackCountForTest(), 0u);
  // Neural student geometry is not the RCD 56×56 Legacy size.
  EXPECT_NE(cv::Size(output.GetOpenClImage().Width(), output.GetOpenClImage().Height()),
            cv::Size(56, 56));
  raw->recycle();
#endif
}

// Purpose: product demosaic waits only at the Neural-stage boundary (no per-tile host wait).
TEST(OpenClRawNeuralTest, MultiTileDecodePerformsNoPerTileHostWaitBeyondFinalBoundary) {
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#else
  RequireOpenCl();

  // 2048×2048 is multi-tile for Bayer student jobs (3×3 grid). Use a synthetic mono frame
  // with training GRBG origin so prepare succeeds without a full RAW decode.
  constexpr int kSize = 2048;
  cv::Mat       mono(kSize, kSize, CV_32FC1, cv::Scalar(0.12f));
  opencl::OpenClImage linear_cfa;
  linear_cfa.Upload(mono);

  RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  opencl::OpenClImage rgb_rgba;
  OpenCL::OpenClNeuralDemosaicOptions options;
  OpenClDemosaicNetModelCache         cache;
  OpenClDemosaicNetLoadOptions        load_opts;
#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  load_opts.model_dir = ALCEDO_DEMOASICNET_MODEL_DIR;
#endif
  options.model_cache  = &cache;
  options.load_options = load_opts;

  opencl::nn::ResetDispatchInstrumentation();
  OpenCL::ResetOpenClNeuralPathCountersForTest();

  const auto result = OpenCL::DemosaicWithNeuralEngine(linear_cfa, pattern, rgb_rgba, options);
  if (!result.succeeded) {
    GTEST_SKIP() << "Neural demosaic unavailable: " << result.error;
  }

  EXPECT_GE(result.tile_count, 2u);
  // Product path records exactly one host-wait counter bump at the Neural stage.
  EXPECT_EQ(OpenCL::OpenClNeuralHostWaitCountForTest(), 1u);
  EXPECT_EQ(rgb_rgba.Type(), CV_32FC4);
  EXPECT_EQ(cv::Size(rgb_rgba.Width(), rgb_rgba.Height()), cv::Size(kSize, kSize));
#endif
}

// Purpose: HLR-enabled Neural output remains OpenCL-backed RGBA with finite values.
TEST(OpenClRawNeuralTest, HighlightReconstructionEnabledProducesFiniteOpenClRgba) {
#ifndef HAVE_OPENCL
  GTEST_SKIP() << "OpenCL is not enabled in this build.";
#else
  RequireOpenCl();

  const std::filesystem::path raw_path = std::filesystem::path(TEST_IMG_PATH) / "raw" / "camera" /
                                         "nikon" / "d800e" / "Nikon-D800e-raw-00002.nef";
  if (!std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "Bayer RAW fixture missing: " << raw_path.string();
  }

  auto raw = std::make_unique<LibRaw>();
  ASSERT_EQ(raw->open_file(raw_path.string().c_str()), LIBRAW_SUCCESS);
  ASSERT_EQ(raw->unpack(), LIBRAW_SUCCESS);

  cv::Mat          patch;
  libraw_rawdata_t patch_data = MakeRawPatchData(*raw, patch, 64);
  RawParams        params;
  params.gpu_backend_            = RawGpuBackend::OpenCL;
  params.demosaic_method_        = RawDemosaicMethod::NeuralEngine;
  params.highlights_reconstruct_ = true;
  params.decode_res_             = DecodeRes::FULL;
  RawRuntimeColorContext context;
  const ushort           no_crop[4] = {};

  RawProcessor processor(params, patch_data, *raw, context, no_crop);
  ImageBuffer  output = processor.Process();

  ASSERT_TRUE(output.gpu_data_valid_);
  EXPECT_EQ(output.GetGPUBackend(), GpuBackendKind::OpenCL);
  EXPECT_EQ(output.GetOpenClImage().Type(), CV_32FC4);

  cv::Mat host;
  output.GetOpenClImage().Download(host);
  EXPECT_TRUE(AllFiniteRgba(host));
  raw->recycle();
#endif
}

}  // namespace alcedo
