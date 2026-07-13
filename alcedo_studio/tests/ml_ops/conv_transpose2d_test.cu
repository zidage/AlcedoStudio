//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/conv_transpose2d.hpp"
#include "cuda/nn/device_buffer.hpp"
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

// Naive NCHW grouped ConvTranspose2d, weight [Cin, Cout/groups, kH, kW] — CPU ref.
auto CpuConvTranspose2d(const std::vector<float>& input, const std::vector<float>& weight,
                        const std::vector<float>* bias, int N, int Cin, int Cout, int H, int W,
                        int kH, int kW, int sH, int sW, int padH, int padW, int dil, int groups,
                        int out_padH, int out_padW) -> std::vector<float> {
  const int Ho = cuda::nn::ConvTranspose2dOutputSize(H, padH, dil, kH, sH, out_padH);
  const int Wo = cuda::nn::ConvTranspose2dOutputSize(W, padW, dil, kW, sW, out_padW);
  EXPECT_GT(Ho, 0);
  EXPECT_GT(Wo, 0);
  EXPECT_EQ(Cin % groups, 0);
  EXPECT_EQ(Cout % groups, 0);

  const int cin_per_group  = Cin / groups;
  const int cout_per_group = Cout / groups;

  std::vector<float> out(static_cast<std::size_t>(N) * Cout * Ho * Wo, 0.0f);

  auto in_at = [&](int n, int c, int h, int w) -> float {
    const std::size_t idx = static_cast<std::size_t>(n) * Cin * H * W +
                            static_cast<std::size_t>(c) * H * W +
                            static_cast<std::size_t>(h) * W + static_cast<std::size_t>(w);
    return input[idx];
  };

  auto w_at = [&](int ci, int co_g, int kh, int kw) -> float {
    // [Cin, Cout/g, kH, kW]
    const std::size_t idx = static_cast<std::size_t>(ci) * cout_per_group * kH * kW +
                            static_cast<std::size_t>(co_g) * kH * kW +
                            static_cast<std::size_t>(kh) * kW + static_cast<std::size_t>(kw);
    return weight[idx];
  };

  for (int n = 0; n < N; ++n) {
    for (int co = 0; co < Cout; ++co) {
      const int g    = co / cout_per_group;
      const int co_g = co % cout_per_group;
      for (int oh = 0; oh < Ho; ++oh) {
        for (int ow = 0; ow < Wo; ++ow) {
          float acc = 0.0f;
          for (int cin_g = 0; cin_g < cin_per_group; ++cin_g) {
            const int ci = g * cin_per_group + cin_g;
            for (int kh = 0; kh < kH; ++kh) {
              const int num_h = oh + padH - kh * dil;
              if (num_h < 0 || (num_h % sH) != 0) {
                continue;
              }
              const int ih = num_h / sH;
              if (ih >= H) {
                continue;
              }
              for (int kw = 0; kw < kW; ++kw) {
                const int num_w = ow + padW - kw * dil;
                if (num_w < 0 || (num_w % sW) != 0) {
                  continue;
                }
                const int iw = num_w / sW;
                if (iw >= W) {
                  continue;
                }
                acc += in_at(n, ci, ih, iw) * w_at(ci, co_g, kh, kw);
              }
            }
          }
          if (bias != nullptr) {
            acc += (*bias)[static_cast<std::size_t>(co)];
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
    std::string   path = std::string(prefix) + filename;
    std::ifstream f(path, std::ios::binary);
    if (f) {
      return path;
    }
  }
  const char* extra[] = {
      "D:/Projects/pu-erh_lab/alcedo_studio/src/config/models/",
  };
  for (const char* prefix : extra) {
    std::string   path = std::string(prefix) + filename;
    std::ifstream f(path, std::ios::binary);
    if (f) {
      return path;
    }
  }
  return {};
}

auto RunGpuConvTranspose(const std::vector<float>& h_in, const std::vector<float>& h_w,
                         const std::vector<float>* h_b, int N, int Cin, int Cout, int H, int W,
                         int kH, int kW, int sH, int sW, int padH, int padW, int dil, int groups,
                         int out_padH, int out_padW, cuda::nn::WorkspacePool* ws)
    -> std::vector<float> {
  const int Ho = cuda::nn::ConvTranspose2dOutputSize(H, padH, dil, kH, sH, out_padH);
  const int Wo = cuda::nn::ConvTranspose2dOutputSize(W, padW, dil, kW, sW, out_padW);
  const std::size_t in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t w_n =
      static_cast<std::size_t>(Cin) * (Cout / groups) * static_cast<std::size_t>(kH) * kW;
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

  cuda::nn::ConvTranspose2dParams p;
  p.in_channels  = Cin;
  p.out_channels = Cout;
  p.kH           = kH;
  p.kW           = kW;
  p.sH           = sH;
  p.sW           = sW;
  p.padH         = padH;
  p.padW         = padW;
  p.output_padH  = out_padH;
  p.output_padW  = out_padW;
  p.dilation     = dil;
  p.groups       = groups;
  p.weight       = d_w.get();
  p.bias         = h_b != nullptr ? d_b.get() : nullptr;

  cuda::nn::ConvTranspose2d(tin, tout, p, nullptr, ws);
  EXPECT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  return d_out.Download();
}

}  // namespace

class MlOpsConvTranspose2dTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsConvTranspose2dTest, OutputSizeFormula) {
  // k=2, s=2, pad=0, dil=1, out_pad=0 → 2× upsample
  EXPECT_EQ(cuda::nn::ConvTranspose2dOutputSize(8, 0, 1, 2, 2, 0), 16);
  EXPECT_EQ(cuda::nn::ConvTranspose2dOutputSize(1, 0, 1, 2, 2, 0), 2);
  EXPECT_EQ(cuda::nn::ConvTranspose2dOutputSize(5, 0, 1, 2, 2, 0), 10);
  // k=3, s=1, pad=0 → (H-1) + 2 + 1 = H+2
  EXPECT_EQ(cuda::nn::ConvTranspose2dOutputSize(10, 0, 1, 3, 1, 0), 12);
  // invalid: output_padding >= stride
  EXPECT_EQ(cuda::nn::ConvTranspose2dOutputSize(8, 0, 1, 2, 2, 2), -1);
}

TEST_F(MlOpsConvTranspose2dTest, GroupedTinyMatchesCpu) {
  // Small grouped transpose (not the unpack specialization shape).
  constexpr int N = 1, Cin = 4, Cout = 2, H = 3, W = 4;
  constexpr int groups = 2;  // 2 in / 1 out per group
  constexpr int kH = 2, kW = 2, sH = 2, sW = 2;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 101);
  const auto    hw =
      MakePattern(static_cast<std::size_t>(Cin) * (Cout / groups) * kH * kW, 102);
  const auto hb = MakePattern(static_cast<std::size_t>(Cout), 103);

  const auto expected = CpuConvTranspose2d(hin, hw, &hb, N, Cin, Cout, H, W, kH, kW, sH, sW, 0, 0,
                                           1, groups, 0, 0);
  const auto actual =
      RunGpuConvTranspose(hin, hw, &hb, N, Cin, Cout, H, W, kH, kW, sH, sW, 0, 0, 1, groups, 0, 0,
                          nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConvTranspose2dTest, Groups1MatchesCpu) {
  constexpr int N = 2, Cin = 3, Cout = 5, H = 5, W = 6;
  constexpr int kH = 3, kW = 3, sH = 1, sW = 1;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 110);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cin * Cout * kH * kW), 111);

  const auto expected =
      CpuConvTranspose2d(hin, hw, nullptr, N, Cin, Cout, H, W, kH, kW, sH, sW, 0, 0, 1, 1, 0, 0);
  const auto actual = RunGpuConvTranspose(hin, hw, nullptr, N, Cin, Cout, H, W, kH, kW, sH, sW, 0,
                                          0, 1, 1, 0, 0, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConvTranspose2dTest, UnpackMosaickShapeMatchesCpu) {
  // Specialized path: 12→3, k=2, s=2, groups=3
  constexpr int N = 1, Cin = 12, Cout = 3, H = 7, W = 9;
  constexpr int groups = 3;
  constexpr int kH = 2, kW = 2, sH = 2, sW = 2;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 120);
  const auto    hw =
      MakePattern(static_cast<std::size_t>(Cin) * (Cout / groups) * kH * kW, 121);  // [12,1,2,2]
  const auto hb = MakePattern(static_cast<std::size_t>(Cout), 122);

  const auto expected = CpuConvTranspose2d(hin, hw, &hb, N, Cin, Cout, H, W, kH, kW, sH, sW, 0, 0,
                                           1, groups, 0, 0);
  const auto actual =
      RunGpuConvTranspose(hin, hw, &hb, N, Cin, Cout, H, W, kH, kW, sH, sW, 0, 0, 1, groups, 0, 0,
                          nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);

  // Output spatial must be 2×
  EXPECT_EQ(cuda::nn::ConvTranspose2dOutputSize(H, 0, 1, 2, 2, 0), 2 * H);
  EXPECT_EQ(cuda::nn::ConvTranspose2dOutputSize(W, 0, 1, 2, 2, 0), 2 * W);
}

TEST_F(MlOpsConvTranspose2dTest, MultiBatchOddSpatial) {
  constexpr int N = 2, Cin = 12, Cout = 3, H = 5, W = 6;
  constexpr int groups = 3;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 130);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cin) * 1 * 4, 131);
  const auto    hb  = MakePattern(static_cast<std::size_t>(Cout), 132);

  const auto expected =
      CpuConvTranspose2d(hin, hw, &hb, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1, groups, 0, 0);
  const auto actual = RunGpuConvTranspose(hin, hw, &hb, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1,
                                          groups, 0, 0, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConvTranspose2dTest, WorkspacePathNoAlloc) {
  constexpr int N = 1, Cin = 12, Cout = 3, H = 16, W = 16;
  constexpr int groups = 3;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 140);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cin) * 4, 141);
  const auto    hb  = MakePattern(static_cast<std::size_t>(Cout), 142);

  cuda::nn::WorkspacePool pool(1 << 20);
  const auto expected =
      CpuConvTranspose2d(hin, hw, &hb, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1, groups, 0, 0);
  const auto actual = RunGpuConvTranspose(hin, hw, &hb, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1,
                                          groups, 0, 0, &pool);
  ExpectVectorsNear(actual, expected, 2e-5f);
  EXPECT_EQ(pool.used_bytes(), 0u);
}

TEST_F(MlOpsConvTranspose2dTest, RealWeightUnpackMosaick) {
  const std::string path = FindModelPath("bayer.safetensors");
  if (path.empty()) {
    GTEST_SKIP() << "bayer.safetensors not found";
  }
  cuda::nn::SafetensorsTensorMap map;
  try {
    map = cuda::nn::LoadSafetensors(path);
  } catch (const std::exception& e) {
    GTEST_SKIP() << "failed to load unpack.weight: " << e.what();
  }
  // Student unpack is bias-free fixed one-hot: [Cin, Cout/groups, kH, kW] = [12, 1, 2, 2]
  const auto& w = cuda::nn::RequireF32Tensor(map, "unpack.weight", {12, 1, 2, 2});

  constexpr int N = 1, Cin = 12, Cout = 3, H = 16, W = 20;
  constexpr int groups = 3;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 150);

  const auto expected =
      CpuConvTranspose2d(hin, w.data, nullptr, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1, groups, 0,
                         0);
  const auto actual = RunGpuConvTranspose(hin, w.data, nullptr, N, Cin, Cout, H, W, 2, 2, 2, 2, 0,
                                          0, 1, groups, 0, 0, nullptr);
  ExpectVectorsNear(actual, expected, 5e-5f);
}

TEST_F(MlOpsConvTranspose2dTest, NoBiasMatchesCpu) {
  constexpr int N = 1, Cin = 12, Cout = 3, H = 4, W = 5;
  constexpr int groups = 3;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 160);
  const auto    hw  = MakePattern(static_cast<std::size_t>(Cin) * 4, 161);

  const auto expected =
      CpuConvTranspose2d(hin, hw, nullptr, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1, groups, 0, 0);
  const auto actual = RunGpuConvTranspose(hin, hw, nullptr, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1,
                                          groups, 0, 0, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConvTranspose2dTest, PerfUnpackMosaick_512) {
  // Soft performance floor for specialized unpack_mosaick (12→3, k=2, s=2, groups=3).
  // Input residual map 512² → RGB upsample 1024². Fail only on catastrophic regressions.
  constexpr int N = 1, Cin = 12, Cout = 3, H = 512, W = 512;
  constexpr int groups = 3;
  const int     Ho = cuda::nn::ConvTranspose2dOutputSize(H, 0, 1, 2, 2, 0);
  const int     Wo = cuda::nn::ConvTranspose2dOutputSize(W, 0, 1, 2, 2, 0);
  ASSERT_EQ(Ho, 1024);
  ASSERT_EQ(Wo, 1024);

  const std::size_t in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t w_n   = static_cast<std::size_t>(Cin) * (Cout / groups) * 2 * 2;  // [12,1,2,2]
  const std::size_t out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

  const auto hin = MakePattern(in_n, 170);
  const auto hw  = MakePattern(w_n, 171);
  const auto hb  = MakePattern(static_cast<std::size_t>(Cout), 172);

  cuda::nn::DeviceBufferF32 d_in(in_n);
  cuda::nn::DeviceBufferF32 d_w(w_n);
  cuda::nn::DeviceBufferF32 d_b(static_cast<std::size_t>(Cout));
  cuda::nn::DeviceBufferF32 d_out(out_n);
  d_in.Upload(hin);
  d_w.Upload(hw);
  d_b.Upload(hb);

  auto tin  = d_in.AsTensor({N, Cin, H, W});
  auto tout = d_out.AsTensor({N, Cout, Ho, Wo});

  cuda::nn::ConvTranspose2dParams p;
  p.in_channels  = Cin;
  p.out_channels = Cout;
  p.kH = p.kW = 2;
  p.sH = p.sW = 2;
  p.padH = p.padW = 0;
  p.output_padH = p.output_padW = 0;
  p.dilation                    = 1;
  p.groups                      = groups;
  p.weight                      = d_w.get();
  p.bias                        = d_b.get();

  // Warmup
  for (int i = 0; i < 3; ++i) {
    cuda::nn::ConvTranspose2d(tin, tout, p);
  }
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start{};
  cudaEvent_t stop{};
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);

  constexpr int kIters = 20;
  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::ConvTranspose2d(tin, tout, p);
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float ms_total = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&ms_total, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  const double ms_per = static_cast<double>(ms_total) / kIters;
  // For s=k=2 pad=0 each output gathers cin_per_group (=4) MACs → 2*4 FLOPs each.
  const int    cin_g  = Cin / groups;
  const double flops  = 2.0 * N * Cout * Ho * Wo * cin_g;
  const double gflops = (flops / (ms_per * 1e-3)) / 1e9;
  // Traffic: read input + write output (weights reused / tiny).
  const double bytes =
      static_cast<double>(in_n + out_n) * sizeof(float);
  const double gbs = (bytes / (ms_per * 1e-3)) / 1e9;

  std::cout << "[ConvTranspose2d perf] unpack_mosaick 12→3 H=W=512→1024: " << ms_per
            << " ms/iter, " << gflops << " GFLOP/s, " << gbs << " GB/s\n";

  // Soft floors: only catch host-side serialization or completely broken launches.
  EXPECT_GT(gflops, 0.5);
  EXPECT_GT(gbs, 1.0);
  EXPECT_LT(ms_per, 1000.0);  // 1s/iter on 1024² RGB would mean something is very wrong
}

}  // namespace alcedo
