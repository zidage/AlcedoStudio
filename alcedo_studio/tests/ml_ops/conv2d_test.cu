//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/conv2d.hpp"
#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/relu.hpp"
#include "cuda/nn/safetensors.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto MakePattern(std::size_t n, std::uint32_t seed) -> std::vector<float> {
  std::mt19937                          rng(seed);
  std::uniform_real_distribution<float> dist(-1.0f, 1.0f);
  std::vector<float>                    v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = dist(rng);
  }
  return v;
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

// Naive NCHW valid/padded conv (groups=1), OIHW weights — CPU reference.
auto CpuConv2d(const std::vector<float>& input, const std::vector<float>& weight,
               const std::vector<float>* bias, int N, int Cin, int Cout, int H, int W, int kH,
               int kW, int sH, int sW, int padH, int padW, int dil, bool relu) -> std::vector<float> {
  const int Ho = cuda::nn::Conv2dOutputSize(H, padH, dil, kH, sH);
  const int Wo = cuda::nn::Conv2dOutputSize(W, padW, dil, kW, sW);
  EXPECT_GT(Ho, 0);
  EXPECT_GT(Wo, 0);
  std::vector<float> out(static_cast<std::size_t>(N) * Cout * Ho * Wo, 0.0f);

  auto in_at = [&](int n, int c, int h, int w) -> float {
    if (h < 0 || h >= H || w < 0 || w >= W) {
      return 0.0f;
    }
    const std::size_t idx = static_cast<std::size_t>(n) * Cin * H * W +
                            static_cast<std::size_t>(c) * H * W +
                            static_cast<std::size_t>(h) * W + static_cast<std::size_t>(w);
    return input[idx];
  };

  for (int n = 0; n < N; ++n) {
    for (int co = 0; co < Cout; ++co) {
      for (int oh = 0; oh < Ho; ++oh) {
        for (int ow = 0; ow < Wo; ++ow) {
          float acc = 0.0f;
          for (int ci = 0; ci < Cin; ++ci) {
            for (int kh = 0; kh < kH; ++kh) {
              for (int kw = 0; kw < kW; ++kw) {
                const int ih = oh * sH - padH + kh * dil;
                const int iw = ow * sW - padW + kw * dil;
                const std::size_t wi =
                    static_cast<std::size_t>(co) * Cin * kH * kW +
                    static_cast<std::size_t>(ci) * kH * kW +
                    static_cast<std::size_t>(kh) * kW + static_cast<std::size_t>(kw);
                acc += in_at(n, ci, ih, iw) * weight[wi];
              }
            }
          }
          if (bias != nullptr) {
            acc += (*bias)[static_cast<std::size_t>(co)];
          }
          if (relu) {
            acc = std::max(acc, 0.0f);
          }
          const std::size_t oi = static_cast<std::size_t>(n) * Cout * Ho * Wo +
                                 static_cast<std::size_t>(co) * Ho * Wo +
                                 static_cast<std::size_t>(oh) * Wo + static_cast<std::size_t>(ow);
          out[oi] = acc;
        }
      }
    }
  }
  return out;
}

// Resolve model path relative to common build / source layouts.
auto FindModelPath(const char* filename) -> std::string {
  const char* candidates[] = {
      "alcedo_studio/src/config/models/",
      "../alcedo_studio/src/config/models/",
      "../../alcedo_studio/src/config/models/",
      "../../../alcedo_studio/src/config/models/",
      "src/config/models/",
      "../src/config/models/",
  };
  for (const char* prefix : candidates) {
    std::string path = std::string(prefix) + filename;
    std::ifstream f(path, std::ios::binary);
    if (f) {
      return path;
    }
  }
  // Absolute-ish fallback from env / known workspace.
  const char* extra[] = {
      "D:/Projects/pu-erh_lab/alcedo_studio/src/config/models/",
  };
  for (const char* prefix : extra) {
    std::string path = std::string(prefix) + filename;
    std::ifstream f(path, std::ios::binary);
    if (f) {
      return path;
    }
  }
  return {};
}

auto RunGpuConv(const std::vector<float>& h_in, const std::vector<float>& h_w,
                const std::vector<float>* h_b, int N, int Cin, int Cout, int H, int W, int kH,
                int kW, int sH, int sW, int padH, int padW, bool relu, cuda::nn::WorkspacePool* ws)
    -> std::vector<float> {
  const int Ho = cuda::nn::Conv2dOutputSize(H, padH, 1, kH, sH);
  const int Wo = cuda::nn::Conv2dOutputSize(W, padW, 1, kW, sW);
  const std::size_t in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t w_n   = static_cast<std::size_t>(Cout) * Cin * kH * kW;
  const std::size_t out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

  cuda::nn::DeviceBufferF32 d_in(in_n);
  cuda::nn::DeviceBufferF32 d_w(w_n);
  cuda::nn::DeviceBufferF32 d_out(out_n);
  d_in.Upload(h_in);
  d_w.Upload(h_w);

  cuda::nn::DeviceBufferF32 d_b;
  if (h_b != nullptr) {
    d_b = cuda::nn::DeviceBufferF32(static_cast<std::size_t>(Cout));
    d_b.Upload(*h_b);
  }

  auto tin  = d_in.AsTensor({N, Cin, H, W});
  auto tout = d_out.AsTensor({N, Cout, Ho, Wo});

  cuda::nn::Conv2dParams p;
  p.in_channels  = Cin;
  p.out_channels = Cout;
  p.kH           = kH;
  p.kW           = kW;
  p.sH           = sH;
  p.sW           = sW;
  p.padH         = padH;
  p.padW         = padW;
  p.dilation     = 1;
  p.groups       = 1;
  p.weight       = d_w.get();
  p.bias         = h_b != nullptr ? d_b.get() : nullptr;

  if (relu) {
    cuda::nn::Conv2dBiasRelu(tin, tout, p, nullptr, ws);
  } else {
    cuda::nn::Conv2d(tin, tout, p, nullptr, ws);
  }
  EXPECT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  return d_out.Download();
}

}  // namespace

class MlOpsConv2dTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsConv2dTest, OutputSizeFormula) {
  // PyTorch: floor((H + 2p - d*(k-1) - 1)/s) + 1
  EXPECT_EQ(cuda::nn::Conv2dOutputSize(32, 0, 1, 3, 1), 30);
  EXPECT_EQ(cuda::nn::Conv2dOutputSize(32, 0, 1, 2, 2), 16);
  EXPECT_EQ(cuda::nn::Conv2dOutputSize(32, 0, 1, 1, 1), 32);
  EXPECT_EQ(cuda::nn::Conv2dOutputSize(7, 0, 1, 3, 1), 5);
  EXPECT_EQ(cuda::nn::Conv2dOutputSize(8, 1, 1, 3, 1), 8);  // same pad
}

TEST_F(MlOpsConv2dTest, OneByOneMatchesCpu) {
  constexpr int N = 1, Cin = 8, Cout = 12, H = 9, W = 11;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 1);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 1 * 1), 2);
  const auto    hb  = MakePattern(static_cast<std::size_t>(Cout), 3);

  const auto expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, 1, false);
  const auto actual   = RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, false, nullptr);
  // 1×1 accumulation over Cin=8 — tight tol
  ExpectVectorsNear(actual, expected, 1e-5f);
}

TEST_F(MlOpsConv2dTest, ThreeByThreeValidMatchesCpu) {
  constexpr int N = 1, Cin = 4, Cout = 6, H = 13, W = 15;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 10);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 11);
  const auto    hb  = MakePattern(static_cast<std::size_t>(Cout), 12);

  const auto expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, false);
  const auto actual = RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConv2dTest, TwoByTwoStride2PackMatchesCpu) {
  // pack_mosaick shape class: 3→4, k=2, s=2
  constexpr int N = 1, Cin = 3, Cout = 4, H = 16, W = 18;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 20);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 4), 21);
  const auto    hb  = MakePattern(static_cast<std::size_t>(Cout), 22);

  const auto expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1, false);
  const auto actual = RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConv2dTest, MultiBatchOddSpatial) {
  constexpr int N = 2, Cin = 5, Cout = 7, H = 11, W = 9;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 30);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 31);

  const auto expected =
      CpuConv2d(hin, hw, nullptr, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, false);
  const auto actual =
      RunGpuConv(hin, hw, nullptr, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConv2dTest, FusedBiasReluMatchesUnfused) {
  constexpr int N = 1, Cin = 16, Cout = 32, H = 24, W = 24;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 40);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 41);
  const auto    hb  = MakePattern(static_cast<std::size_t>(Cout), 42);

  // Unfused: Conv2d (+bias) then Relu
  const int Ho = cuda::nn::Conv2dOutputSize(H, 0, 1, 3, 1);
  const int Wo = cuda::nn::Conv2dOutputSize(W, 0, 1, 3, 1);
  const std::size_t in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t w_n   = static_cast<std::size_t>(Cout) * Cin * 9;
  const std::size_t out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

  cuda::nn::DeviceBufferF32 d_in(in_n);
  cuda::nn::DeviceBufferF32 d_w(w_n);
  cuda::nn::DeviceBufferF32 d_b(static_cast<std::size_t>(Cout));
  cuda::nn::DeviceBufferF32 d_mid(out_n);
  cuda::nn::DeviceBufferF32 d_unfused(out_n);
  cuda::nn::DeviceBufferF32 d_fused(out_n);
  d_in.Upload(hin);
  d_w.Upload(hw);
  d_b.Upload(hb);

  auto tin     = d_in.AsTensor({N, Cin, H, W});
  auto tmid    = d_mid.AsTensor({N, Cout, Ho, Wo});
  auto tunf    = d_unfused.AsTensor({N, Cout, Ho, Wo});
  auto tfused  = d_fused.AsTensor({N, Cout, Ho, Wo});

  cuda::nn::Conv2dParams p;
  p.in_channels  = Cin;
  p.out_channels = Cout;
  p.kH = p.kW = 3;
  p.sH = p.sW = 1;
  p.padH = p.padW = 0;
  p.weight        = d_w.get();
  p.bias          = d_b.get();

  cuda::nn::Conv2d(tin, tmid, p);
  // Copy mid → unfused buffer then ReLU inplace on unfused
  ASSERT_EQ(::cudaMemcpy(d_unfused.get(), d_mid.get(), out_n * sizeof(float),
                         cudaMemcpyDeviceToDevice),
            cudaSuccess);
  cuda::nn::ReluInplace(tunf);
  cuda::nn::Conv2dBiasRelu(tin, tfused, p);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  // Fused vs unfused: near-equality (same math order within kernel may differ slightly
  // from separate launches only if kernels differ — here both use same 3×3 path).
  ExpectVectorsNear(d_fused.Download(), d_unfused.Download(), 1e-6f);

  // Also vs CPU
  const auto expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
  ExpectVectorsNear(d_fused.Download(), expected, 5e-5f);
}

TEST_F(MlOpsConv2dTest, WorkspacePathNoCrash) {
  constexpr int N = 1, Cin = 3, Cout = 8, H = 20, W = 20;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 50);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 51);
  const auto    hb  = MakePattern(static_cast<std::size_t>(Cout), 52);

  cuda::nn::WorkspacePool pool(1 << 20);
  const auto expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, false);
  const auto actual = RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, false, &pool);
  ExpectVectorsNear(actual, expected, 2e-5f);
  // Direct path must not grow the pool (no scratch alloc).
  EXPECT_EQ(pool.used_bytes(), 0u);
}

TEST_F(MlOpsConv2dTest, RealWeightPackMosaick) {
  const std::string path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  cuda::nn::SafetensorsTensorMap map;
  try {
    map = cuda::nn::LoadSafetensors(path);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "failed to load pack_mosaick: " << e.what();
  }
  const auto& w = cuda::nn::RequireF32Tensor(map, "pack_mosaick.weight", {4, 3, 2, 2});
  const auto& b = cuda::nn::RequireF32Tensor(map, "pack_mosaick.bias", {4});

  constexpr int N = 1, Cin = 3, Cout = 4, H = 32, W = 32;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 60);
  const auto expected =
      CpuConv2d(hin, w.data, &b.data, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1, false);
  const auto actual =
      RunGpuConv(hin, w.data, &b.data, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 5e-5f);
}

TEST_F(MlOpsConv2dTest, RealWeightConv1AndOutput1x1) {
  const std::string path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  cuda::nn::SafetensorsTensorMap map;
  try {
    map = cuda::nn::LoadSafetensors(path);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "failed to load bayer weights: " << e.what();
  }
  const auto& conv1_w = cuda::nn::RequireF32Tensor(map, "conv1.weight", {64, 4, 3, 3});
  const auto& conv1_b = cuda::nn::RequireF32Tensor(map, "conv1.bias", {64});
  const auto& out_w   = cuda::nn::RequireF32Tensor(map, "output.weight", {3, 64, 1, 1});
  const auto& out_b   = cuda::nn::RequireF32Tensor(map, "output.bias", {3});

  {
    constexpr int N = 1, Cin = 4, Cout = 64, H = 20, W = 20;
    const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 70);
    const auto expected =
        CpuConv2d(hin, conv1_w.data, &conv1_b.data, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
    const auto actual = RunGpuConv(hin, conv1_w.data, &conv1_b.data, N, Cin, Cout, H, W, 3, 3, 1, 1,
                                   0, 0, true, nullptr);
    ExpectVectorsNear(actual, expected, 1e-4f);
  }

  {
    constexpr int N = 1, Cin = 64, Cout = 3, H = 17, W = 19;
    const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 71);
    const auto expected =
        CpuConv2d(hin, out_w.data, &out_b.data, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, 1, false);
    const auto actual = RunGpuConv(hin, out_w.data, &out_b.data, N, Cin, Cout, H, W, 1, 1, 1, 1, 0,
                                   0, false, nullptr);
    ExpectVectorsNear(actual, expected, 1e-4f);
  }
}

TEST_F(MlOpsConv2dTest, RealWeightPostConv1BayerAndXtrans) {
  // post_conv1 Bayer: 6→64 k=3; XTrans: 67→64 k=3
  {
    const std::string path = FindModelPath("bayer.safetensors");
    if (path.empty()) {
      GTEST_SKIP() << "bayer.safetensors not found";
    }
    cuda::nn::SafetensorsTensorMap map;
    try {
      map = cuda::nn::LoadSafetensors(path);
    } catch (const std::exception& e) {
      GTEST_SKIP() << e.what();
    }
    const auto& w = cuda::nn::RequireF32Tensor(map, "post_conv1.weight", {64, 6, 3, 3});
    const auto& b = cuda::nn::RequireF32Tensor(map, "post_conv1.bias", {64});
    constexpr int N = 1, Cin = 6, Cout = 64, H = 18, W = 18;
    const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 80);
    const auto expected =
        CpuConv2d(hin, w.data, &b.data, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
    const auto actual =
        RunGpuConv(hin, w.data, &b.data, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, true, nullptr);
    ExpectVectorsNear(actual, expected, 1e-4f);
  }
  {
    const std::string path = FindModelPath("xtrans.safetensors");
    if (path.empty()) {
      GTEST_SKIP() << "xtrans.safetensors not found";
    }
    cuda::nn::SafetensorsTensorMap map;
    try {
      map = cuda::nn::LoadSafetensors(path);
    } catch (const std::exception& e) {
      GTEST_SKIP() << e.what();
    }
    const auto& w = cuda::nn::RequireF32Tensor(map, "post_conv1.weight", {64, 67, 3, 3});
    const auto& b = cuda::nn::RequireF32Tensor(map, "post_conv1.bias", {64});
    constexpr int N = 1, Cin = 67, Cout = 64, H = 16, W = 16;
    const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 81);
    const auto expected =
        CpuConv2d(hin, w.data, &b.data, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
    const auto actual =
        RunGpuConv(hin, w.data, &b.data, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, true, nullptr);
    ExpectVectorsNear(actual, expected, 1e-4f);
  }
}

TEST_F(MlOpsConv2dTest, PerfThreeByThreeC64_512) {
  // Soft performance floor: fail only on catastrophic regressions.
  constexpr int N = 1, Cin = 64, Cout = 64, H = 512, W = 512;
  const int     Ho = cuda::nn::Conv2dOutputSize(H, 0, 1, 3, 1);
  const int     Wo = cuda::nn::Conv2dOutputSize(W, 0, 1, 3, 1);
  const std::size_t in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t w_n   = static_cast<std::size_t>(Cout) * Cin * 9;
  const std::size_t out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

  const auto hin = MakePattern(in_n, 90);
  const auto hw  = MakePattern(w_n, 91);
  const auto hb  = MakePattern(static_cast<std::size_t>(Cout), 92);

  cuda::nn::DeviceBufferF32 d_in(in_n);
  cuda::nn::DeviceBufferF32 d_w(w_n);
  cuda::nn::DeviceBufferF32 d_b(static_cast<std::size_t>(Cout));
  cuda::nn::DeviceBufferF32 d_out(out_n);
  d_in.Upload(hin);
  d_w.Upload(hw);
  d_b.Upload(hb);

  auto tin  = d_in.AsTensor({N, Cin, H, W});
  auto tout = d_out.AsTensor({N, Cout, Ho, Wo});

  cuda::nn::Conv2dParams p;
  p.in_channels  = Cin;
  p.out_channels = Cout;
  p.kH = p.kW = 3;
  p.sH = p.sW = 1;
  p.weight    = d_w.get();
  p.bias      = d_b.get();

  // Warmup
  for (int i = 0; i < 3; ++i) {
    cuda::nn::Conv2dBiasRelu(tin, tout, p);
  }
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start{};
  cudaEvent_t stop{};
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);

  constexpr int kIters = 10;
  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::Conv2dBiasRelu(tin, tout, p);
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float ms_total = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&ms_total, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  const double ms_per = static_cast<double>(ms_total) / kIters;
  // FLOPs ≈ 2 * N * Cout * Ho * Wo * Cin * kH * kW
  const double flops = 2.0 * N * Cout * Ho * Wo * Cin * 3.0 * 3.0;
  const double gflops = (flops / (ms_per * 1e-3)) / 1e9;

  std::cout << "[Conv2d perf] 3x3 C=64 H=W=512: " << ms_per << " ms/iter, " << gflops
            << " GFLOP/s\n";

  // Soft floor for the multi-Cout tiled 3×3 path (Phase 8).
  // Pre-overhaul (one Cout/block, weights-only SMEM) was tens of GFLOP/s and
  // ~0.4 s per 64→64 layer at ~1024 spatial. Release tiled path targets TFLOP/s.
  // Keep floors loose enough for CI / laptop GPUs, tight enough to catch the
  // old naive path or a host-side serialization bug.
  EXPECT_GT(gflops, 50.0);
  EXPECT_LT(ms_per, 200.0);
}

// Matches the X-Trans / Bayer tile spatial scale used in Phase 6c profiling.
TEST_F(MlOpsConv2dTest, PerfThreeByThreeC64_1024) {
  constexpr int N = 1, Cin = 64, Cout = 64, H = 1024, W = 1024;
  const int     Ho = cuda::nn::Conv2dOutputSize(H, 0, 1, 3, 1);
  const int     Wo = cuda::nn::Conv2dOutputSize(W, 0, 1, 3, 1);
  const std::size_t in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t w_n   = static_cast<std::size_t>(Cout) * Cin * 9;
  const std::size_t out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

  const auto hin = MakePattern(in_n, 93);
  const auto hw  = MakePattern(w_n, 94);
  const auto hb  = MakePattern(static_cast<std::size_t>(Cout), 95);

  cuda::nn::DeviceBufferF32 d_in(in_n);
  cuda::nn::DeviceBufferF32 d_w(w_n);
  cuda::nn::DeviceBufferF32 d_b(static_cast<std::size_t>(Cout));
  cuda::nn::DeviceBufferF32 d_out(out_n);
  d_in.Upload(hin);
  d_w.Upload(hw);
  d_b.Upload(hb);

  auto tin  = d_in.AsTensor({N, Cin, H, W});
  auto tout = d_out.AsTensor({N, Cout, Ho, Wo});

  cuda::nn::Conv2dParams p;
  p.in_channels  = Cin;
  p.out_channels = Cout;
  p.kH = p.kW = 3;
  p.sH = p.sW = 1;
  p.weight    = d_w.get();
  p.bias      = d_b.get();

  for (int i = 0; i < 2; ++i) {
    cuda::nn::Conv2dBiasRelu(tin, tout, p);
  }
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start{};
  cudaEvent_t stop{};
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);

  constexpr int kIters = 5;
  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::Conv2dBiasRelu(tin, tout, p);
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float ms_total = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&ms_total, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  const double ms_per = static_cast<double>(ms_total) / kIters;
  const double flops  = 2.0 * N * Cout * Ho * Wo * Cin * 3.0 * 3.0;
  const double gflops = (flops / (ms_per * 1e-3)) / 1e9;

  std::cout << "[Conv2d perf] 3x3 C=64 H=W=1024: " << ms_per << " ms/iter, " << gflops
            << " GFLOP/s\n";

  // Pre-overhaul baseline at this scale was ~400–470 ms/layer.
  EXPECT_GT(gflops, 50.0);
  EXPECT_LT(ms_per, 400.0);
}

}  // namespace alcedo
