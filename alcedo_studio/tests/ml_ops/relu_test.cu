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
#include "cuda/nn/relu.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

// Local alias so existing tests stay compact; storage lives in cuda::nn.
template <typename T>
using DeviceBuffer = cuda::nn::DeviceBuffer<T>;

auto CpuRelu(const std::vector<float>& input) -> std::vector<float> {
  std::vector<float> out(input.size());
  for (std::size_t i = 0; i < input.size(); ++i) {
    out[i] = std::max(input[i], 0.0f);
  }
  return out;
}

auto MakePattern(std::size_t n, std::uint32_t seed = 42U) -> std::vector<float> {
  std::mt19937                          rng(seed);
  std::uniform_real_distribution<float> dist(-3.0f, 3.0f);
  std::vector<float>                    values(n);
  for (std::size_t i = 0; i < n; ++i) {
    values[i] = dist(rng);
  }
  // Sprinkle exact zeros and extremes.
  if (n > 0) {
    values[0] = 0.0f;
  }
  if (n > 1) {
    values[1] = -0.0f;
  }
  if (n > 2) {
    values[2] = -1.0e-6f;
  }
  if (n > 3) {
    values[3] = 1.0e-6f;
  }
  return values;
}

void ExpectVectorsNear(const std::vector<float>& actual, const std::vector<float>& expected,
                       float abs_tol = 0.0f) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    EXPECT_NEAR(actual[i], expected[i], abs_tol) << "mismatch at index " << i;
  }
}

}  // namespace

class MlOpsReluTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsReluTest, ContiguousPointerMatchesCpuReference) {
  constexpr std::size_t kN = 10'007;  // prime-ish, exercises float4 tail
  const auto            host_in  = MakePattern(kN);
  const auto            expected = CpuRelu(host_in);

  DeviceBuffer<float> d_in(kN);
  DeviceBuffer<float> d_out(kN);
  d_in.Upload(host_in);

  cuda::nn::Relu(d_in.get(), d_out.get(), static_cast<std::int64_t>(kN));
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ExpectVectorsNear(d_out.Download(), expected);
}

TEST_F(MlOpsReluTest, InplaceMatchesOutOfPlace) {
  constexpr std::size_t kN = 4096;
  const auto            host_in  = MakePattern(kN, 7U);
  const auto            expected = CpuRelu(host_in);

  DeviceBuffer<float> d_a(kN);
  DeviceBuffer<float> d_b(kN);
  d_a.Upload(host_in);
  d_b.Upload(host_in);

  cuda::nn::Relu(d_a.get(), d_a.get(), static_cast<std::int64_t>(kN));
  cuda::nn::ReluInplace(d_b.get(), static_cast<std::int64_t>(kN));
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ExpectVectorsNear(d_a.Download(), expected);
  ExpectVectorsNear(d_b.Download(), expected);
}

TEST_F(MlOpsReluTest, EmptyAndTinyInputsAreHandled) {
  cuda::nn::Relu(static_cast<const float*>(nullptr), static_cast<float*>(nullptr), 0);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  DeviceBuffer<float> d(1);
  d.Upload({-2.5f});
  cuda::nn::ReluInplace(d.get(), 1);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  EXPECT_FLOAT_EQ(d.Download()[0], 0.0f);

  DeviceBuffer<float> d3(3);
  d3.Upload({-1.0f, 0.0f, 2.0f});
  cuda::nn::ReluInplace(d3.get(), 3);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  ExpectVectorsNear(d3.Download(), {0.0f, 0.0f, 2.0f});
}

TEST_F(MlOpsReluTest, NchwActivationShapeMatchesCpu) {
  // Typical demosaicnet intermediate: N=1, C=64, H=32, W=48
  constexpr std::int64_t n = 1;
  constexpr std::int64_t c = 64;
  constexpr std::int64_t h = 32;
  constexpr std::int64_t w = 48;
  const std::size_t      numel = static_cast<std::size_t>(n * c * h * w);

  const auto host_in  = MakePattern(numel, 99U);
  const auto expected = CpuRelu(host_in);

  DeviceBuffer<float> d_in(numel);
  DeviceBuffer<float> d_out(numel);
  d_in.Upload(host_in);

  auto in_tensor  = cuda::nn::DeviceTensor::Contiguous(d_in.get(), {n, c, h, w});
  auto out_tensor = cuda::nn::DeviceTensor::Contiguous(d_out.get(), {n, c, h, w});
  ASSERT_TRUE(in_tensor.IsContiguous());
  EXPECT_EQ(in_tensor.Numel(), static_cast<std::int64_t>(numel));

  cuda::nn::Relu(in_tensor, out_tensor);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  ExpectVectorsNear(d_out.Download(), expected);
}

TEST_F(MlOpsReluTest, StridedChannelViewMatchesContiguous) {
  // Contiguous NCHW buffer; apply ReLU only via a strided view that skips every other element
  // along W by using a custom stride pattern on a dense buffer of known values.
  constexpr std::int64_t h = 4;
  constexpr std::int64_t w = 8;
  const std::size_t      numel = static_cast<std::size_t>(h * w);

  std::vector<float> host_in(numel);
  for (std::size_t i = 0; i < numel; ++i) {
    host_in[i] = static_cast<float>(static_cast<int>(i) - 16);  // mix of +/−
  }

  DeviceBuffer<float> d_in(numel);
  DeviceBuffer<float> d_out(numel);
  d_in.Upload(host_in);
  // Initialize output with sentinel so untouched locations stay detectable.
  {
    std::vector<float> sent(numel, -123.0f);
    d_out.Upload(sent);
  }

  // View as [H, W] but with stride_w = 2 (every other column) and shape_w = W/2.
  // This models a non-contiguous slice without requiring a separate allocation.
  cuda::nn::DeviceTensor in_view;
  in_view.data       = d_in.get();
  in_view.rank       = 2;
  in_view.shape[0]   = h;
  in_view.shape[1]   = w / 2;
  in_view.strides[0] = w;
  in_view.strides[1] = 2;

  cuda::nn::DeviceTensor out_view = in_view;
  out_view.data                   = d_out.get();

  cuda::nn::Relu(in_view, out_view);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_out.Download();
  for (std::int64_t y = 0; y < h; ++y) {
    for (std::int64_t x = 0; x < w / 2; ++x) {
      const std::size_t idx = static_cast<std::size_t>(y * w + x * 2);
      EXPECT_FLOAT_EQ(got[idx], std::max(host_in[idx], 0.0f)) << "y=" << y << " x=" << x;
    }
  }
}

TEST_F(MlOpsReluTest, GpuMatMatchesCpu) {
  constexpr int rows = 17;
  constexpr int cols = 23;
  cv::Mat       host(rows, cols, CV_32FC3);
  std::mt19937  rng(123U);
  std::uniform_real_distribution<float> dist(-2.0f, 2.0f);
  for (int y = 0; y < rows; ++y) {
    auto* row = host.ptr<cv::Vec3f>(y);
    for (int x = 0; x < cols; ++x) {
      row[x] = cv::Vec3f(dist(rng), dist(rng), dist(rng));
    }
  }

  cv::cuda::GpuMat d_in;
  d_in.upload(host);
  // OpenCV may pitch-pad rows; both continuous and pitched Relu paths must be correct.

  cv::cuda::GpuMat d_out;
  cuda::nn::Relu(d_in, d_out);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cv::Mat got;
  d_out.download(got);
  ASSERT_EQ(got.type(), host.type());
  ASSERT_EQ(got.size(), host.size());

  for (int y = 0; y < rows; ++y) {
    const auto* src = host.ptr<cv::Vec3f>(y);
    const auto* dst = got.ptr<cv::Vec3f>(y);
    for (int x = 0; x < cols; ++x) {
      for (int c = 0; c < 3; ++c) {
        EXPECT_FLOAT_EQ(dst[x][c], std::max(src[x][c], 0.0f));
      }
    }
  }
}

TEST_F(MlOpsReluTest, GpuMatInplace) {
  cv::Mat host = (cv::Mat_<float>(2, 2) << -1.0f, 0.5f, 2.0f, -3.0f);
  cv::cuda::GpuMat d;
  d.upload(host);
  cuda::nn::ReluInplace(d);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cv::Mat got;
  d.download(got);
  EXPECT_FLOAT_EQ(got.at<float>(0, 0), 0.0f);
  EXPECT_FLOAT_EQ(got.at<float>(0, 1), 0.5f);
  EXPECT_FLOAT_EQ(got.at<float>(1, 0), 2.0f);
  EXPECT_FLOAT_EQ(got.at<float>(1, 1), 0.0f);
}

TEST_F(MlOpsReluTest, DeviceTensorFromGpuMatZeroCopy) {
  cv::Mat host(8, 16, CV_32FC1);
  for (int i = 0; i < host.rows * host.cols; ++i) {
    host.at<float>(i) = static_cast<float>(i - 50);
  }
  cv::cuda::GpuMat d;
  d.upload(host);

  auto tensor = cuda::nn::DeviceTensor::FromGpuMat(d);
  EXPECT_EQ(tensor.rank, 3);
  EXPECT_EQ(tensor.shape[0], 8);
  EXPECT_EQ(tensor.shape[1], 16);
  EXPECT_EQ(tensor.shape[2], 1);
  EXPECT_EQ(tensor.data, d.ptr<float>());

  cuda::nn::ReluInplace(tensor);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cv::Mat got;
  d.download(got);
  for (int i = 0; i < host.rows * host.cols; ++i) {
    EXPECT_FLOAT_EQ(got.at<float>(i), std::max(host.at<float>(i), 0.0f));
  }
}

// Performance: ReLU is memory-bandwidth bound. We measure effective GB/s and require a
// reasonable fraction of a modern GPU's bandwidth so regressions in launch config / path
// selection (e.g. accidentally forcing the strided kernel) fail the test.
TEST_F(MlOpsReluTest, ContiguousBandwidthIsSane) {
  // ~64 MiB of f32 ≈ demosaicnet-scale activation for a large tile (C=64, H=W≈512).
  constexpr std::size_t kN         = 16U * 1024U * 1024U;
  constexpr int         kWarmup    = 5;
  constexpr int         kIters     = 20;
  constexpr double      kMinGBs    = 50.0;  // well below any modern discrete GPU; catches catastrophes

  const auto host_in = MakePattern(kN, 1U);
  DeviceBuffer<float> d_in(kN);
  DeviceBuffer<float> d_out(kN);
  d_in.Upload(host_in);

  for (int i = 0; i < kWarmup; ++i) {
    cuda::nn::Relu(d_in.get(), d_out.get(), static_cast<std::int64_t>(kN));
  }
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start = nullptr;
  cudaEvent_t stop  = nullptr;
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);

  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::Relu(d_in.get(), d_out.get(), static_cast<std::int64_t>(kN));
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float elapsed_ms = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&elapsed_ms, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  // Read input + write output once per iteration.
  const double bytes =
      static_cast<double>(kIters) * static_cast<double>(kN) * sizeof(float) * 2.0;
  const double seconds = static_cast<double>(elapsed_ms) * 1.0e-3;
  const double gbs     = (bytes / seconds) / (1024.0 * 1024.0 * 1024.0);

  std::cout << "[MlOpsRelu] contiguous out-of-place: n=" << kN << " iters=" << kIters
            << " elapsed_ms=" << elapsed_ms << " effective_GBs=" << gbs << std::endl;

  EXPECT_GT(gbs, kMinGBs) << "ReLU bandwidth far below expectation; check vectorized path";

  // Correctness smoke on the last result.
  const auto got = d_out.Download();
  for (std::size_t i = 0; i < 64; ++i) {
    EXPECT_FLOAT_EQ(got[i], std::max(host_in[i], 0.0f));
  }
}

TEST_F(MlOpsReluTest, InplaceBandwidthIsSane) {
  constexpr std::size_t kN      = 16U * 1024U * 1024U;
  constexpr int         kWarmup = 5;
  constexpr int         kIters  = 20;
  constexpr double      kMinGBs = 40.0;

  DeviceBuffer<float> d(kN);
  d.Upload(MakePattern(kN, 2U));

  for (int i = 0; i < kWarmup; ++i) {
    cuda::nn::ReluInplace(d.get(), static_cast<std::int64_t>(kN));
  }
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start = nullptr;
  cudaEvent_t stop  = nullptr;
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);

  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::ReluInplace(d.get(), static_cast<std::int64_t>(kN));
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float elapsed_ms = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&elapsed_ms, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  // In-place still typically loads + stores each element (no free lunch for ReLU).
  const double bytes =
      static_cast<double>(kIters) * static_cast<double>(kN) * sizeof(float) * 2.0;
  const double seconds = static_cast<double>(elapsed_ms) * 1.0e-3;
  const double gbs     = (bytes / seconds) / (1024.0 * 1024.0 * 1024.0);

  std::cout << "[MlOpsRelu] contiguous inplace: n=" << kN << " iters=" << kIters
            << " elapsed_ms=" << elapsed_ms << " effective_GBs=" << gbs << std::endl;

  EXPECT_GT(gbs, kMinGBs);
}

TEST_F(MlOpsReluTest, RejectsShapeMismatch) {
  DeviceBuffer<float> a(8);
  DeviceBuffer<float> b(8);
  auto                t_a = cuda::nn::DeviceTensor::Contiguous(a.get(), {2, 4});
  auto                t_b = cuda::nn::DeviceTensor::Contiguous(b.get(), {4, 2});
  EXPECT_THROW(cuda::nn::Relu(t_a, t_b), std::runtime_error);
}

}  // namespace alcedo
