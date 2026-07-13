//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <cuda_runtime.h>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>
#include <opencv2/core/cuda_stream_accessor.hpp>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/safetensors.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"
#include "decoders/processor/nn/demosaicnet_activation_slots.hpp"
#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/nn/demosaicnet_preprocess.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"
#include "decoders/processor/operators/gpu/cuda_demosaicnet.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"

namespace alcedo {
namespace {

namespace fs = std::filesystem;

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
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

void ExpectVectorsNear(const std::vector<float>& actual, const std::vector<float>& expected,
                       float abs_tol) {
  ASSERT_EQ(actual.size(), expected.size());
  float max_abs = 0.0f;
  for (std::size_t i = 0; i < actual.size(); ++i) {
    const float err = std::fabs(actual[i] - expected[i]);
    max_abs         = std::max(max_abs, err);
    EXPECT_NEAR(actual[i], expected[i], abs_tol) << "mismatch at index " << i;
  }
  if (::testing::Test::HasFailure()) {
    std::cerr << "max abs error = " << max_abs << "\n";
  }
}

auto MakePattern(std::size_t n, std::uint32_t seed) -> std::vector<float> {
  std::mt19937                          rng(seed);
  std::uniform_real_distribution<float> dist(0.0f, 1.0f);
  std::vector<float>                    v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = dist(rng);
  }
  return v;
}

class MlOpsDemosaicNetTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device";
    }
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// LoadWeights / CRTP
// ---------------------------------------------------------------------------

TEST_F(MlOpsDemosaicNetTest, BayerLoadWeightsAndRejectWrongShape) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  auto map = cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors");
  BayerDemosaicNet net;
  EXPECT_FALSE(net.weights_loaded());
  ASSERT_NO_THROW(net.LoadWeights(map));
  EXPECT_TRUE(net.weights_loaded());
  EXPECT_GT(net.ResidentWeightBytes(), 0u);
  EXPECT_NE(net.PackWeightDevicePtr(), nullptr);

  // Mutate a weight shape and expect RequireF32Tensor / LoadWeights to fail.
  auto bad = map;
  auto mutated = bad.at("trunk.0.weight");
  mutated.shape = {24, 5, 3, 3};  // wrong Cin
  mutated.data.assign(mutated.numel(), 0.0f);
  bad.Insert(std::move(mutated));

  BayerDemosaicNet bad_net;
  EXPECT_THROW(bad_net.LoadWeights(bad), std::runtime_error);
  EXPECT_FALSE(bad_net.weights_loaded());
}

TEST_F(MlOpsDemosaicNetTest, BayerLoadWeightsRejectsWrongArchitectureMetadata) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  auto map = cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors");
  auto meta = map.metadata();
  meta["architecture"] = "teacher_bayer_d15";
  map.SetMetadata(std::move(meta));
  BayerDemosaicNet net;
  EXPECT_THROW(net.LoadWeights(map), std::runtime_error);
}

TEST_F(MlOpsDemosaicNetTest, XTransLoadWeightsAndRejectWrongShape) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  auto map = cuda::nn::LoadSafetensors(model_dir / "xtrans.safetensors");
  XTransDemosaicNet net;
  ASSERT_NO_THROW(net.LoadWeights(map));
  EXPECT_TRUE(net.weights_loaded());
  EXPECT_GT(net.ResidentWeightBytes(), 0u);

  auto bad = map;
  auto mutated = bad.at("post_conv.weight");
  mutated.shape = {32, 5, 3, 3};  // wrong Cin (must be 6)
  mutated.data.assign(mutated.numel(), 0.0f);
  bad.Insert(std::move(mutated));

  XTransDemosaicNet bad_net;
  EXPECT_THROW(bad_net.LoadWeights(bad), std::runtime_error);
}

// ---------------------------------------------------------------------------
// Lazy cache
// ---------------------------------------------------------------------------

TEST_F(MlOpsDemosaicNetTest, CacheColdUntilEnsureLoadedIndependentVariants) {
  DemosaicNetModelCache cache;
  EXPECT_FALSE(cache.IsLoaded(DemosaicNetVariant::Bayer));
  EXPECT_FALSE(cache.IsLoaded(DemosaicNetVariant::XTrans));
  EXPECT_EQ(cache.ResidentWeightBytes(), 0u);

  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  DemosaicNetLoadOptions opts;
  opts.model_dir = model_dir;

  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, opts)) << cache.LastError();
  EXPECT_TRUE(cache.IsLoaded(DemosaicNetVariant::Bayer));
  EXPECT_FALSE(cache.IsLoaded(DemosaicNetVariant::XTrans));

  const auto bayer_bytes = cache.ResidentWeightBytes();
  EXPECT_GT(bayer_bytes, 0u);

  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::XTrans, opts)) << cache.LastError();
  EXPECT_TRUE(cache.IsLoaded(DemosaicNetVariant::XTrans));
  EXPECT_GT(cache.ResidentWeightBytes(), bayer_bytes);

  // Second EnsureLoaded is a no-op (same instance / pointers).
  const float* pack_ptr = cache.Bayer().PackWeightDevicePtr();
  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, opts));
  EXPECT_EQ(cache.Bayer().PackWeightDevicePtr(), pack_ptr);
}

TEST_F(MlOpsDemosaicNetTest, CacheNoReloadSameWeightPointers) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  DemosaicNetLoadOptions opts;
  opts.model_dir = model_dir;

  DemosaicNetModelCache cache;
  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, opts)) << cache.LastError();

  const auto& m1 = cache.Bayer();
  const float* w0 = m1.PackWeightDevicePtr();
  const float* w1 = m1.OutputWeightDevicePtr();

  // Two sequential forwards must not reload weights (natural-size student tile).
  constexpr int H = 64, W = 64, N = 1;
  const int     Oh = BayerDemosaicNet::OutputHeight(H, W);
  const int     Ow = BayerDemosaicNet::OutputWidth(W, H);
  const auto    hin = MakePattern(static_cast<std::size_t>(N * 3 * H * W), 7);
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(static_cast<std::size_t>(N) * 3 * Oh * Ow);

  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, Oh, Ow});

  cuda::nn::WorkspacePool ws;
  ws.Reserve(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  m1.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, opts));
  const auto& m2 = cache.Bayer();
  EXPECT_EQ(m2.PackWeightDevicePtr(), w0);
  EXPECT_EQ(m2.OutputWeightDevicePtr(), w1);

  m2.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  EXPECT_EQ(cache.Bayer().PackWeightDevicePtr(), w0);
}

// ---------------------------------------------------------------------------
// Golden forward (exported student tile fixtures: 1086→1024 / 1048→1024)
// ---------------------------------------------------------------------------

TEST_F(MlOpsDemosaicNetTest, BayerStudentForwardMatchesExportedGolden00) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_00.bin");
  const auto out_path  = FindGolden("bayer_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N = 1;
  constexpr int H = BayerDemosaicNet::kTileInput;
  constexpr int W = BayerDemosaicNet::kTileInput;
  constexpr int Oh = BayerDemosaicNet::kTileOutput;
  constexpr int Ow = BayerDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(expected.size());
  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, Oh, Ow});

  cuda::nn::WorkspacePool ws;
  ws.Reserve(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  net.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ExpectVectorsNear(d_out.Download(), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, BayerStudentForwardMatchesExportedGolden01) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_01.bin");
  const auto out_path  = FindGolden("bayer_output_01.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N = 1;
  constexpr int H = BayerDemosaicNet::kTileInput;
  constexpr int W = BayerDemosaicNet::kTileInput;
  constexpr int Oh = BayerDemosaicNet::kTileOutput;
  constexpr int Ow = BayerDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(expected.size());
  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, Oh, Ow});
  cuda::nn::WorkspacePool ws(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  net.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  ExpectVectorsNear(d_out.Download(), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, XTransStudentForwardMatchesExportedGolden00) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_00.bin");
  const auto out_path  = FindGolden("xtrans_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N = 1;
  constexpr int H = XTransDemosaicNet::kTileInput;
  constexpr int W = XTransDemosaicNet::kTileInput;
  constexpr int Oh = XTransDemosaicNet::kTileOutput;
  constexpr int Ow = XTransDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  XTransDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "xtrans.safetensors"));

  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(expected.size());
  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, Oh, Ow});

  cuda::nn::WorkspacePool ws;
  ws.Reserve(XTransDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  net.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ExpectVectorsNear(d_out.Download(), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, XTransStudentForwardMatchesExportedGolden01) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_01.bin");
  const auto out_path  = FindGolden("xtrans_output_01.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N = 1;
  constexpr int H = XTransDemosaicNet::kTileInput;
  constexpr int W = XTransDemosaicNet::kTileInput;
  constexpr int Oh = XTransDemosaicNet::kTileOutput;
  constexpr int Ow = XTransDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  XTransDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "xtrans.safetensors"));
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(expected.size());
  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, Oh, Ow});
  cuda::nn::WorkspacePool ws(XTransDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  net.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  ExpectVectorsNear(d_out.Download(), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, BayerStudentGoldenCornerPixelsFinite) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_00.bin");
  const auto out_path  = FindGolden("bayer_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N = 1;
  constexpr int H = BayerDemosaicNet::kTileInput;
  constexpr int W = BayerDemosaicNet::kTileInput;
  constexpr int Oh = BayerDemosaicNet::kTileOutput;
  constexpr int Ow = BayerDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));
  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(expected.size());
  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, Oh, Ow});
  cuda::nn::WorkspacePool ws(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  net.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  const auto out = d_out.Download();

  // Corners of each RGB plane (NCHW): plane c starts at c * Oh * Ow.
  const auto plane = Oh * Ow;
  EXPECT_NEAR(out[0], expected[0], 1e-4f);
  EXPECT_NEAR(out[plane - 1], expected[plane - 1], 1e-4f);
  EXPECT_NEAR(out[plane], expected[plane], 1e-4f);
  EXPECT_NEAR(out[2 * plane - 1], expected[2 * plane - 1], 1e-4f);
  EXPECT_NEAR(out[2 * plane], expected[2 * plane], 1e-4f);
  EXPECT_NEAR(out[3 * plane - 1], expected[3 * plane - 1], 1e-4f);
  for (float v : out) {
    EXPECT_TRUE(std::isfinite(v));
  }
}

// ---------------------------------------------------------------------------
// Concurrency smoke: two threads, two workspaces, shared module
// ---------------------------------------------------------------------------

TEST_F(MlOpsDemosaicNetTest, ConcurrentForwardsSharedWeights) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  DemosaicNetModelCache cache;
  DemosaicNetLoadOptions opts;
  opts.model_dir = model_dir;
  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, opts)) << cache.LastError();
  const BayerDemosaicNet& net    = cache.Bayer();
  const float*            pack_w = net.PackWeightDevicePtr();

  constexpr int H = 64, W = 64, N = 1;
  const int     Oh = BayerDemosaicNet::OutputHeight(H, W);
  const int     Ow = BayerDemosaicNet::OutputWidth(W, H);
  const auto    hin = MakePattern(static_cast<std::size_t>(N * 3 * H * W), 99);
  std::atomic<int> ok{0};
  std::atomic<int> fail{0};

  auto worker = [&](int seed_offset) {
    try {
      cuda::nn::DeviceBufferF32 d_in(hin.size());
      d_in.Upload(hin);
      cuda::nn::DeviceBufferF32 d_out(static_cast<std::size_t>(N) * 3 * Oh * Ow);
      auto tin  = d_in.AsTensor({N, 3, H, W});
      auto tout = d_out.AsTensor({N, 3, Oh, Ow});

      cuda::nn::WorkspacePool ws;
      ws.Reserve(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
      net.Forward(tin, tout, ws);
      if (::cudaDeviceSynchronize() != cudaSuccess) {
        fail.fetch_add(1);
        return;
      }
      // Weight pointers must stay stable across concurrent readers.
      if (net.PackWeightDevicePtr() != pack_w) {
        fail.fetch_add(1);
        return;
      }
      (void)seed_offset;
      ok.fetch_add(1);
    } catch (...) {
      fail.fetch_add(1);
    }
  };

  std::thread t0(worker, 0);
  std::thread t1(worker, 1);
  t0.join();
  t1.join();
  EXPECT_EQ(fail.load(), 0);
  EXPECT_EQ(ok.load(), 2);
  EXPECT_EQ(cache.Bayer().PackWeightDevicePtr(), pack_w);
}

TEST_F(MlOpsDemosaicNetTest, OutputShapeHelpers) {
  // Natural size for non-tile inputs.
  EXPECT_EQ(BayerDemosaicNet::OutputHeight(64, 64), 64 - BayerDemosaicNet::kNaturalSpatialLoss);
  EXPECT_EQ(BayerDemosaicNet::OutputWidth(128, 64), 128 - BayerDemosaicNet::kNaturalSpatialLoss);
  EXPECT_EQ(XTransDemosaicNet::OutputHeight(48, 48), 48 - XTransDemosaicNet::kNaturalSpatialLoss);
  EXPECT_EQ(XTransDemosaicNet::OutputWidth(26, 48), 26 - XTransDemosaicNet::kNaturalSpatialLoss);
  // Export tile contract.
  EXPECT_EQ(BayerDemosaicNet::OutputHeight(BayerDemosaicNet::kTileInput,
                                           BayerDemosaicNet::kTileInput),
            BayerDemosaicNet::kTileOutput);
  EXPECT_EQ(XTransDemosaicNet::OutputHeight(XTransDemosaicNet::kTileInput,
                                            XTransDemosaicNet::kTileInput),
            XTransDemosaicNet::kTileOutput);
  EXPECT_EQ(BayerDemosaicNet::kTilePad, 32);
  EXPECT_EQ(XTransDemosaicNet::kTileStep, 1020);
  EXPECT_GT(BayerDemosaicNet::EstimateWorkspaceBytes(64, 64, 1), 0u);
  EXPECT_GT(XTransDemosaicNet::EstimateWorkspaceBytes(48, 48, 1), 0u);
}

// ---------------------------------------------------------------------------
// P1 — activation lifetime reuse (peak-live slots, golden parity, gen stability)
// ---------------------------------------------------------------------------

namespace {

// Pre-P1 sum-of-all-activations estimate (for regression: peak-live must be smaller).
[[nodiscard]] auto LegacySumOfAllActivationBytes(int input_h, int input_w, int batch, int pack_out_ch,
                                                 int width, int residual_ch, int depth,
                                                 int pack_factor) -> std::size_t {
  using demosaicnet_slots::TensorBytes;
  if (batch < 1 || (input_h % pack_factor) != 0 || (input_w % pack_factor) != 0) {
    return 0;
  }
  const std::int64_t N  = batch;
  const std::int64_t ph = input_h / pack_factor;
  const std::int64_t pw = input_w / pack_factor;
  std::size_t        total = TensorBytes(N, pack_out_ch, ph, pw);
  std::int64_t       ch    = ph;
  std::int64_t       cw    = pw;
  for (int i = 0; i < depth; ++i) {
    const std::int64_t oh = ch - 2;
    const std::int64_t ow = cw - 2;
    total += TensorBytes(N, width, oh, ow);
    ch = oh;
    cw = ow;
  }
  total += TensorBytes(N, residual_ch, ch, cw);
  const std::int64_t uh = ch * pack_factor;
  const std::int64_t uw = cw * pack_factor;
  total += TensorBytes(N, 3, uh, uw);
  total += TensorBytes(N, 3, uh, uw);
  total += TensorBytes(N, 6, uh, uw);
  const std::int64_t nh = uh - 2;
  const std::int64_t nw = uw - 2;
  total += TensorBytes(N, width, nh, nw);
  total += TensorBytes(N, 3, nh, nw);
  return total + demosaicnet_slots::kScratchHeadroomBytes;
}

}  // namespace

TEST_F(MlOpsDemosaicNetTest, BayerStudentForwardReusesTwoTrunkSlotsAndMatchesExportedGolden) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_00.bin");
  const auto out_path  = FindGolden("bayer_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N  = 1;
  constexpr int H  = BayerDemosaicNet::kTileInput;
  constexpr int W  = BayerDemosaicNet::kTileInput;
  constexpr int Oh = BayerDemosaicNet::kTileOutput;
  constexpr int Ow = BayerDemosaicNet::kTileOutput;

  const auto peak = demosaicnet_slots::ComputePeakLiveSlots(
      H, W, N, BayerDemosaicNet::kPackOutCh, BayerDemosaicNet::kWidth,
      BayerDemosaicNet::kResidualCh, BayerDemosaicNet::kDepth, BayerDemosaicNet::kPackFactor);
  const auto legacy = LegacySumOfAllActivationBytes(
      H, W, N, BayerDemosaicNet::kPackOutCh, BayerDemosaicNet::kWidth,
      BayerDemosaicNet::kResidualCh, BayerDemosaicNet::kDepth, BayerDemosaicNet::kPackFactor);
  ASSERT_GT(peak.trunk_slot_bytes, 0u);
  ASSERT_GT(peak.estimate_bytes, 0u);
  // Two trunk slots + structural + post must beat the old sum-of-all path.
  EXPECT_LT(peak.estimate_bytes, legacy);
  EXPECT_LE(peak.estimate_bytes, (legacy * 3) / 4);
  EXPECT_EQ(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N), peak.estimate_bytes);

  const auto hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));
  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(expected.size());
  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, Oh, Ow});

  // Exact peak-live reserve: proves Forward does not need the legacy sum-of-all slab.
  cuda::nn::WorkspacePool ws;
  ws.Reserve(peak.estimate_bytes);
  net.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  EXPECT_EQ(ws.used_bytes(), peak.peak_live_bytes);
  EXPECT_EQ(ws.capacity_bytes(), peak.estimate_bytes);
  ExpectVectorsNear(d_out.Download(), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, XTransStudentForwardReusesTwoTrunkSlotsAndMatchesExportedGolden) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_00.bin");
  const auto out_path  = FindGolden("xtrans_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N  = 1;
  constexpr int H  = XTransDemosaicNet::kTileInput;
  constexpr int W  = XTransDemosaicNet::kTileInput;
  constexpr int Oh = XTransDemosaicNet::kTileOutput;
  constexpr int Ow = XTransDemosaicNet::kTileOutput;

  const auto peak = demosaicnet_slots::ComputePeakLiveSlots(
      H, W, N, XTransDemosaicNet::kPackOutCh, XTransDemosaicNet::kWidth,
      XTransDemosaicNet::kResidualCh, XTransDemosaicNet::kDepth, XTransDemosaicNet::kPackFactor);
  const auto legacy = LegacySumOfAllActivationBytes(
      H, W, N, XTransDemosaicNet::kPackOutCh, XTransDemosaicNet::kWidth,
      XTransDemosaicNet::kResidualCh, XTransDemosaicNet::kDepth, XTransDemosaicNet::kPackFactor);
  ASSERT_GT(peak.trunk_slot_bytes, 0u);
  EXPECT_LT(peak.estimate_bytes, legacy);
  EXPECT_LE(peak.estimate_bytes, (legacy * 3) / 4);
  EXPECT_EQ(XTransDemosaicNet::EstimateWorkspaceBytes(H, W, N), peak.estimate_bytes);

  const auto hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));
  XTransDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "xtrans.safetensors"));
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(expected.size());
  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, Oh, Ow});
  cuda::nn::WorkspacePool ws;
  ws.Reserve(peak.estimate_bytes);
  net.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  EXPECT_EQ(ws.used_bytes(), peak.peak_live_bytes);
  ExpectVectorsNear(d_out.Download(), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, StudentForwardPeakWorkspaceEstimateCoversEveryLiveSlot) {
  auto check = [](int H, int W, int pack_out, int width, int residual, int depth, int pack,
                  auto estimate_fn) {
    using demosaicnet_slots::TensorBytes;
    // P4-A product default: fused post drops the width-channel post slot.
    const auto peak_fused = demosaicnet_slots::ComputePeakLiveSlots(
        H, W, 1, pack_out, width, residual, depth, pack, /*fuse_post_output=*/true);
    ASSERT_GT(peak_fused.trunk_slot_bytes, 0u);
    ASSERT_GT(peak_fused.structural_slot_bytes, 0u);
    EXPECT_EQ(peak_fused.post_slot_bytes, 0u);
    EXPECT_TRUE(peak_fused.fuse_post_output);
    const std::size_t live_fused =
        2 * peak_fused.trunk_slot_bytes + peak_fused.structural_slot_bytes;
    EXPECT_EQ(peak_fused.peak_live_bytes, live_fused);
    EXPECT_EQ(peak_fused.estimate_bytes, live_fused + demosaicnet_slots::kScratchHeadroomBytes);
    EXPECT_EQ(estimate_fn(H, W, 1, true), peak_fused.estimate_bytes);

    // Ordinary unfused path still reserves post + natural RGB structural capacity.
    const auto peak_ord = demosaicnet_slots::ComputePeakLiveSlots(
        H, W, 1, pack_out, width, residual, depth, pack, /*fuse_post_output=*/false);
    ASSERT_GT(peak_ord.post_slot_bytes, 0u);
    const std::size_t live_ord =
        2 * peak_ord.trunk_slot_bytes + peak_ord.structural_slot_bytes + peak_ord.post_slot_bytes;
    EXPECT_EQ(peak_ord.peak_live_bytes, live_ord);
    EXPECT_EQ(estimate_fn(H, W, 1, false), peak_ord.estimate_bytes);
    EXPECT_LT(peak_fused.estimate_bytes, peak_ord.estimate_bytes);

    const std::int64_t ph = H / pack;
    const std::int64_t pw = W / pack;
    const std::int64_t mh = ph - 2 * depth;
    const std::int64_t mw = pw - 2 * depth;
    const std::int64_t uh = mh * pack;
    const std::int64_t uw = mw * pack;
    EXPECT_GE(peak_fused.trunk_slot_bytes, TensorBytes(1, pack_out, ph, pw));
    EXPECT_GE(peak_fused.trunk_slot_bytes, TensorBytes(1, width, ph - 2, pw - 2));
    EXPECT_GE(peak_fused.trunk_slot_bytes, TensorBytes(1, residual, mh, mw));
    EXPECT_GE(peak_fused.trunk_slot_bytes, TensorBytes(1, 3, uh, uw));
    EXPECT_GE(peak_fused.trunk_slot_bytes, TensorBytes(1, 6, uh, uw));
    EXPECT_GE(peak_fused.structural_slot_bytes, TensorBytes(1, 3, uh, uw));
    EXPECT_GE(peak_ord.post_slot_bytes, TensorBytes(1, width, uh - 2, uw - 2));
  };

  check(BayerDemosaicNet::kTileInput, BayerDemosaicNet::kTileInput, BayerDemosaicNet::kPackOutCh,
        BayerDemosaicNet::kWidth, BayerDemosaicNet::kResidualCh, BayerDemosaicNet::kDepth,
        BayerDemosaicNet::kPackFactor, &BayerDemosaicNet::EstimateWorkspaceBytes);
  check(XTransDemosaicNet::kTileInput, XTransDemosaicNet::kTileInput, XTransDemosaicNet::kPackOutCh,
        XTransDemosaicNet::kWidth, XTransDemosaicNet::kResidualCh, XTransDemosaicNet::kDepth,
        XTransDemosaicNet::kPackFactor, &XTransDemosaicNet::EstimateWorkspaceBytes);
  // Natural-size mini tiles also covered.
  check(64, 64, BayerDemosaicNet::kPackOutCh, BayerDemosaicNet::kWidth,
        BayerDemosaicNet::kResidualCh, BayerDemosaicNet::kDepth, BayerDemosaicNet::kPackFactor,
        &BayerDemosaicNet::EstimateWorkspaceBytes);
  check(48, 48, XTransDemosaicNet::kPackOutCh, XTransDemosaicNet::kWidth,
        XTransDemosaicNet::kResidualCh, XTransDemosaicNet::kDepth, XTransDemosaicNet::kPackFactor,
        &XTransDemosaicNet::EstimateWorkspaceBytes);
}

TEST_F(MlOpsDemosaicNetTest, StudentForwardRepeatedRunsKeepAllocationGenerationStable) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int kInput  = BayerDemosaicNet::kTileInput;
  constexpr int kOutput = BayerDemosaicNet::kTileOutput;
  cv::Mat       cfa(kInput, kInput, CV_32FC1);
  cv::randu(cfa, 0.0F, 1.0F);
  cv::cuda::GpuMat              gpu_cfa(cfa);
  cv::cuda::GpuMat              rgb;
  const RawCfaPattern           pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  DemosaicNetModelCache         cache;
  DemosaicNetLoadOptions        load_opts;
  load_opts.model_dir = model_dir;
  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache  = &cache;
  options.load_options = load_opts;
  options.workspace    = &workspace;

  const auto first = CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
  ASSERT_TRUE(first.succeeded) << first.error;
  ASSERT_EQ(rgb.size(), cv::Size(kOutput, kOutput));
  const std::uint64_t gen_after_warmup = workspace.allocation_generation();
  ASSERT_GT(gen_after_warmup, 0u);
  const std::size_t owned_after_warmup = workspace.OwnedDeviceBytes();
  ASSERT_GT(owned_after_warmup, 0u);

  for (int i = 0; i < 5; ++i) {
    const auto again = CUDA::DemosaicWithNeuralEngine(gpu_cfa, pattern, rgb, nullptr, options);
    ASSERT_TRUE(again.succeeded) << again.error;
    EXPECT_EQ(workspace.allocation_generation(), gen_after_warmup) << "iteration " << i;
    EXPECT_EQ(workspace.OwnedDeviceBytes(), owned_after_warmup) << "iteration " << i;
  }
}

// ---------------------------------------------------------------------------
// P3 — CUDA Graph launch amortization (fixed-shape model forward)
// ---------------------------------------------------------------------------

namespace {

auto DownloadGpuMatRgb(const cv::cuda::GpuMat& rgb) -> std::vector<float> {
  cv::Mat host;
  rgb.download(host);
  EXPECT_EQ(host.type(), CV_32FC3);
  std::vector<float> out(static_cast<std::size_t>(host.rows) * host.cols * 3);
  if (host.isContinuous()) {
    std::memcpy(out.data(), host.ptr<float>(), out.size() * sizeof(float));
  } else {
    for (int y = 0; y < host.rows; ++y) {
      std::memcpy(out.data() + static_cast<std::size_t>(y) * host.cols * 3, host.ptr<float>(y),
                  static_cast<std::size_t>(host.cols) * 3 * sizeof(float));
    }
  }
  return out;
}

auto MakeStudentTileCfa(const int edge) -> cv::cuda::GpuMat {
  cv::Mat host(edge, edge, CV_32FC1);
  cv::randu(host, 0.05F, 0.95F);
  return cv::cuda::GpuMat(host);
}

auto BuildForwardGraphKey(const DemosaicNetVariant variant, const int device, const int H,
                          const int W, const int Oh, const int Ow,
                          CUDA::NeuralDemosaicWorkspace& workspace, const float* pack_w,
                          const float* out_w,
                          const std::uint64_t weight_generation = 1) -> CUDA::NeuralForwardGraphKey {
  CUDA::NeuralForwardGraphKey key;
  key.variant             = variant;
  key.device              = device;
  key.input_h             = H;
  key.input_w             = W;
  key.output_h            = Oh;
  key.output_w            = Ow;
  key.input_data          = workspace.input_buffer().data();
  key.output_data         = workspace.output_buffer().data();
  key.activation_base     = workspace.activation_workspace().base();
  key.activation_capacity = workspace.activation_workspace().capacity_bytes();
  key.pack_weight         = pack_w;
  key.output_weight       = out_w;
  key.weight_generation   = weight_generation;
  return key;
}

}  // namespace

TEST_F(MlOpsDemosaicNetTest, GraphAndOrdinaryStudentTileMatchBayer) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int kOwned = BayerDemosaicNet::kTileOutput;
  const int     kCfa   = BayerDemosaicNet::kTileInput;  // sufficient canvas for one virtual-pad tile
  auto          cfa    = MakeStudentTileCfa(kCfa + 64);
  const RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  DemosaicNetModelCache  cache;
  DemosaicNetLoadOptions load_opts;
  load_opts.model_dir = model_dir;

  CUDA::NeuralDemosaicWorkspace ws_ord;
  CUDA::NeuralDemosaicWorkspace ws_graph;
  CUDA::NeuralDemosaicOptions   opt_ord;
  opt_ord.model_cache        = &cache;
  opt_ord.load_options       = load_opts;
  opt_ord.workspace          = &ws_ord;
  opt_ord.student_owned_tile_edge = kOwned;
  opt_ord.enable_cuda_graph  = false;

  CUDA::NeuralDemosaicOptions opt_graph = opt_ord;
  opt_graph.workspace                   = &ws_graph;
  opt_graph.enable_cuda_graph           = true;

  cv::cuda::Stream stream;
  cv::cuda::GpuMat rgb_ord;
  cv::cuda::GpuMat rgb_graph;
  const cv::Point  origin(0, 0);

  const auto r0 = CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_ord, &stream,
                                                            opt_ord);
  ASSERT_TRUE(r0.succeeded) << r0.error;
  const auto ordinary = DownloadGpuMatRgb(rgb_ord);

  // Capture + first forward.
  const auto r1 = CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_graph,
                                                            &stream, opt_graph);
  ASSERT_TRUE(r1.succeeded) << r1.error;
  ASSERT_TRUE(ws_graph.forward_graph().ready()) << "expected successful CUDA Graph capture";
  EXPECT_EQ(ws_graph.forward_graph().capture_count(), 1u);
  ExpectVectorsNear(DownloadGpuMatRgb(rgb_graph), ordinary, 1e-4f);

  // Replay.
  const auto r2 = CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_graph,
                                                            &stream, opt_graph);
  ASSERT_TRUE(r2.succeeded) << r2.error;
  EXPECT_EQ(ws_graph.forward_graph().launch_count(), 1u);
  ExpectVectorsNear(DownloadGpuMatRgb(rgb_graph), ordinary, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, GraphAndOrdinaryStudentTileMatchXTrans) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int kOwned = XTransDemosaicNet::kTileOutput;
  auto          cfa    = MakeStudentTileCfa(XTransDemosaicNet::kTileInput + 64);
  const RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::XTrans6x6);
  DemosaicNetModelCache  cache;
  DemosaicNetLoadOptions load_opts;
  load_opts.model_dir = model_dir;

  CUDA::NeuralDemosaicWorkspace ws_ord;
  CUDA::NeuralDemosaicWorkspace ws_graph;
  CUDA::NeuralDemosaicOptions   opt_ord;
  opt_ord.model_cache             = &cache;
  opt_ord.load_options            = load_opts;
  opt_ord.workspace               = &ws_ord;
  opt_ord.student_owned_tile_edge = kOwned;
  opt_ord.enable_cuda_graph       = false;

  CUDA::NeuralDemosaicOptions opt_graph = opt_ord;
  opt_graph.workspace                   = &ws_graph;
  opt_graph.enable_cuda_graph           = true;

  cv::cuda::Stream stream;
  cv::cuda::GpuMat rgb_ord;
  cv::cuda::GpuMat rgb_graph;
  const cv::Point  origin(0, 0);

  const auto r0 = CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_ord, &stream,
                                                            opt_ord);
  ASSERT_TRUE(r0.succeeded) << r0.error;
  const auto ordinary = DownloadGpuMatRgb(rgb_ord);

  const auto r1 = CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_graph,
                                                            &stream, opt_graph);
  ASSERT_TRUE(r1.succeeded) << r1.error;
  ASSERT_TRUE(ws_graph.forward_graph().ready());
  ExpectVectorsNear(DownloadGpuMatRgb(rgb_graph), ordinary, 1e-4f);

  const auto r2 = CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_graph,
                                                            &stream, opt_graph);
  ASSERT_TRUE(r2.succeeded) << r2.error;
  EXPECT_GE(ws_graph.forward_graph().launch_count(), 1u);
  ExpectVectorsNear(DownloadGpuMatRgb(rgb_graph), ordinary, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, GraphReplayPreservesExportedBayerGolden) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_00.bin");
  const auto out_path  = FindGolden("bayer_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N  = 1;
  constexpr int H  = BayerDemosaicNet::kTileInput;
  constexpr int W  = BayerDemosaicNet::kTileInput;
  constexpr int Oh = BayerDemosaicNet::kTileOutput;
  constexpr int Ow = BayerDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  CUDA::NeuralDemosaicWorkspace workspace;
  workspace.EnsureCapacity(DemosaicNetVariant::Bayer, H, W,
                           static_cast<std::size_t>(N * 3 * H * W),
                           static_cast<std::size_t>(N * 3 * Oh * Ow));
  ASSERT_EQ(::cudaMemcpy(workspace.input_buffer().data(), hin.data(), hin.size() * sizeof(float),
                         cudaMemcpyHostToDevice),
            cudaSuccess);

  auto tin  = cuda::nn::DeviceTensor::Contiguous(workspace.input_buffer().data(), {N, 3, H, W});
  auto tout = cuda::nn::DeviceTensor::Contiguous(workspace.output_buffer().data(), {N, 3, Oh, Ow});

  int device = 0;
  ASSERT_EQ(::cudaGetDevice(&device), cudaSuccess);
  cv::cuda::Stream stream;
  const cudaStream_t cuda_stream = cv::cuda::StreamAccessor::getStream(stream);

  const auto key = BuildForwardGraphKey(DemosaicNetVariant::Bayer, device, H, W, Oh, Ow, workspace,
                                        net.PackWeightDevicePtr(), net.OutputWeightDevicePtr());
  auto& graph = workspace.forward_graph();
  auto ordinary = [&]() {
    net.Forward(tin, tout, workspace.activation_workspace(), cuda_stream);
  };
  graph.Capture(key, cuda_stream, ordinary);
  stream.waitForCompletion();
  ASSERT_TRUE(graph.ready());
  ExpectVectorsNear(workspace.output_buffer().Download(), expected, 1e-4f);

  // Scrub output then replay.
  ASSERT_EQ(::cudaMemsetAsync(workspace.output_buffer().data(), 0,
                              workspace.output_buffer().bytes(), cuda_stream),
            cudaSuccess);
  ASSERT_TRUE(graph.TryLaunch(key, cuda_stream));
  stream.waitForCompletion();
  ExpectVectorsNear(workspace.output_buffer().Download(), expected, 1e-4f);
  EXPECT_EQ(graph.launch_count(), 1u);
  EXPECT_EQ(graph.capture_count(), 1u);
}

TEST_F(MlOpsDemosaicNetTest, GraphReplayPreservesExportedXTransGolden) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_00.bin");
  const auto out_path  = FindGolden("xtrans_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N  = 1;
  constexpr int H  = XTransDemosaicNet::kTileInput;
  constexpr int W  = XTransDemosaicNet::kTileInput;
  constexpr int Oh = XTransDemosaicNet::kTileOutput;
  constexpr int Ow = XTransDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  XTransDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "xtrans.safetensors"));

  CUDA::NeuralDemosaicWorkspace workspace;
  workspace.EnsureCapacity(DemosaicNetVariant::XTrans, H, W,
                           static_cast<std::size_t>(N * 3 * H * W),
                           static_cast<std::size_t>(N * 3 * Oh * Ow));
  ASSERT_EQ(::cudaMemcpy(workspace.input_buffer().data(), hin.data(), hin.size() * sizeof(float),
                         cudaMemcpyHostToDevice),
            cudaSuccess);

  auto tin  = cuda::nn::DeviceTensor::Contiguous(workspace.input_buffer().data(), {N, 3, H, W});
  auto tout = cuda::nn::DeviceTensor::Contiguous(workspace.output_buffer().data(), {N, 3, Oh, Ow});

  int device = 0;
  ASSERT_EQ(::cudaGetDevice(&device), cudaSuccess);
  cv::cuda::Stream stream;
  const cudaStream_t cuda_stream = cv::cuda::StreamAccessor::getStream(stream);
  const auto key = BuildForwardGraphKey(DemosaicNetVariant::XTrans, device, H, W, Oh, Ow, workspace,
                                        net.PackWeightDevicePtr(), net.OutputWeightDevicePtr());
  auto& graph = workspace.forward_graph();
  graph.Capture(key, cuda_stream, [&]() {
    net.Forward(tin, tout, workspace.activation_workspace(), cuda_stream);
  });
  stream.waitForCompletion();
  ASSERT_TRUE(graph.ready());
  ExpectVectorsNear(workspace.output_buffer().Download(), expected, 1e-4f);

  ASSERT_EQ(::cudaMemsetAsync(workspace.output_buffer().data(), 0,
                              workspace.output_buffer().bytes(), cuda_stream),
            cudaSuccess);
  ASSERT_TRUE(graph.TryLaunch(key, cuda_stream));
  stream.waitForCompletion();
  ExpectVectorsNear(workspace.output_buffer().Download(), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, ModelReloadInvalidatesCapturedWeightPointers) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int kOwned = BayerDemosaicNet::kTileOutput;
  auto          cfa    = MakeStudentTileCfa(BayerDemosaicNet::kTileInput + 64);
  const RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  DemosaicNetModelCache  cache;
  DemosaicNetLoadOptions load_opts;
  load_opts.model_dir = model_dir;

  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache             = &cache;
  options.load_options            = load_opts;
  options.workspace               = &workspace;
  options.student_owned_tile_edge = kOwned;
  options.enable_cuda_graph       = true;  // P3 path under test (product default is off)

  cv::cuda::Stream stream;
  cv::cuda::GpuMat rgb;
  const cv::Point  origin(0, 0);

  ASSERT_TRUE(CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb, &stream, options)
                  .succeeded);
  ASSERT_TRUE(workspace.forward_graph().ready());
  const auto gen_before = cache.WeightGeneration(DemosaicNetVariant::Bayer);
  const auto captures0  = workspace.forward_graph().capture_count();
  const auto key_gen0   = workspace.forward_graph().key().weight_generation;
  EXPECT_EQ(key_gen0, gen_before);

  cache.Unload(DemosaicNetVariant::Bayer);
  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, load_opts));
  const auto gen_after = cache.WeightGeneration(DemosaicNetVariant::Bayer);
  ASSERT_NE(gen_before, gen_after);

  // Product path keys on weight generation: mismatch invalidates + recaptures even
  // when CUDA recycles the same device addresses for the reloaded weights.
  ASSERT_TRUE(CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb, &stream, options)
                  .succeeded);
  EXPECT_TRUE(workspace.forward_graph().ready());
  EXPECT_EQ(workspace.forward_graph().capture_count(), captures0 + 1);
  EXPECT_EQ(workspace.forward_graph().key().weight_generation, gen_after);
}

TEST_F(MlOpsDemosaicNetTest, WorkspaceGrowthInvalidatesCapturedActivationPointers) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int H  = 64;
  constexpr int W  = 64;
  const int     Oh = BayerDemosaicNet::OutputHeight(H, W);
  const int     Ow = BayerDemosaicNet::OutputWidth(W, H);

  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  CUDA::NeuralDemosaicWorkspace workspace;
  workspace.EnsureCapacity(DemosaicNetVariant::Bayer, H, W,
                           static_cast<std::size_t>(3 * H * W),
                           static_cast<std::size_t>(3 * Oh * Ow));

  int device = 0;
  ASSERT_EQ(::cudaGetDevice(&device), cudaSuccess);
  cv::cuda::Stream stream;
  const cudaStream_t cuda_stream = cv::cuda::StreamAccessor::getStream(stream);

  auto tin  = cuda::nn::DeviceTensor::Contiguous(workspace.input_buffer().data(), {1, 3, H, W});
  auto tout = cuda::nn::DeviceTensor::Contiguous(workspace.output_buffer().data(), {1, 3, Oh, Ow});
  const auto key = BuildForwardGraphKey(DemosaicNetVariant::Bayer, device, H, W, Oh, Ow, workspace,
                                        net.PackWeightDevicePtr(), net.OutputWeightDevicePtr());
  workspace.forward_graph().Capture(key, cuda_stream, [&]() {
    net.Forward(tin, tout, workspace.activation_workspace(), cuda_stream);
  });
  stream.waitForCompletion();
  ASSERT_TRUE(workspace.forward_graph().ready());

  // Forward leaves bump allocations live; rewind before growth (product EnsureCapacity
  // runs with empty bump after each tile's host-side Reset at the next Forward, but a
  // graph-only capture leaves the bump advanced until Reset).
  workspace.activation_workspace().Reset();

  // Grow activation (and possibly I/O) beyond the captured tile.
  constexpr int H2  = BayerDemosaicNet::kTileInput;
  constexpr int W2  = BayerDemosaicNet::kTileInput;
  constexpr int Oh2 = BayerDemosaicNet::kTileOutput;
  constexpr int Ow2 = BayerDemosaicNet::kTileOutput;
  workspace.EnsureCapacity(DemosaicNetVariant::Bayer, H2, W2,
                           static_cast<std::size_t>(3 * H2 * W2),
                           static_cast<std::size_t>(3 * Oh2 * Ow2));
  EXPECT_FALSE(workspace.forward_graph().ready())
      << "workspace growth must drop the captured graph before pointers go stale";
}

TEST_F(MlOpsDemosaicNetTest, GraphReplayDoesNotChangeAllocationGeneration) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int kOwned = BayerDemosaicNet::kTileOutput;
  auto          cfa    = MakeStudentTileCfa(BayerDemosaicNet::kTileInput + 64);
  const RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  DemosaicNetModelCache  cache;
  DemosaicNetLoadOptions load_opts;
  load_opts.model_dir = model_dir;

  CUDA::NeuralDemosaicWorkspace workspace;
  CUDA::NeuralDemosaicOptions   options;
  options.model_cache             = &cache;
  options.load_options            = load_opts;
  options.workspace               = &workspace;
  options.student_owned_tile_edge = kOwned;
  options.enable_cuda_graph       = true;

  cv::cuda::Stream stream;
  cv::cuda::GpuMat rgb;
  const cv::Point  origin(0, 0);

  ASSERT_TRUE(CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb, &stream, options)
                  .succeeded);
  ASSERT_TRUE(workspace.forward_graph().ready());
  const std::uint64_t gen = workspace.allocation_generation();
  const std::size_t owned = workspace.OwnedDeviceBytes();

  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(
        CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb, &stream, options)
            .succeeded)
        << "iteration " << i;
    EXPECT_EQ(workspace.allocation_generation(), gen) << "iteration " << i;
    EXPECT_EQ(workspace.OwnedDeviceBytes(), owned) << "iteration " << i;
  }
  EXPECT_GE(workspace.forward_graph().launch_count(), 8u);
  EXPECT_EQ(workspace.forward_graph().capture_count(), 1u);
}

TEST_F(MlOpsDemosaicNetTest, TwoWorkspacesOwnIndependentGraphExecutables) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int kOwned = BayerDemosaicNet::kTileOutput;
  auto          cfa    = MakeStudentTileCfa(BayerDemosaicNet::kTileInput + 64);
  const RawCfaPattern pattern = DemosaicNetTrainingPattern(RawCfaKind::Bayer2x2);
  DemosaicNetModelCache  cache;
  DemosaicNetLoadOptions load_opts;
  load_opts.model_dir = model_dir;

  CUDA::NeuralDemosaicWorkspace ws_a;
  CUDA::NeuralDemosaicWorkspace ws_b;
  CUDA::NeuralDemosaicOptions   opt_a;
  opt_a.model_cache             = &cache;
  opt_a.load_options            = load_opts;
  opt_a.workspace               = &ws_a;
  opt_a.student_owned_tile_edge = kOwned;
  opt_a.enable_cuda_graph       = true;
  CUDA::NeuralDemosaicOptions opt_b = opt_a;
  opt_b.workspace                   = &ws_b;

  cv::cuda::Stream stream_a;
  cv::cuda::Stream stream_b;
  cv::cuda::GpuMat rgb_a;
  cv::cuda::GpuMat rgb_b;
  const cv::Point  origin(0, 0);

  ASSERT_TRUE(
      CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_a, &stream_a, opt_a)
          .succeeded);
  ASSERT_TRUE(
      CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_b, &stream_b, opt_b)
          .succeeded);
  ASSERT_TRUE(ws_a.forward_graph().ready());
  ASSERT_TRUE(ws_b.forward_graph().ready());
  // Distinct executables: independent launch counters and activation bases.
  EXPECT_NE(ws_a.activation_workspace().base(), ws_b.activation_workspace().base());
  EXPECT_NE(ws_a.forward_graph().key().activation_base, ws_b.forward_graph().key().activation_base);

  ASSERT_TRUE(
      CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_a, &stream_a, opt_a)
          .succeeded);
  ASSERT_TRUE(
      CUDA::DemosaicStudentTileWithNeuralEngine(cfa, origin, pattern, rgb_b, &stream_b, opt_b)
          .succeeded);
  EXPECT_EQ(ws_a.forward_graph().launch_count(), 1u);
  EXPECT_EQ(ws_b.forward_graph().launch_count(), 1u);
  EXPECT_EQ(ws_a.forward_graph().capture_count(), 1u);
  EXPECT_EQ(ws_b.forward_graph().capture_count(), 1u);

  const auto ha = DownloadGpuMatRgb(rgb_a);
  const auto hb = DownloadGpuMatRgb(rgb_b);
  ExpectVectorsNear(ha, hb, 1e-4f);
}

// ---------------------------------------------------------------------------
// P4-A — fused post/output tail (exact student 6→24/32→3)
// ---------------------------------------------------------------------------

TEST_F(MlOpsDemosaicNetTest, FusedPostOutputMatchesUnfusedStudentForward) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_00.bin");
  const auto out_path  = FindGolden("bayer_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N  = 1;
  constexpr int H  = BayerDemosaicNet::kTileInput;
  constexpr int W  = BayerDemosaicNet::kTileInput;
  constexpr int Oh = BayerDemosaicNet::kTileOutput;
  constexpr int Ow = BayerDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  auto tin = d_in.AsTensor({N, 3, H, W});

  cuda::nn::DeviceBufferF32 d_fused(expected.size());
  cuda::nn::DeviceBufferF32 d_ord(expected.size());
  auto tout_f = d_fused.AsTensor({N, 3, Oh, Ow});
  auto tout_o = d_ord.AsTensor({N, 3, Oh, Ow});

  cuda::nn::WorkspacePool ws_f(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N, true));
  cuda::nn::WorkspacePool ws_o(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N, false));
  net.Forward(tin, tout_f, ws_f, nullptr, /*force_ordinary_tail=*/false);
  net.Forward(tin, tout_o, ws_o, nullptr, /*force_ordinary_tail=*/true);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ExpectVectorsNear(d_fused.Download(), expected, 1e-4f);
  ExpectVectorsNear(d_ord.Download(), expected, 1e-4f);
  ExpectVectorsNear(d_fused.Download(), d_ord.Download(), 1e-5f);
  EXPECT_LT(ws_f.capacity_bytes(), ws_o.capacity_bytes());
}

TEST_F(MlOpsDemosaicNetTest, FusedPostOutputMatchesUnfusedXTransStudentForward) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_00.bin");
  const auto out_path  = FindGolden("xtrans_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N  = 1;
  constexpr int H  = XTransDemosaicNet::kTileInput;
  constexpr int W  = XTransDemosaicNet::kTileInput;
  constexpr int Oh = XTransDemosaicNet::kTileOutput;
  constexpr int Ow = XTransDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  XTransDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "xtrans.safetensors"));

  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  auto tin = d_in.AsTensor({N, 3, H, W});

  cuda::nn::DeviceBufferF32 d_fused(expected.size());
  cuda::nn::DeviceBufferF32 d_ord(expected.size());
  auto tout_f = d_fused.AsTensor({N, 3, Oh, Ow});
  auto tout_o = d_ord.AsTensor({N, 3, Oh, Ow});

  cuda::nn::WorkspacePool ws_f(XTransDemosaicNet::EstimateWorkspaceBytes(H, W, N, true));
  cuda::nn::WorkspacePool ws_o(XTransDemosaicNet::EstimateWorkspaceBytes(H, W, N, false));
  net.Forward(tin, tout_f, ws_f, nullptr, /*force_ordinary_tail=*/false);
  net.Forward(tin, tout_o, ws_o, nullptr, /*force_ordinary_tail=*/true);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ExpectVectorsNear(d_fused.Download(), expected, 1e-4f);
  ExpectVectorsNear(d_ord.Download(), expected, 1e-4f);
  ExpectVectorsNear(d_fused.Download(), d_ord.Download(), 1e-5f);
}

TEST_F(MlOpsDemosaicNetTest, FusedStudentHwcOutputMatchesOrdinaryNchwUnpack) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_00.bin");
  const auto out_path  = FindGolden("bayer_output_00.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or student goldens not found";
  }

  constexpr int N  = 1;
  constexpr int H  = BayerDemosaicNet::kTileInput;
  constexpr int W  = BayerDemosaicNet::kTileInput;
  constexpr int Oh = BayerDemosaicNet::kTileOutput;
  constexpr int Ow = BayerDemosaicNet::kTileOutput;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  auto tin = d_in.AsTensor({N, 3, H, W});

  cuda::nn::DeviceBufferF32 d_nchw(expected.size());
  auto tout = d_nchw.AsTensor({N, 3, Oh, Ow});
  cuda::nn::WorkspacePool ws(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N, true));
  net.Forward(tin, tout, ws, nullptr, /*force_ordinary_tail=*/false);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cv::cuda::GpuMat hwc(Oh, Ow, CV_32FC3);
  cuda::nn::WorkspacePool ws_hwc(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N, true));
  net.ForwardHwc(tin, reinterpret_cast<float*>(hwc.ptr()), static_cast<std::size_t>(hwc.step),
                 ws_hwc, nullptr, /*apply_gamma_decode=*/false);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  // NCHW golden → HWC order for comparison.
  const auto nchw = d_nchw.Download();
  std::vector<float> expected_hwc(static_cast<std::size_t>(Oh) * Ow * 3);
  const std::size_t  plane = static_cast<std::size_t>(Oh) * Ow;
  for (int y = 0; y < Oh; ++y) {
    for (int x = 0; x < Ow; ++x) {
      const std::size_t pix = static_cast<std::size_t>(y) * Ow + x;
      expected_hwc[pix * 3 + 0] = nchw[0 * plane + pix];
      expected_hwc[pix * 3 + 1] = nchw[1 * plane + pix];
      expected_hwc[pix * 3 + 2] = nchw[2 * plane + pix];
    }
  }
  ExpectVectorsNear(DownloadGpuMatRgb(hwc), expected_hwc, 1e-5f);
  ExpectVectorsNear(nchw, expected, 1e-4f);
}

}  // namespace alcedo
