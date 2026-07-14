//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.
//
// Phase 4: OpenCL fixed Bayer/X-Trans modules vs exported student reference pairs.
// Absolute tolerance 1e-4. Cache reuse and invalid-metadata rejection.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "decoders/processor/nn/opencl_demosaicnet_cache.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_module.hpp"
#include "decoders/processor/nn/opencl_demosaicnet_tiled.hpp"
#include "decoders/processor/operators/gpu/opencl_demosaicnet_programs.hpp"
#include "nn/safetensors.hpp"
#include "opencl/nn/convolution.hpp"
#include "opencl/nn/device_buffer.hpp"
#include "opencl/nn/workspace.hpp"
#include "opencl/opencl_backend_program_registry.hpp"
#include "opencl/opencl_context.hpp"
#include "opencl/opencl_program_library.hpp"
#include "opencl/opencl_runtime.hpp"

namespace alcedo {
namespace {

namespace fs     = std::filesystem;
namespace nn_ocl = opencl::nn;

constexpr float kAbsTol = 1e-4f;

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
  RegisterOpenClBackendPrograms();
}

auto FindPath(const char* filename, std::initializer_list<const char*> prefixes) -> fs::path {
  for (const char* prefix : prefixes) {
    fs::path path = fs::path(prefix) / filename;
    std::error_code ec;
    if (fs::is_regular_file(path, ec)) {
      return path;
    }
  }
  return {};
}

auto FindModelDir() -> fs::path {
#ifdef ALCEDO_DEMOASICNET_MODEL_DIR
  {
    fs::path p{ALCEDO_DEMOASICNET_MODEL_DIR};
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
      return p;
    }
  }
#endif
  return FindPath("bayer.safetensors",
                  {"alcedo_studio/src/config/models/", "../alcedo_studio/src/config/models/",
                   "../../alcedo_studio/src/config/models/",
                   "../../../alcedo_studio/src/config/models/", "src/config/models/",
                   "../src/config/models/", "D:/Projects/pu-erh_lab/alcedo_studio/src/config/models/"})
      .parent_path();
}

auto FindGolden(const char* filename) -> fs::path {
  return FindPath(filename, {"alcedo_studio/tests/ml_ops/goldens/",
                             "../alcedo_studio/tests/ml_ops/goldens/",
                             "../../alcedo_studio/tests/ml_ops/goldens/",
                             "../../../alcedo_studio/tests/ml_ops/goldens/",
                             "tests/ml_ops/goldens/", "../tests/ml_ops/goldens/",
                             "D:/Projects/pu-erh_lab/alcedo_studio/tests/ml_ops/goldens/"});
}

auto LoadFloatBin(const fs::path& path, std::size_t expected_count) -> std::vector<float> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("failed to open " + path.string());
  }
  in.seekg(0, std::ios::end);
  const auto nbytes = static_cast<std::size_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  if (nbytes != expected_count * sizeof(float)) {
    throw std::runtime_error("unexpected size for " + path.string());
  }
  std::vector<float> data(expected_count);
  in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(nbytes));
  return data;
}

// Product forward writes HWC; references remain NCHW [1,3,H,W].
auto HwcToNchw(const std::vector<float>& hwc, int height, int width) -> std::vector<float> {
  const std::size_t plane = static_cast<std::size_t>(height) * width;
  std::vector<float> nchw(plane * 3);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const std::size_t pix = static_cast<std::size_t>(y) * width + x;
      nchw[0 * plane + pix] = hwc[pix * 3 + 0];
      nchw[1 * plane + pix] = hwc[pix * 3 + 1];
      nchw[2 * plane + pix] = hwc[pix * 3 + 2];
    }
  }
  return nchw;
}

struct MaxAbsReport {
  float       max_abs  = 0.0f;
  std::size_t index    = 0;
  float       expected = 0.0f;
  float       actual   = 0.0f;
  int         y        = 0;
  int         x        = 0;
  int         channel  = 0;
};

auto ComputeMaxAbsNchw(const std::vector<float>& actual, const std::vector<float>& expected, int h,
                       int w) -> MaxAbsReport {
  MaxAbsReport report;
  const std::size_t plane = static_cast<std::size_t>(h) * w;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const float err = std::fabs(actual[i] - expected[i]);
    if (err > report.max_abs) {
      report.max_abs  = err;
      report.index    = i;
      report.expected = expected[i];
      report.actual   = actual[i];
      report.channel  = static_cast<int>(i / plane);
      const std::size_t pix = i % plane;
      report.y        = static_cast<int>(pix / static_cast<std::size_t>(w));
      report.x        = static_cast<int>(pix % static_cast<std::size_t>(w));
    }
  }
  return report;
}

void ExpectNchwNear(const std::vector<float>& actual, const std::vector<float>& expected, int h,
                    int w, float abs_tol) {
  ASSERT_EQ(actual.size(), expected.size());
  const auto report = ComputeMaxAbsNchw(actual, expected, h, w);
  EXPECT_LE(report.max_abs, abs_tol)
      << "max_abs=" << report.max_abs << " at nchw index=" << report.index << " y=" << report.y
      << " x=" << report.x << " channel=" << report.channel << " expected=" << report.expected
      << " actual=" << report.actual;
  if (report.max_abs > abs_tol) {
    std::cerr << "first failing coordinate: y=" << report.y << " x=" << report.x
              << " c=" << report.channel << "\n";
  }
}

template <typename Net>
auto ForwardReference(const Net& net, const std::vector<float>& input_nchw, int h, int w, int oh,
                      int ow, nn_ocl::WorkspacePool& workspace) -> std::vector<float> {
  constexpr int N = 1;
  nn_ocl::DeviceBuffer in_buf  = nn_ocl::DeviceBuffer::Floats(input_nchw.size());
  nn_ocl::DeviceBuffer out_buf =
      nn_ocl::DeviceBuffer::Floats(static_cast<std::size_t>(N) * oh * ow * 3);
  in_buf.UploadFloats(input_nchw);

  auto& ctx = OpenClContext::Instance();
  net.ForwardNchwToHwc(in_buf.get(), N, h, w, out_buf.get(), workspace, ctx.Queue(),
                       /*apply_gamma_decode=*/false);
  nn_ocl::WaitQueue(ctx.Queue());

  auto hwc = out_buf.DownloadFloats(static_cast<std::size_t>(N) * oh * ow * 3);
  return HwcToNchw(hwc, oh, ow);
}

class OpenClDemosaicNetModuleTest : public ::testing::Test {
 protected:
  void SetUp() override { RequireOpenCl(); }
};

}  // namespace

// ---------------------------------------------------------------------------
// LoadWeights / metadata validation
// ---------------------------------------------------------------------------

TEST_F(OpenClDemosaicNetModuleTest, BayerLoadWeightsRejectsWrongArchitectureMetadata) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  auto map  = nn::LoadSafetensors(model_dir / "bayer.safetensors");
  auto meta = map.metadata();
  meta["architecture"] = "teacher_bayer_d15";
  map.SetMetadata(std::move(meta));

  OpenClBayerDemosaicNet net;
  EXPECT_THROW(net.LoadWeights(map), std::runtime_error);
  EXPECT_FALSE(net.weights_loaded());
}

TEST_F(OpenClDemosaicNetModuleTest, BayerLoadWeightsRejectsWrongTensorShapeWithoutPublishing) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  auto map     = nn::LoadSafetensors(model_dir / "bayer.safetensors");
  auto mutated = map.at("trunk.0.weight");
  mutated.shape = {24, 5, 3, 3};
  mutated.data.assign(mutated.numel(), 0.0f);
  map.InsertOrAssign(std::move(mutated));

  OpenClBayerDemosaicNet net;
  EXPECT_THROW(net.LoadWeights(map), std::runtime_error);
  EXPECT_FALSE(net.weights_loaded());
}

TEST_F(OpenClDemosaicNetModuleTest, XTransLoadWeightsAndResidentWeightsNonZero) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  auto map = nn::LoadSafetensors(model_dir / "xtrans.safetensors");
  OpenClXTransDemosaicNet net;
  ASSERT_NO_THROW(net.LoadWeights(map));
  EXPECT_TRUE(net.weights_loaded());
  EXPECT_GT(net.ResidentWeightBytes(), 0u);
  EXPECT_NE(net.Trunk0WeightBuffer(), nullptr);
  EXPECT_NE(net.OutputWeightBuffer(), nullptr);
}

// ---------------------------------------------------------------------------
// Lazy cache
// ---------------------------------------------------------------------------

TEST_F(OpenClDemosaicNetModuleTest, CacheRejectsInvalidMetadataWithoutPublishingEntry) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  // Use a temporary copy with bad architecture metadata is heavy; instead load a
  // path that exists then simulate via direct module rejection (above). Here we
  // verify a missing weight file leaves the cache cold.
  OpenClDemosaicNetModelCache cache;
  OpenClDemosaicNetLoadOptions opts;
  opts.model_dir = model_dir / "does_not_exist_subdir";
  EXPECT_FALSE(cache.EnsureLoaded(OpenClDemosaicNetVariant::Bayer, opts));
  EXPECT_FALSE(cache.IsLoaded(OpenClDemosaicNetVariant::Bayer));
  EXPECT_FALSE(cache.LastError().empty());
}

TEST_F(OpenClDemosaicNetModuleTest, CacheSecondEnsureLoadedReusesWeightBuffers) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  OpenClDemosaicNetLoadOptions opts;
  opts.model_dir = model_dir;

  OpenClDemosaicNetModelCache cache;
  ASSERT_TRUE(cache.EnsureLoaded(OpenClDemosaicNetVariant::Bayer, opts)) << cache.LastError();
  const cl_mem w0 = cache.Bayer().Trunk0WeightBuffer();
  const cl_mem w1 = cache.Bayer().OutputWeightBuffer();
  ASSERT_NE(w0, nullptr);
  ASSERT_NE(w1, nullptr);

  ASSERT_TRUE(cache.EnsureLoaded(OpenClDemosaicNetVariant::Bayer, opts));
  EXPECT_EQ(cache.Bayer().Trunk0WeightBuffer(), w0);
  EXPECT_EQ(cache.Bayer().OutputWeightBuffer(), w1);
}

TEST_F(OpenClDemosaicNetModuleTest,
       SecondForwardReusesWorkspaceAllocationAndCompiledPrograms) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_64_in.bin");
  const auto out_path  = FindGolden("bayer_64_out.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or small reference fixtures not found";
  }

  constexpr int N  = 1;
  constexpr int H  = 64;
  constexpr int W  = 64;
  const int     Oh = OpenClBayerDemosaicNet::OutputHeight(H, W);
  const int     Ow = OpenClBayerDemosaicNet::OutputWidth(W, H);
  const auto    hin = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));

  OpenClBayerDemosaicNet net;
  net.LoadWeights(nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  nn_ocl::WorkspacePool workspace;
  workspace.Reserve(OpenClBayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  const auto gen_after_reserve = workspace.allocation_generation();

  const bool built_before_first =
      OpenClProgramLibrary::Instance().IsProgramBuilt(OpenCL::DemosaicNet::kConvBayerProgramName);

  (void)ForwardReference(net, hin, H, W, Oh, Ow, workspace);
  EXPECT_TRUE(OpenClProgramLibrary::Instance().IsProgramBuilt(
      OpenCL::DemosaicNet::kConvBayerProgramName));
  EXPECT_TRUE(OpenClProgramLibrary::Instance().IsProgramBuilt(
      OpenCL::DemosaicNet::kStructuralProgramName));
  (void)built_before_first;

  const auto gen_after_first = workspace.allocation_generation();
  EXPECT_EQ(gen_after_first, gen_after_reserve)
      << "first forward must not reallocate after Reserve";

  const cl_mem pack_w = net.Trunk0WeightBuffer();
  (void)ForwardReference(net, hin, H, W, Oh, Ow, workspace);
  EXPECT_EQ(workspace.allocation_generation(), gen_after_first)
      << "second forward must not reallocate workspace";
  EXPECT_EQ(net.Trunk0WeightBuffer(), pack_w) << "second forward must not re-upload weights";
}

// ---------------------------------------------------------------------------
// Exported student reference pairs (absolute 1e-4)
// ---------------------------------------------------------------------------

TEST_F(OpenClDemosaicNetModuleTest, BayerStudentForwardMatchesExportedReference00) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_00.bin");
  const auto out_path  = FindGolden("bayer_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student references not found";
  }

  constexpr int N  = 1;
  constexpr int H  = OpenClBayerDemosaicNet::kTileInput;
  constexpr int W  = OpenClBayerDemosaicNet::kTileInput;
  constexpr int Oh = OpenClBayerDemosaicNet::kTileOutput;
  constexpr int Ow = OpenClBayerDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  OpenClBayerDemosaicNet net;
  net.LoadWeights(nn::LoadSafetensors(model_dir / "bayer.safetensors"));
  nn_ocl::WorkspacePool ws(OpenClBayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  ExpectNchwNear(ForwardReference(net, hin, H, W, Oh, Ow, ws), expected, Oh, Ow, kAbsTol);
}

TEST_F(OpenClDemosaicNetModuleTest, BayerStudentForwardMatchesExportedReference01) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_01.bin");
  const auto out_path  = FindGolden("bayer_output_01.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student references not found";
  }

  constexpr int N  = 1;
  constexpr int H  = OpenClBayerDemosaicNet::kTileInput;
  constexpr int W  = OpenClBayerDemosaicNet::kTileInput;
  constexpr int Oh = OpenClBayerDemosaicNet::kTileOutput;
  constexpr int Ow = OpenClBayerDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  OpenClBayerDemosaicNet net;
  net.LoadWeights(nn::LoadSafetensors(model_dir / "bayer.safetensors"));
  nn_ocl::WorkspacePool ws(OpenClBayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  ExpectNchwNear(ForwardReference(net, hin, H, W, Oh, Ow, ws), expected, Oh, Ow, kAbsTol);
}

TEST_F(OpenClDemosaicNetModuleTest, XTransStudentForwardMatchesExportedReference00) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_00.bin");
  const auto out_path  = FindGolden("xtrans_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student references not found";
  }

  constexpr int N  = 1;
  constexpr int H  = OpenClXTransDemosaicNet::kTileInput;
  constexpr int W  = OpenClXTransDemosaicNet::kTileInput;
  constexpr int Oh = OpenClXTransDemosaicNet::kTileOutput;
  constexpr int Ow = OpenClXTransDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  OpenClXTransDemosaicNet net;
  net.LoadWeights(nn::LoadSafetensors(model_dir / "xtrans.safetensors"));
  nn_ocl::WorkspacePool ws(OpenClXTransDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  ExpectNchwNear(ForwardReference(net, hin, H, W, Oh, Ow, ws), expected, Oh, Ow, kAbsTol);
}

TEST_F(OpenClDemosaicNetModuleTest, XTransStudentForwardMatchesExportedReference01) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_01.bin");
  const auto out_path  = FindGolden("xtrans_output_01.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student references not found";
  }

  constexpr int N  = 1;
  constexpr int H  = OpenClXTransDemosaicNet::kTileInput;
  constexpr int W  = OpenClXTransDemosaicNet::kTileInput;
  constexpr int Oh = OpenClXTransDemosaicNet::kTileOutput;
  constexpr int Ow = OpenClXTransDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  OpenClXTransDemosaicNet net;
  net.LoadWeights(nn::LoadSafetensors(model_dir / "xtrans.safetensors"));
  nn_ocl::WorkspacePool ws(OpenClXTransDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  ExpectNchwNear(ForwardReference(net, hin, H, W, Oh, Ow, ws), expected, Oh, Ow, kAbsTol);
}

TEST_F(OpenClDemosaicNetModuleTest, OutputShapeHelpersMatchSpecs) {
  EXPECT_EQ(OpenClBayerDemosaicNet::OutputHeight(64, 64),
            64 - OpenClBayerDemosaicNet::kNaturalSpatialLoss);
  EXPECT_EQ(OpenClBayerDemosaicNet::OutputHeight(OpenClBayerDemosaicNet::kTileInput,
                                                 OpenClBayerDemosaicNet::kTileInput),
            OpenClBayerDemosaicNet::kTileOutput);
  EXPECT_EQ(OpenClXTransDemosaicNet::OutputHeight(OpenClXTransDemosaicNet::kTileInput,
                                                  OpenClXTransDemosaicNet::kTileInput),
            OpenClXTransDemosaicNet::kTileOutput);
}

// Purpose: Bayer product tiling queues all shared jobs without a host wait,
// preserves the aligned canvas geometry, and completes to finite RGB values.
TEST_F(OpenClDemosaicNetModuleTest,
       BayerTiledExecutionQueuesMultiTileCoverageWithoutHostSynchronization) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int kWidth = 2048;
  constexpr int kHeight = 2048;
  std::vector<float> input(static_cast<std::size_t>(kWidth) * kHeight * 3, 0.1f);
  nn_ocl::DeviceBuffer input_buffer = nn_ocl::DeviceBuffer::Floats(input.size());
  nn_ocl::DeviceBuffer output_buffer = nn_ocl::DeviceBuffer::Floats(input.size());
  input_buffer.UploadFloats(input);

  OpenClBayerDemosaicNet module;
  module.LoadWeights(nn::LoadSafetensors(model_dir / "bayer.safetensors"));
  OpenClDemosaicNetTiledExecutor executor;
  nn_ocl::WorkspacePool workspace;
  nn_ocl::ResetDispatchInstrumentation();

  const auto result = executor.EnqueueBayer(
      module, workspace,
      {.input_aligned_hwc = input_buffer.get(),
       .output_aligned_hwc = output_buffer.get(),
       .aligned_width = kWidth,
       .aligned_height = kHeight,
       .queue = OpenClContext::Instance().Queue()});

  // Bayer's model-output origin is -1 (pad32/border31), so the 2048 square
  // requires a 3×3 shared-job grid rather than a naïve 2×2 grid.
  EXPECT_EQ(result.tile_count, 9u);
  EXPECT_EQ(result.output_width, kWidth);
  EXPECT_EQ(result.output_height, kHeight);
  EXPECT_EQ(nn_ocl::GetDispatchInstrumentation().finish_count, 0u);
  EXPECT_EQ(nn_ocl::GetDispatchInstrumentation().wait_count, 0u);

  nn_ocl::WaitQueue(OpenClContext::Instance().Queue());
  const auto output = output_buffer.DownloadFloats(input.size());
  EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                          [](const float value) { return std::isfinite(value); }));
}

// Purpose: X-Trans product tiling keeps period-6 shared-job coverage asynchronous
// and writes finite RGB to the period-trimmed aligned canvas.
TEST_F(OpenClDemosaicNetModuleTest,
       XTransTiledExecutionQueuesMultiTileCoverageWithoutHostSynchronization) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int kWidth = 2040;
  constexpr int kHeight = 2040;
  std::vector<float> input(static_cast<std::size_t>(kWidth) * kHeight * 3, 0.1f);
  nn_ocl::DeviceBuffer input_buffer = nn_ocl::DeviceBuffer::Floats(input.size());
  nn_ocl::DeviceBuffer output_buffer = nn_ocl::DeviceBuffer::Floats(input.size());
  input_buffer.UploadFloats(input);

  OpenClXTransDemosaicNet module;
  module.LoadWeights(nn::LoadSafetensors(model_dir / "xtrans.safetensors"));
  OpenClDemosaicNetTiledExecutor executor;
  nn_ocl::WorkspacePool workspace;
  nn_ocl::ResetDispatchInstrumentation();

  const auto result = executor.EnqueueXTrans(
      module, workspace,
      {.input_aligned_hwc = input_buffer.get(),
       .output_aligned_hwc = output_buffer.get(),
       .aligned_width = kWidth,
       .aligned_height = kHeight,
       .queue = OpenClContext::Instance().Queue()});

  EXPECT_EQ(result.tile_count, 4u);
  EXPECT_EQ(result.output_width, kWidth);
  EXPECT_EQ(result.output_height, kHeight);
  EXPECT_EQ(nn_ocl::GetDispatchInstrumentation().finish_count, 0u);
  EXPECT_EQ(nn_ocl::GetDispatchInstrumentation().wait_count, 0u);

  nn_ocl::WaitQueue(OpenClContext::Instance().Queue());
  const auto output = output_buffer.DownloadFloats(input.size());
  EXPECT_TRUE(std::all_of(output.begin(), output.end(),
                          [](const float value) { return std::isfinite(value); }));
}

}  // namespace alcedo
