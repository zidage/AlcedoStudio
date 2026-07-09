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

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/safetensors.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"
#include "decoders/processor/nn/demosaicnet_bayer.hpp"
#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/nn/demosaicnet_xtrans.hpp"

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
  auto mutated = bad.at("conv1.weight");
  mutated.shape = {64, 5, 3, 3};  // wrong Cin
  mutated.data.assign(mutated.numel(), 0.0f);
  bad.Insert(std::move(mutated));

  BayerDemosaicNet bad_net;
  EXPECT_THROW(bad_net.LoadWeights(bad), std::runtime_error);
  EXPECT_FALSE(bad_net.weights_loaded());
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
  auto mutated = bad.at("post_conv1.weight");
  mutated.shape = {64, 66, 3, 3};  // wrong Cin (must be 67)
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

  // Two sequential forwards must not reload weights.
  constexpr int H = 64, W = 64, N = 1;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * 3 * H * W), 7);
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(
      static_cast<std::size_t>(N) * 3 * BayerDemosaicNet::OutputHeight(H) *
      BayerDemosaicNet::OutputWidth(W));

  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor(
      {N, 3, BayerDemosaicNet::OutputHeight(H), BayerDemosaicNet::OutputWidth(W)});

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
// Golden forward (PyTorch reference dumps)
// ---------------------------------------------------------------------------

TEST_F(MlOpsDemosaicNetTest, BayerForwardMatchesPyTorchGolden) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_64_in.bin");
  const auto out_path  = FindGolden("bayer_64_out.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or goldens not found";
  }

  constexpr int N = 1, H = 64, W = 64;
  constexpr int Oh = 2, Ow = 2;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  auto map = cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors");
  BayerDemosaicNet net;
  net.LoadWeights(map);

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

TEST_F(MlOpsDemosaicNetTest, XTransForwardMatchesPyTorchGolden) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_48_in.bin");
  const auto out_path  = FindGolden("xtrans_48_out.bin");
  if (model_dir.empty() || in_path.empty() || out_path.empty()) {
    GTEST_SKIP() << "models or goldens not found";
  }

  constexpr int N = 1, H = 48, W = 48;
  constexpr int Oh = 24, Ow = 24;
  const auto    hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto    expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));

  auto map = cuda::nn::LoadSafetensors(model_dir / "xtrans.safetensors");
  XTransDemosaicNet net;
  net.LoadWeights(map);

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

TEST_F(MlOpsDemosaicNetTest, BayerGoldenCornerPixelsAndStats) {
  // Locked expected values from PyTorch seed=12345, H=W=64 (full out is 2×2×3).
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_64_in.bin");
  if (model_dir.empty() || in_path.empty()) {
    GTEST_SKIP() << "models or goldens not found";
  }

  constexpr int N = 1, H = 64, W = 64;
  const auto    hin = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  cuda::nn::DeviceBufferF32 d_out(12);
  auto tin  = d_in.AsTensor({N, 3, H, W});
  auto tout = d_out.AsTensor({N, 3, 2, 2});
  cuda::nn::WorkspacePool ws(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  net.Forward(tin, tout, ws);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  const auto out = d_out.Download();

  // Corner RGB at (0,0) and (1,1)
  EXPECT_NEAR(out[0], 2.05449295f, 1e-4f);
  EXPECT_NEAR(out[4], 1.08356583f, 1e-4f);
  EXPECT_NEAR(out[8], 1.52744353f, 1e-4f);
  EXPECT_NEAR(out[3], 2.26219130f, 1e-4f);
  EXPECT_NEAR(out[7], 0.53580523f, 1e-4f);
  EXPECT_NEAR(out[11], 1.59372783f, 1e-4f);
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
  const auto    hin = MakePattern(static_cast<std::size_t>(N * 3 * H * W), 99);
  std::atomic<int> ok{0};
  std::atomic<int> fail{0};

  auto worker = [&](int seed_offset) {
    try {
      cuda::nn::DeviceBufferF32 d_in(hin.size());
      d_in.Upload(hin);
      cuda::nn::DeviceBufferF32 d_out(
          static_cast<std::size_t>(N) * 3 * BayerDemosaicNet::OutputHeight(H) *
          BayerDemosaicNet::OutputWidth(W));
      auto tin  = d_in.AsTensor({N, 3, H, W});
      auto tout = d_out.AsTensor(
          {N, 3, BayerDemosaicNet::OutputHeight(H), BayerDemosaicNet::OutputWidth(W)});

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
  EXPECT_EQ(BayerDemosaicNet::OutputHeight(64), 2);
  EXPECT_EQ(BayerDemosaicNet::OutputWidth(128), 66);
  EXPECT_EQ(XTransDemosaicNet::OutputHeight(48), 24);
  EXPECT_EQ(XTransDemosaicNet::OutputWidth(26), 2);
  EXPECT_GT(BayerDemosaicNet::EstimateWorkspaceBytes(64, 64, 1), 0u);
  EXPECT_GT(XTransDemosaicNet::EstimateWorkspaceBytes(48, 48, 1), 0u);
}

}  // namespace alcedo
