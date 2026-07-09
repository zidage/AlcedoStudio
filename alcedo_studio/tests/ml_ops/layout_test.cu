//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>
#include <opencv2/core.hpp>
#include <opencv2/core/cuda.hpp>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/layout.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

// Host HWC [N,H,W,C] contiguous ↔ NCHW [N,C,H,W].
auto CpuPackHwcToNchw(const std::vector<float>& hwc, int N, int H, int W, int C)
    -> std::vector<float> {
  std::vector<float> nchw(static_cast<std::size_t>(N) * C * H * W);
  for (int n = 0; n < N; ++n) {
    for (int h = 0; h < H; ++h) {
      for (int w = 0; w < W; ++w) {
        for (int c = 0; c < C; ++c) {
          const std::size_t ih =
              static_cast<std::size_t>(((n * H + h) * W + w) * C + c);
          const std::size_t io =
              static_cast<std::size_t>(((n * C + c) * H + h) * W + w);
          nchw[io] = hwc[ih];
        }
      }
    }
  }
  return nchw;
}

auto CpuUnpackNchwToHwc(const std::vector<float>& nchw, int N, int C, int H, int W)
    -> std::vector<float> {
  std::vector<float> hwc(static_cast<std::size_t>(N) * H * W * C);
  for (int n = 0; n < N; ++n) {
    for (int c = 0; c < C; ++c) {
      for (int h = 0; h < H; ++h) {
        for (int w = 0; w < W; ++w) {
          const std::size_t ii =
              static_cast<std::size_t>(((n * C + c) * H + h) * W + w);
          const std::size_t io =
              static_cast<std::size_t>(((n * H + h) * W + w) * C + c);
          hwc[io] = nchw[ii];
        }
      }
    }
  }
  return hwc;
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

}  // namespace

class MlOpsLayoutTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsLayoutTest, TensorPackMatchesCpuOddSizes) {
  constexpr int N = 1;
  constexpr int H = 17;
  constexpr int W = 13;
  constexpr int C = 3;
  const auto hwc_host = MakePattern(static_cast<std::size_t>(N * H * W * C), 9U);
  const auto expected = CpuPackHwcToNchw(hwc_host, N, H, W, C);

  cuda::nn::DeviceBufferF32 d_hwc(hwc_host.size());
  cuda::nn::DeviceBufferF32 d_nchw(expected.size());
  d_hwc.Upload(hwc_host);

  auto thwc  = d_hwc.AsTensor({N, H, W, C});
  auto tnchw = d_nchw.AsTensor({N, C, H, W});
  cuda::nn::PackHwcToNchw(thwc, tnchw);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_nchw.Download();
  ASSERT_EQ(got.size(), expected.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]) << "i=" << i;
  }
}

TEST_F(MlOpsLayoutTest, TensorUnpackMatchesCpu) {
  constexpr int N = 1;
  constexpr int C = 3;
  constexpr int H = 8;
  constexpr int W = 11;
  const auto nchw_host = MakePattern(static_cast<std::size_t>(N * C * H * W), 12U);
  const auto expected  = CpuUnpackNchwToHwc(nchw_host, N, C, H, W);

  cuda::nn::DeviceBufferF32 d_nchw(nchw_host.size());
  cuda::nn::DeviceBufferF32 d_hwc(expected.size());
  d_nchw.Upload(nchw_host);

  auto tnchw = d_nchw.AsTensor({N, C, H, W});
  auto thwc  = d_hwc.AsTensor({N, H, W, C});
  cuda::nn::UnpackNchwToHwc(tnchw, thwc);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_hwc.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

TEST_F(MlOpsLayoutTest, RoundTripMaxErrorZero) {
  constexpr int N = 1;
  constexpr int H = 32;
  constexpr int W = 48;
  constexpr int C = 3;
  const auto hwc0 = MakePattern(static_cast<std::size_t>(N * H * W * C), 99U);

  cuda::nn::DeviceBufferF32 d_hwc(hwc0.size());
  cuda::nn::DeviceBufferF32 d_nchw(static_cast<std::size_t>(N * C * H * W));
  cuda::nn::DeviceBufferF32 d_back(hwc0.size());
  d_hwc.Upload(hwc0);

  auto thwc  = d_hwc.AsTensor({N, H, W, C});
  auto tnchw = d_nchw.AsTensor({N, C, H, W});
  auto tback = d_back.AsTensor({N, H, W, C});

  cuda::nn::PackHwcToNchw(thwc, tnchw);
  cuda::nn::UnpackNchwToHwc(tnchw, tback);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_back.Download();
  float max_abs = 0.0f;
  for (std::size_t i = 0; i < hwc0.size(); ++i) {
    max_abs = std::max(max_abs, std::abs(got[i] - hwc0[i]));
    EXPECT_FLOAT_EQ(got[i], hwc0[i]) << "i=" << i;
  }
  EXPECT_EQ(max_abs, 0.0f);
}

TEST_F(MlOpsLayoutTest, Rank3HwcAndNchw) {
  constexpr int H = 4;
  constexpr int W = 5;
  constexpr int C = 3;
  const auto hwc_host = MakePattern(static_cast<std::size_t>(H * W * C), 1U);
  const auto expected = CpuPackHwcToNchw(hwc_host, 1, H, W, C);

  cuda::nn::DeviceBufferF32 d_hwc(hwc_host.size());
  cuda::nn::DeviceBufferF32 d_nchw(expected.size());
  d_hwc.Upload(hwc_host);

  auto thwc  = d_hwc.AsTensor({H, W, C});
  auto tnchw = d_nchw.AsTensor({C, H, W});
  cuda::nn::PackHwcToNchw(thwc, tnchw);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_nchw.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }

  cuda::nn::DeviceBufferF32 d_back(hwc_host.size());
  auto tback = d_back.AsTensor({H, W, C});
  cuda::nn::UnpackNchwToHwc(tnchw, tback);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  const auto back = d_back.Download();
  for (std::size_t i = 0; i < back.size(); ++i) {
    EXPECT_FLOAT_EQ(back[i], hwc_host[i]);
  }
}

TEST_F(MlOpsLayoutTest, GpuMatPackUnpackRoundTrip) {
  constexpr int rows = 19;
  constexpr int cols = 27;
  cv::Mat host(rows, cols, CV_32FC3);
  std::mt19937                          rng(42U);
  std::uniform_real_distribution<float> dist(-2.0f, 2.0f);
  for (int y = 0; y < rows; ++y) {
    auto* row = host.ptr<cv::Vec3f>(y);
    for (int x = 0; x < cols; ++x) {
      row[x] = cv::Vec3f(dist(rng), dist(rng), dist(rng));
    }
  }

  cv::cuda::GpuMat d_in;
  d_in.upload(host);

  cuda::nn::DeviceBufferF32 d_nchw(static_cast<std::size_t>(1 * 3 * rows * cols));
  auto tnchw = d_nchw.AsTensor({1, 3, rows, cols});

  cuda::nn::PackHwcToNchw(d_in, tnchw);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cv::cuda::GpuMat d_out;
  cuda::nn::UnpackNchwToHwc(tnchw, d_out);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cv::Mat got;
  d_out.download(got);
  ASSERT_EQ(got.type(), host.type());
  ASSERT_EQ(got.size(), host.size());

  float max_abs = 0.0f;
  for (int y = 0; y < rows; ++y) {
    const auto* src = host.ptr<cv::Vec3f>(y);
    const auto* dst = got.ptr<cv::Vec3f>(y);
    for (int x = 0; x < cols; ++x) {
      for (int c = 0; c < 3; ++c) {
        max_abs = std::max(max_abs, std::abs(dst[x][c] - src[x][c]));
        EXPECT_FLOAT_EQ(dst[x][c], src[x][c]);
      }
    }
  }
  EXPECT_EQ(max_abs, 0.0f);
}

TEST_F(MlOpsLayoutTest, GpuMatPackMatchesCpuNchw) {
  constexpr int H = 5;
  constexpr int W = 6;
  cv::Mat host(H, W, CV_32FC3);
  float v = 0.0f;
  for (int y = 0; y < H; ++y) {
    auto* row = host.ptr<cv::Vec3f>(y);
    for (int x = 0; x < W; ++x) {
      row[x] = cv::Vec3f(v, v + 0.1f, v + 0.2f);
      v += 1.0f;
    }
  }

  // Build expected NCHW from host HWC row-major.
  std::vector<float> hwc_flat(static_cast<std::size_t>(H * W * 3));
  for (int y = 0; y < H; ++y) {
    const auto* row = host.ptr<cv::Vec3f>(y);
    for (int x = 0; x < W; ++x) {
      for (int c = 0; c < 3; ++c) {
        hwc_flat[static_cast<std::size_t>((y * W + x) * 3 + c)] = row[x][c];
      }
    }
  }
  const auto expected = CpuPackHwcToNchw(hwc_flat, 1, H, W, 3);

  cv::cuda::GpuMat d_in;
  d_in.upload(host);
  cuda::nn::DeviceBufferF32 d_nchw(expected.size());
  auto tnchw = d_nchw.AsTensor({1, 3, H, W});
  cuda::nn::PackHwcToNchw(d_in, tnchw);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_nchw.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

TEST_F(MlOpsLayoutTest, MultiChannelNotOnlyThree) {
  // Single-channel and multi-channel (4) tensors.
  constexpr int N = 1;
  constexpr int H = 4;
  constexpr int W = 4;
  for (int C : {1, 4}) {
    const auto hwc = MakePattern(static_cast<std::size_t>(N * H * W * C), static_cast<std::uint32_t>(C));
    const auto exp = CpuPackHwcToNchw(hwc, N, H, W, C);
    cuda::nn::DeviceBufferF32 d_hwc(hwc.size());
    cuda::nn::DeviceBufferF32 d_nchw(exp.size());
    d_hwc.Upload(hwc);
    auto th = d_hwc.AsTensor({N, H, W, C});
    auto tn = d_nchw.AsTensor({N, C, H, W});
    cuda::nn::PackHwcToNchw(th, tn);
    ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
    const auto got = d_nchw.Download();
    for (std::size_t i = 0; i < got.size(); ++i) {
      EXPECT_FLOAT_EQ(got[i], exp[i]) << "C=" << C << " i=" << i;
    }
  }
}

TEST_F(MlOpsLayoutTest, BandwidthSmokePack) {
  constexpr int H = 1024;
  constexpr int W = 1024;
  constexpr int C = 3;
  constexpr int kWarmup = 3;
  constexpr int kIters  = 10;
  constexpr double kMinGBs = 15.0;

  const std::size_t n = static_cast<std::size_t>(H) * W * C;
  cuda::nn::DeviceBufferF32 d_hwc(n);
  cuda::nn::DeviceBufferF32 d_nchw(n);
  d_hwc.FillZero();
  auto th = d_hwc.AsTensor({1, H, W, C});
  auto tn = d_nchw.AsTensor({1, C, H, W});

  for (int i = 0; i < kWarmup; ++i) {
    cuda::nn::PackHwcToNchw(th, tn);
  }
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start = nullptr;
  cudaEvent_t stop  = nullptr;
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);
  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::PackHwcToNchw(th, tn);
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float elapsed_ms = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&elapsed_ms, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  const double bytes = static_cast<double>(kIters) * static_cast<double>(n) * sizeof(float) * 2.0;
  const double gbs =
      (bytes / (static_cast<double>(elapsed_ms) * 1.0e-3)) / (1024.0 * 1024.0 * 1024.0);
  std::cout << "[MlOpsLayout] pack " << H << "x" << W << "x" << C << " elapsed_ms=" << elapsed_ms
            << " effective_GBs=" << gbs << std::endl;
  EXPECT_GT(gbs, kMinGBs);
}

TEST_F(MlOpsLayoutTest, RejectsShapeMismatch) {
  // Same numel (12) but NCHW spatial dims do not match HWC [1,2,2,3] → [1,3,2,2].
  cuda::nn::DeviceBufferF32 d_hwc(12);
  cuda::nn::DeviceBufferF32 d_nchw(12);
  auto th = d_hwc.AsTensor({1, 2, 2, 3});
  auto tn = d_nchw.AsTensor({1, 3, 1, 4});  // wrong H/W vs HWC
  EXPECT_THROW(cuda::nn::PackHwcToNchw(th, tn), std::runtime_error);
}

}  // namespace alcedo
