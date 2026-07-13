//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>
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

#include "cuda/nn/conv2d.hpp"
#include "cuda/nn/cutlass_conv2d.hpp"
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
               int kW, int sH, int sW, int padH, int padW, int dil, bool relu)
    -> std::vector<float> {
  const int Ho = cuda::nn::Conv2dOutputSize(H, padH, dil, kH, sH);
  const int Wo = cuda::nn::Conv2dOutputSize(W, padW, dil, kW, sW);
  EXPECT_GT(Ho, 0);
  EXPECT_GT(Wo, 0);
  std::vector<float> out(static_cast<std::size_t>(N) * Cout * Ho * Wo, 0.0f);

  auto               in_at = [&](int n, int c, int h, int w) -> float {
    if (h < 0 || h >= H || w < 0 || w >= W) {
      return 0.0f;
    }
    const std::size_t idx = static_cast<std::size_t>(n) * Cin * H * W +
                            static_cast<std::size_t>(c) * H * W + static_cast<std::size_t>(h) * W +
                            static_cast<std::size_t>(w);
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
                const int         ih = oh * sH - padH + kh * dil;
                const int         iw = ow * sW - padW + kw * dil;
                const std::size_t wi = static_cast<std::size_t>(co) * Cin * kH * kW +
                                       static_cast<std::size_t>(ci) * kH * kW +
                                       static_cast<std::size_t>(kh) * kW +
                                       static_cast<std::size_t>(kw);
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
    std::string   path = std::string(prefix) + filename;
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
    std::string   path = std::string(prefix) + filename;
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
  const int                 Ho    = cuda::nn::Conv2dOutputSize(H, padH, 1, kH, sH);
  const int                 Wo    = cuda::nn::Conv2dOutputSize(W, padW, 1, kW, sW);
  const std::size_t         in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t         w_n   = static_cast<std::size_t>(Cout) * Cin * kH * kW;
  const std::size_t         out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

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

auto NchwToNhwc(const std::vector<float>& nchw, const int N, const int C, const int H, const int W)
    -> std::vector<float> {
  std::vector<float> nhwc(nchw.size());
  for (int n = 0; n < N; ++n) {
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        for (int c = 0; c < C; ++c) {
          nhwc[((static_cast<std::size_t>(n) * H + y) * W + x) * C + c] =
              nchw[((static_cast<std::size_t>(n) * C + c) * H + y) * W + x];
        }
      }
    }
  }
  return nhwc;
}

auto NhwcToNchw(const std::vector<float>& nhwc, const int N, const int C, const int H, const int W)
    -> std::vector<float> {
  std::vector<float> nchw(nhwc.size());
  for (int n = 0; n < N; ++n) {
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        for (int c = 0; c < C; ++c) {
          nchw[((static_cast<std::size_t>(n) * C + c) * H + y) * W + x] =
              nhwc[((static_cast<std::size_t>(n) * H + y) * W + x) * C + c];
        }
      }
    }
  }
  return nchw;
}

auto RunGpuNhwcTrunk(const std::vector<float>& h_in_nchw, const std::vector<float>& h_w,
                     const std::vector<float>& h_b, const int channels, const int H, const int W)
    -> std::vector<float> {
  const auto         h_in_nhwc = NchwToNhwc(h_in_nchw, 1, channels, H, W);
  std::vector<float> h_w_ckco(static_cast<std::size_t>(channels) * channels * 9);
  cuda::nn::TransformConv2d3x3WeightsNhwc(h_w.data(), channels, channels, h_w_ckco.data());

  cuda::nn::DeviceBufferF32 d_in(h_in_nhwc.size());
  cuda::nn::DeviceBufferF32 d_w(h_w_ckco.size());
  cuda::nn::DeviceBufferF32 d_b(h_b.size());
  cuda::nn::DeviceBufferF32 d_out(static_cast<std::size_t>(channels) * (H - 2) * (W - 2));
  d_in.Upload(h_in_nhwc);
  d_w.Upload(h_w_ckco);
  d_b.Upload(h_b);
  cuda::nn::Conv2d3x3NhwcBiasRelu(d_in.data(), d_out.data(), d_w.data(), d_b.data(), 1, H, W,
                                  channels);
  EXPECT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  return NhwcToNchw(d_out.Download(), 1, channels, H - 2, W - 2);
}

auto RunGpuCutlassNhwcTrunk(const std::vector<float>& h_in_nchw, const std::vector<float>& h_w,
                            const std::vector<float>& h_b, const int channels, const int H,
                            const int W) -> std::vector<float> {
  const auto         h_in_nhwc = NchwToNhwc(h_in_nchw, 1, channels, H, W);
  std::vector<float> h_w_krsc(static_cast<std::size_t>(channels) * channels * 9);
  cuda::nn::TransformConv2d3x3WeightsCutlassKrsc(h_w.data(), channels, h_w_krsc.data());

  cuda::nn::DeviceBufferF32 d_in(h_in_nhwc.size());
  cuda::nn::DeviceBufferF32 d_w(h_w_krsc.size());
  cuda::nn::DeviceBufferF32 d_b(h_b.size());
  cuda::nn::DeviceBufferF32 d_out(static_cast<std::size_t>(channels) * (H - 2) * (W - 2));
  d_in.Upload(h_in_nhwc);
  d_w.Upload(h_w_krsc);
  d_b.Upload(h_b);
  cuda::nn::Conv2d3x3NhwcCutlassBiasRelu(d_in.data(), d_out.data(), d_w.data(), d_b.data(), 1, H, W,
                                         channels);
  EXPECT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  return NhwcToNchw(d_out.Download(), 1, channels, H - 2, W - 2);
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
  const auto    hin      = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 1);
  const auto    hw       = MakePattern(static_cast<std::size_t>(Cout * Cin * 1 * 1), 2);
  const auto    hb       = MakePattern(static_cast<std::size_t>(Cout), 3);

  const auto    expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, 1, false);
  const auto    actual =
      RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, false, nullptr);
  // 1×1 accumulation over Cin=8 — tight tol
  ExpectVectorsNear(actual, expected, 1e-5f);
}

TEST_F(MlOpsConv2dTest, ChannelsLastBayerC24TrunkMatchesNchwReference) {
  constexpr int H        = 19;
  constexpr int W        = 23;
  constexpr int channels = 24;
  const auto    hin      = MakePattern(static_cast<std::size_t>(channels) * H * W, 924);
  const auto    hw       = MakePattern(static_cast<std::size_t>(channels) * channels * 9, 1024);
  const auto    hb       = MakePattern(static_cast<std::size_t>(channels), 1124);
  const auto    expected =
      CpuConv2d(hin, hw, &hb, 1, channels, channels, H, W, 3, 3, 1, 1, 0, 0, 1, true);
  const auto actual = RunGpuNhwcTrunk(hin, hw, hb, channels, H, W);
  ExpectVectorsNear(actual, expected, 2e-4F);
}

TEST_F(MlOpsConv2dTest, CutlassChannelsLastC32TrunkMatchesNchwReference) {
  constexpr int H = 19;
  constexpr int W = 23;
  constexpr int C = 32;
  const auto hin = MakePattern(static_cast<std::size_t>(C) * H * W, 1232);
  const auto hw  = MakePattern(static_cast<std::size_t>(C) * C * 9, 1332);
  const auto hb  = MakePattern(static_cast<std::size_t>(C), 1432);
  const auto expected = CpuConv2d(hin, hw, &hb, 1, C, C, H, W, 3, 3, 1, 1, 0, 0, 1, true);
  const auto actual   = RunGpuCutlassNhwcTrunk(hin, hw, hb, C, H, W);
  ExpectVectorsNear(actual, expected, 3e-4F);
}

TEST_F(MlOpsConv2dTest, StudentResidualOneByOneExactCout12MatchesCpu) {
  constexpr int N = 2, Cin = 24, Cout = 12, H = 13, W = 17;
  const auto    hin      = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 4);
  const auto    hw       = MakePattern(static_cast<std::size_t>(Cout * Cin), 5);
  const auto    hb       = MakePattern(static_cast<std::size_t>(Cout), 6);

  const auto    expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, 1, false);
  const auto    actual =
      RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 1e-4f);
}

TEST_F(MlOpsConv2dTest, StudentOutputOneByOneExactCout3MatchesCpu) {
  constexpr int N = 2, Cin = 32, Cout = 3, H = 15, W = 19;
  const auto    hin      = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 7);
  const auto    hw       = MakePattern(static_cast<std::size_t>(Cout * Cin), 8);
  const auto    hb       = MakePattern(static_cast<std::size_t>(Cout), 9);

  const auto    expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, 1, false);
  const auto    actual =
      RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 1e-4f);
}

TEST_F(MlOpsConv2dTest, ThreeByThreeValidMatchesCpu) {
  constexpr int N = 1, Cin = 4, Cout = 6, H = 13, W = 15;
  const auto    hin      = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 10);
  const auto    hw       = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 11);
  const auto    hb       = MakePattern(static_cast<std::size_t>(Cout), 12);

  const auto    expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, false);
  const auto    actual =
      RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConv2dTest, StudentTrunk24ProductDirectMatchesCpu) {
  constexpr int                 N = 1, Cin = 24, Cout = 24, H = 41, W = 37;
  const auto                    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 110);
  const auto                    hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 111);
  const auto                    hb  = MakePattern(static_cast<std::size_t>(Cout), 112);

  cuda::nn::Conv2d3x3KernelInfo info{};
  ASSERT_TRUE(cuda::nn::QueryConv2d3x3KernelInfo(Cin, Cout, &info));
  EXPECT_STREQ(info.name, "direct_tiled_24");
  EXPECT_GT(info.num_regs, 0);

  const auto expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
  const auto actual = RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, true, nullptr);
  ExpectVectorsNear(actual, expected, 2e-4f);
}

TEST_F(MlOpsConv2dTest, StudentTrunk32ProductDirectMatchesCpu) {
  constexpr int                 N = 1, Cin = 32, Cout = 32, H = 35, W = 33;
  const auto                    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 120);
  const auto                    hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 121);
  const auto                    hb  = MakePattern(static_cast<std::size_t>(Cout), 122);

  cuda::nn::Conv2d3x3KernelInfo info{};
  ASSERT_TRUE(cuda::nn::QueryConv2d3x3KernelInfo(Cin, Cout, &info));
  EXPECT_STREQ(info.name, "direct_tiled_32");
  EXPECT_GT(info.num_regs, 0);

  const auto expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
  const auto actual = RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, true, nullptr);
  ExpectVectorsNear(actual, expected, 3e-4f);
}

TEST_F(MlOpsConv2dTest, StudentTrunk24EvenSpatialProductDirectMatchesCpu) {
  constexpr int N = 1, Cin = 24, Cout = 24, H = 34, W = 34;
  const auto    hin      = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 140);
  const auto    hw       = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 141);
  const auto    hb       = MakePattern(static_cast<std::size_t>(Cout), 142);
  const auto    expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, false);
  const auto    actual =
      RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 2e-4f);
}

TEST_F(MlOpsConv2dTest, ThinCinPostConvKeepsDirectTiledFallback) {
  cuda::nn::Conv2d3x3KernelInfo bayer{};
  ASSERT_TRUE(cuda::nn::QueryConv2d3x3KernelInfo(6, 24, &bayer));
  EXPECT_STREQ(bayer.name, "direct_tiled_24");

  cuda::nn::Conv2d3x3KernelInfo xtrans{};
  ASSERT_TRUE(cuda::nn::QueryConv2d3x3KernelInfo(6, 32, &xtrans));
  EXPECT_STREQ(xtrans.name, "direct_tiled_32");

  constexpr int N = 1, Cin = 6, Cout = 24, H = 19, W = 17;
  const auto    hin      = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 130);
  const auto    hw       = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 131);
  const auto    hb       = MakePattern(static_cast<std::size_t>(Cout), 132);
  const auto    expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
  const auto actual = RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, true, nullptr);
  ExpectVectorsNear(actual, expected, 1e-4f);
}

TEST_F(MlOpsConv2dTest, TwoByTwoStride2PackMatchesCpu) {
  // pack_mosaick shape class: 3→4, k=2, s=2
  constexpr int N = 1, Cin = 3, Cout = 4, H = 16, W = 18;
  const auto    hin      = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 20);
  const auto    hw       = MakePattern(static_cast<std::size_t>(Cout * Cin * 4), 21);
  const auto    hb       = MakePattern(static_cast<std::size_t>(Cout), 22);

  const auto    expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1, false);
  const auto    actual =
      RunGpuConv(hin, hw, &hb, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConv2dTest, MultiBatchOddSpatial) {
  constexpr int N = 2, Cin = 5, Cout = 7, H = 11, W = 9;
  const auto    hin   = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 30);
  const auto    hw    = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 31);

  const auto expected = CpuConv2d(hin, hw, nullptr, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, false);
  const auto actual =
      RunGpuConv(hin, hw, nullptr, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 2e-5f);
}

TEST_F(MlOpsConv2dTest, FusedBiasReluMatchesUnfused) {
  constexpr int             N = 1, Cin = 16, Cout = 32, H = 24, W = 24;
  const auto                hin   = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 40);
  const auto                hw    = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 41);
  const auto                hb    = MakePattern(static_cast<std::size_t>(Cout), 42);

  // Unfused: Conv2d (+bias) then Relu
  const int                 Ho    = cuda::nn::Conv2dOutputSize(H, 0, 1, 3, 1);
  const int                 Wo    = cuda::nn::Conv2dOutputSize(W, 0, 1, 3, 1);
  const std::size_t         in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t         w_n   = static_cast<std::size_t>(Cout) * Cin * 9;
  const std::size_t         out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

  cuda::nn::DeviceBufferF32 d_in(in_n);
  cuda::nn::DeviceBufferF32 d_w(w_n);
  cuda::nn::DeviceBufferF32 d_b(static_cast<std::size_t>(Cout));
  cuda::nn::DeviceBufferF32 d_mid(out_n);
  cuda::nn::DeviceBufferF32 d_unfused(out_n);
  cuda::nn::DeviceBufferF32 d_fused(out_n);
  d_in.Upload(hin);
  d_w.Upload(hw);
  d_b.Upload(hb);

  auto                   tin    = d_in.AsTensor({N, Cin, H, W});
  auto                   tmid   = d_mid.AsTensor({N, Cout, Ho, Wo});
  auto                   tunf   = d_unfused.AsTensor({N, Cout, Ho, Wo});
  auto                   tfused = d_fused.AsTensor({N, Cout, Ho, Wo});

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
  ASSERT_EQ(
      ::cudaMemcpy(d_unfused.get(), d_mid.get(), out_n * sizeof(float), cudaMemcpyDeviceToDevice),
      cudaSuccess);
  cuda::nn::ReluInplace(tunf);
  cuda::nn::Conv2dBiasRelu(tin, tfused, p);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ExpectVectorsNear(d_fused.Download(), d_unfused.Download(), 1e-6f);

  const auto expected = CpuConv2d(hin, hw, &hb, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
  ExpectVectorsNear(d_fused.Download(), expected, 5e-5f);
}

TEST_F(MlOpsConv2dTest, WorkspacePathNoCrash) {
  constexpr int           N = 1, Cin = 3, Cout = 8, H = 20, W = 20;
  const auto              hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 50);
  const auto              hw  = MakePattern(static_cast<std::size_t>(Cout * Cin * 9), 51);
  const auto              hb  = MakePattern(static_cast<std::size_t>(Cout), 52);

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
    GTEST_SKIP() << "failed to load pack.weight: " << e.what();
  }
  // Student pack is bias-free fixed one-hot.
  const auto&   w = cuda::nn::RequireF32Tensor(map, "pack.weight", {4, 3, 2, 2});

  constexpr int N = 1, Cin = 3, Cout = 4, H = 32, W = 32;
  const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 60);
  const auto    expected =
      CpuConv2d(hin, w.data, nullptr, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, 1, false);
  const auto actual =
      RunGpuConv(hin, w.data, nullptr, N, Cin, Cout, H, W, 2, 2, 2, 2, 0, 0, false, nullptr);
  ExpectVectorsNear(actual, expected, 5e-5f);
}

TEST_F(MlOpsConv2dTest, RealWeightTrunk0AndOutput1x1) {
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
  const auto& trunk0_w = cuda::nn::RequireF32Tensor(map, "trunk.0.weight", {24, 4, 3, 3});
  const auto& trunk0_b = cuda::nn::RequireF32Tensor(map, "trunk.0.bias", {24});
  const auto& out_w    = cuda::nn::RequireF32Tensor(map, "output.weight", {3, 24, 1, 1});
  const auto& out_b    = cuda::nn::RequireF32Tensor(map, "output.bias", {3});

  {
    constexpr int N = 1, Cin = 4, Cout = 24, H = 20, W = 20;
    const auto    hin   = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 70);
    const auto expected = CpuConv2d(hin, trunk0_w.data, &trunk0_b.data, N, Cin, Cout, H, W, 3, 3, 1,
                                    1, 0, 0, 1, true);
    const auto actual = RunGpuConv(hin, trunk0_w.data, &trunk0_b.data, N, Cin, Cout, H, W, 3, 3, 1,
                                   1, 0, 0, true, nullptr);
    ExpectVectorsNear(actual, expected, 1e-4f);
  }

  {
    constexpr int N = 1, Cin = 24, Cout = 3, H = 17, W = 19;
    const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 71);
    const auto    expected =
        CpuConv2d(hin, out_w.data, &out_b.data, N, Cin, Cout, H, W, 1, 1, 1, 1, 0, 0, 1, false);
    const auto actual = RunGpuConv(hin, out_w.data, &out_b.data, N, Cin, Cout, H, W, 1, 1, 1, 1, 0,
                                   0, false, nullptr);
    ExpectVectorsNear(actual, expected, 1e-4f);
  }
}

TEST_F(MlOpsConv2dTest, RealWeightPostConvBayerAndXtrans) {
  // Student post_conv: Bayer 6→24; X-Trans 6→32
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
    const auto&   w = cuda::nn::RequireF32Tensor(map, "post_conv.weight", {24, 6, 3, 3});
    const auto&   b = cuda::nn::RequireF32Tensor(map, "post_conv.bias", {24});
    constexpr int N = 1, Cin = 6, Cout = 24, H = 18, W = 18;
    const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 80);
    const auto    expected =
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
    const auto&   w = cuda::nn::RequireF32Tensor(map, "post_conv.weight", {32, 6, 3, 3});
    const auto&   b = cuda::nn::RequireF32Tensor(map, "post_conv.bias", {32});
    constexpr int N = 1, Cin = 6, Cout = 32, H = 16, W = 16;
    const auto    hin = MakePattern(static_cast<std::size_t>(N * Cin * H * W), 81);
    const auto    expected =
        CpuConv2d(hin, w.data, &b.data, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, 1, true);
    const auto actual =
        RunGpuConv(hin, w.data, &b.data, N, Cin, Cout, H, W, 3, 3, 1, 1, 0, 0, true, nullptr);
    ExpectVectorsNear(actual, expected, 1e-4f);
  }
}

TEST_F(MlOpsConv2dTest, PerfThreeByThreeC64_512) {
  // Soft performance floor: fail only on catastrophic regressions.
  constexpr int             N = 1, Cin = 64, Cout = 64, H = 512, W = 512;
  const int                 Ho    = cuda::nn::Conv2dOutputSize(H, 0, 1, 3, 1);
  const int                 Wo    = cuda::nn::Conv2dOutputSize(W, 0, 1, 3, 1);
  const std::size_t         in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t         w_n   = static_cast<std::size_t>(Cout) * Cin * 9;
  const std::size_t         out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

  const auto                hin   = MakePattern(in_n, 90);
  const auto                hw    = MakePattern(w_n, 91);
  const auto                hb    = MakePattern(static_cast<std::size_t>(Cout), 92);

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
  const double flops  = 2.0 * N * Cout * Ho * Wo * Cin * 3.0 * 3.0;
  const double gflops = (flops / (ms_per * 1e-3)) / 1e9;

  std::cout << "[Conv2d perf] 3x3 C=64 H=W=512: " << ms_per << " ms/iter, " << gflops
            << " GFLOP/s\n";

  EXPECT_GT(gflops, 50.0);
  EXPECT_LT(ms_per, 200.0);
}

// Matches the X-Trans / Bayer tile spatial scale used in Phase 6c profiling.
TEST_F(MlOpsConv2dTest, PerfThreeByThreeC64_1024) {
  constexpr int             N = 1, Cin = 64, Cout = 64, H = 1024, W = 1024;
  const int                 Ho    = cuda::nn::Conv2dOutputSize(H, 0, 1, 3, 1);
  const int                 Wo    = cuda::nn::Conv2dOutputSize(W, 0, 1, 3, 1);
  const std::size_t         in_n  = static_cast<std::size_t>(N) * Cin * H * W;
  const std::size_t         w_n   = static_cast<std::size_t>(Cout) * Cin * 9;
  const std::size_t         out_n = static_cast<std::size_t>(N) * Cout * Ho * Wo;

  const auto                hin   = MakePattern(in_n, 93);
  const auto                hw    = MakePattern(w_n, 94);
  const auto                hb    = MakePattern(static_cast<std::size_t>(Cout), 95);

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

  EXPECT_GT(gflops, 50.0);
  EXPECT_LT(ms_per, 400.0);
}

}  // namespace alcedo
