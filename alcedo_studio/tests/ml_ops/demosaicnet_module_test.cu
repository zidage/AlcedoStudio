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

// Product forward writes pitched HWC; goldens remain NCHW [1,3,H,W].
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

template <typename Net>
auto ForwardHwcAsNchw(const Net& net, const cuda::nn::DeviceTensor& input, int out_h, int out_w,
                      cuda::nn::WorkspacePool& workspace) -> std::vector<float> {
  cv::cuda::GpuMat hwc(out_h, out_w, CV_32FC3);
  net.ForwardHwcChannelsLast(input, reinterpret_cast<float*>(hwc.ptr()),
                             static_cast<std::size_t>(hwc.step), workspace, nullptr,
                             /*apply_gamma_decode=*/false);
  if (::cudaDeviceSynchronize() != cudaSuccess) {
    throw std::runtime_error("ForwardHwcChannelsLast synchronize failed");
  }
  return HwcToNchw(DownloadGpuMatRgb(hwc), out_h, out_w);
}

// Pre-P1 sum-of-all-activations estimate (peak-live product path must be smaller).
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
  EXPECT_NE(net.OutputWeightCioDevicePtr(), nullptr);

  auto bad     = map;
  auto mutated = bad.at("trunk.0.weight");
  mutated.shape = {24, 5, 3, 3};  // wrong Cin
  mutated.data.assign(mutated.numel(), 0.0f);
  bad.InsertOrAssign(std::move(mutated));

  BayerDemosaicNet bad_net;
  EXPECT_THROW(bad_net.LoadWeights(bad), std::runtime_error);
  EXPECT_FALSE(bad_net.weights_loaded());
}

TEST_F(MlOpsDemosaicNetTest, BayerLoadWeightsRejectsWrongArchitectureMetadata) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }
  auto map  = cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors");
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

  auto bad     = map;
  auto mutated = bad.at("post_conv.weight");
  mutated.shape = {32, 5, 3, 3};  // wrong Cin (must be 6)
  mutated.data.assign(mutated.numel(), 0.0f);
  bad.InsertOrAssign(std::move(mutated));

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

  const auto&  m1 = cache.Bayer();
  const float* w0 = m1.PackWeightDevicePtr();
  const float* w1 = m1.OutputWeightCioDevicePtr();

  constexpr int H = 64, W = 64, N = 1;
  const int     Oh  = BayerDemosaicNet::OutputHeight(H, W);
  const int     Ow  = BayerDemosaicNet::OutputWidth(W, H);
  const auto    hin = MakePattern(static_cast<std::size_t>(N * 3 * H * W), 7);
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  auto tin = d_in.AsTensor({N, 3, H, W});

  cuda::nn::WorkspacePool ws;
  ws.Reserve(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  (void)ForwardHwcAsNchw(m1, tin, Oh, Ow, ws);

  ASSERT_TRUE(cache.EnsureLoaded(DemosaicNetVariant::Bayer, opts));
  const auto& m2 = cache.Bayer();
  EXPECT_EQ(m2.PackWeightDevicePtr(), w0);
  EXPECT_EQ(m2.OutputWeightCioDevicePtr(), w1);

  (void)ForwardHwcAsNchw(m2, tin, Oh, Ow, ws);
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
  cuda::nn::WorkspacePool ws(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  ExpectVectorsNear(ForwardHwcAsNchw(net, tin, Oh, Ow, ws), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, BayerStudentForwardMatchesExportedGolden01) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("bayer_input_01.bin");
  const auto out_path  = FindGolden("bayer_output_01.bin");
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
  cuda::nn::WorkspacePool ws(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  ExpectVectorsNear(ForwardHwcAsNchw(net, tin, Oh, Ow, ws), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, XTransStudentForwardMatchesExportedGolden00) {
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
  cuda::nn::WorkspacePool ws(XTransDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  ExpectVectorsNear(ForwardHwcAsNchw(net, tin, Oh, Ow, ws), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, XTransStudentForwardMatchesExportedGolden01) {
  const auto model_dir = FindModelDir();
  const auto in_path   = FindGolden("xtrans_input_01.bin");
  const auto out_path  = FindGolden("xtrans_output_01.bin");
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
  cuda::nn::WorkspacePool ws(XTransDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  ExpectVectorsNear(ForwardHwcAsNchw(net, tin, Oh, Ow, ws), expected, 1e-4f);
}

TEST_F(MlOpsDemosaicNetTest, BayerStudentGoldenCornerPixelsFinite) {
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
  cuda::nn::WorkspacePool ws(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
  const auto out = ForwardHwcAsNchw(net, tin, Oh, Ow, ws);

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
// Concurrent product forwards: two threads, two workspaces, shared module
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
  const int     Oh  = BayerDemosaicNet::OutputHeight(H, W);
  const int     Ow  = BayerDemosaicNet::OutputWidth(W, H);
  const auto    hin = MakePattern(static_cast<std::size_t>(N * 3 * H * W), 99);
  std::atomic<int> ok{0};
  std::atomic<int> fail{0};

  auto worker = [&](int /*seed_offset*/) {
    try {
      cuda::nn::DeviceBufferF32 d_in(hin.size());
      d_in.Upload(hin);
      auto tin = d_in.AsTensor({N, 3, H, W});
      cuda::nn::WorkspacePool ws;
      ws.Reserve(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N));
      (void)ForwardHwcAsNchw(net, tin, Oh, Ow, ws);
      if (net.PackWeightDevicePtr() != pack_w) {
        fail.fetch_add(1);
        return;
      }
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
  EXPECT_EQ(BayerDemosaicNet::OutputHeight(64, 64), 64 - BayerDemosaicNet::kNaturalSpatialLoss);
  EXPECT_EQ(BayerDemosaicNet::OutputWidth(128, 64), 128 - BayerDemosaicNet::kNaturalSpatialLoss);
  EXPECT_EQ(XTransDemosaicNet::OutputHeight(48, 48), 48 - XTransDemosaicNet::kNaturalSpatialLoss);
  EXPECT_EQ(XTransDemosaicNet::OutputWidth(26, 48), 26 - XTransDemosaicNet::kNaturalSpatialLoss);
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
// Peak-live workspace + two trunk slots (product ForwardHwcChannelsLast)
// ---------------------------------------------------------------------------

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
  EXPECT_TRUE(peak.fuse_post_output);
  EXPECT_EQ(peak.post_slot_bytes, 0u);
  EXPECT_LT(peak.estimate_bytes, legacy);
  EXPECT_LE(peak.estimate_bytes, (legacy * 3) / 4);
  EXPECT_EQ(BayerDemosaicNet::EstimateWorkspaceBytes(H, W, N), peak.estimate_bytes);

  const auto hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));
  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  auto tin = d_in.AsTensor({N, 3, H, W});

  // Exact peak-live reserve: product path must not need the legacy sum-of-all slab.
  cuda::nn::WorkspacePool ws;
  ws.Reserve(peak.estimate_bytes);
  ExpectVectorsNear(ForwardHwcAsNchw(net, tin, Oh, Ow, ws), expected, 1e-4f);
  // NHWC product forward materializes two trunk slots; structural is reserved but
  // residual/unpack/concat reuse the dead trunk side.
  EXPECT_EQ(ws.used_bytes(), 2 * peak.trunk_slot_bytes);
  EXPECT_EQ(ws.capacity_bytes(), peak.estimate_bytes);
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
  EXPECT_TRUE(peak.fuse_post_output);
  EXPECT_LT(peak.estimate_bytes, legacy);
  EXPECT_LE(peak.estimate_bytes, (legacy * 3) / 4);
  EXPECT_EQ(XTransDemosaicNet::EstimateWorkspaceBytes(H, W, N), peak.estimate_bytes);

  const auto hin      = LoadFloatBin(in_path, static_cast<std::size_t>(N * 3 * H * W));
  const auto expected = LoadFloatBin(out_path, static_cast<std::size_t>(N * 3 * Oh * Ow));
  XTransDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "xtrans.safetensors"));
  cuda::nn::DeviceBufferF32 d_in(hin.size());
  d_in.Upload(hin);
  auto tin = d_in.AsTensor({N, 3, H, W});
  cuda::nn::WorkspacePool ws;
  ws.Reserve(peak.estimate_bytes);
  ExpectVectorsNear(ForwardHwcAsNchw(net, tin, Oh, Ow, ws), expected, 1e-4f);
  EXPECT_EQ(ws.used_bytes(), 2 * peak.trunk_slot_bytes);
}

TEST_F(MlOpsDemosaicNetTest, StudentForwardPeakWorkspaceEstimateCoversEveryLiveSlot) {
  auto check = [](int H, int W, int pack_out, int width, int residual, int depth, int pack,
                  auto estimate_fn) {
    using demosaicnet_slots::TensorBytes;
    const auto peak = demosaicnet_slots::ComputePeakLiveSlots(H, W, 1, pack_out, width, residual,
                                                              depth, pack);
    ASSERT_GT(peak.trunk_slot_bytes, 0u);
    ASSERT_GT(peak.structural_slot_bytes, 0u);
    EXPECT_EQ(peak.post_slot_bytes, 0u);
    EXPECT_TRUE(peak.fuse_post_output);
    const std::size_t live = 2 * peak.trunk_slot_bytes + peak.structural_slot_bytes;
    EXPECT_EQ(peak.peak_live_bytes, live);
    EXPECT_EQ(peak.estimate_bytes, live + demosaicnet_slots::kScratchHeadroomBytes);
    EXPECT_EQ(estimate_fn(H, W, 1), peak.estimate_bytes);

    const std::int64_t ph = H / pack;
    const std::int64_t pw = W / pack;
    const std::int64_t mh = ph - 2 * depth;
    const std::int64_t mw = pw - 2 * depth;
    const std::int64_t uh = mh * pack;
    const std::int64_t uw = mw * pack;
    EXPECT_GE(peak.trunk_slot_bytes, TensorBytes(1, pack_out, ph, pw));
    EXPECT_GE(peak.trunk_slot_bytes, TensorBytes(1, width, ph - 2, pw - 2));
    EXPECT_GE(peak.trunk_slot_bytes, TensorBytes(1, residual, mh, mw));
    EXPECT_GE(peak.trunk_slot_bytes, TensorBytes(1, 3, uh, uw));
    EXPECT_GE(peak.trunk_slot_bytes, TensorBytes(1, 6, uh, uw));
    EXPECT_GE(peak.structural_slot_bytes, TensorBytes(1, 3, uh, uw));
  };

  check(BayerDemosaicNet::kTileInput, BayerDemosaicNet::kTileInput, BayerDemosaicNet::kPackOutCh,
        BayerDemosaicNet::kWidth, BayerDemosaicNet::kResidualCh, BayerDemosaicNet::kDepth,
        BayerDemosaicNet::kPackFactor, &BayerDemosaicNet::EstimateWorkspaceBytes);
  check(XTransDemosaicNet::kTileInput, XTransDemosaicNet::kTileInput, XTransDemosaicNet::kPackOutCh,
        XTransDemosaicNet::kWidth, XTransDemosaicNet::kResidualCh, XTransDemosaicNet::kDepth,
        XTransDemosaicNet::kPackFactor, &XTransDemosaicNet::EstimateWorkspaceBytes);
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

TEST_F(MlOpsDemosaicNetTest, EnsureCapacityFourArgDoesNotGrowAfterWarmup) {
  const auto model_dir = FindModelDir();
  if (model_dir.empty()) {
    GTEST_SKIP() << "model dir not found";
  }

  constexpr int H  = BayerDemosaicNet::kTileInput;
  constexpr int W  = BayerDemosaicNet::kTileInput;
  constexpr int Oh = BayerDemosaicNet::kTileOutput;
  constexpr int Ow = BayerDemosaicNet::kTileOutput;
  const auto hin = MakePattern(static_cast<std::size_t>(3 * H * W), 11);

  BayerDemosaicNet net;
  net.LoadWeights(cuda::nn::LoadSafetensors(model_dir / "bayer.safetensors"));

  CUDA::NeuralDemosaicWorkspace workspace;
  workspace.EnsureCapacity(DemosaicNetVariant::Bayer, H, W,
                           static_cast<std::size_t>(3 * H * W));
  const auto gen0 = workspace.allocation_generation();
  ASSERT_GT(gen0, 0u);

  ASSERT_EQ(::cudaMemcpy(workspace.input_buffer().data(), hin.data(), hin.size() * sizeof(float),
                         cudaMemcpyHostToDevice),
            cudaSuccess);
  auto tin = cuda::nn::DeviceTensor::Contiguous(workspace.input_buffer().data(), {1, 3, H, W});
  cv::cuda::GpuMat& rgb = workspace.rgb_buffer();
  ASSERT_FALSE(rgb.empty());
  ASSERT_EQ(rgb.rows, Oh);
  ASSERT_EQ(rgb.cols, Ow);

  for (int i = 0; i < 4; ++i) {
    net.ForwardHwcChannelsLast(tin, reinterpret_cast<float*>(rgb.ptr()),
                               static_cast<std::size_t>(rgb.step),
                               workspace.activation_workspace(), nullptr,
                               /*apply_gamma_decode=*/false);
    ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
    workspace.EnsureCapacity(DemosaicNetVariant::Bayer, H, W,
                             static_cast<std::size_t>(3 * H * W));
    EXPECT_EQ(workspace.allocation_generation(), gen0) << "iteration " << i;
  }
}

}  // namespace alcedo
