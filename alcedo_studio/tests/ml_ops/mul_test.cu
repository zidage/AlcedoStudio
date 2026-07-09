//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/mul.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto MakePattern(std::size_t n, std::uint32_t seed) -> std::vector<float> {
  std::mt19937                          rng(seed);
  std::uniform_real_distribution<float> dist(-2.0f, 2.0f);
  std::vector<float>                    v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = dist(rng);
  }
  if (n > 0) {
    v[0] = 0.0f;
  }
  if (n > 1) {
    v[1] = -0.0f;
  }
  return v;
}

auto CpuMul(const std::vector<float>& a, const std::vector<float>& b) -> std::vector<float> {
  std::vector<float> out(a.size());
  for (std::size_t i = 0; i < a.size(); ++i) {
    out[i] = a[i] * b[i];
  }
  return out;
}

void ExpectVectorsNear(const std::vector<float>& actual, const std::vector<float>& expected,
                       float abs_tol = 0.0f) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t i = 0; i < actual.size(); ++i) {
    EXPECT_NEAR(actual[i], expected[i], abs_tol) << "mismatch at index " << i;
  }
}

}  // namespace

class MlOpsMulTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsMulTest, ContiguousPointerMatchesCpu) {
  constexpr std::size_t kN = 10'007;  // prime-ish, exercises float4 tail
  const auto            ha = MakePattern(kN, 11U);
  const auto            hb = MakePattern(kN, 22U);
  const auto            expected = CpuMul(ha, hb);

  cuda::nn::DeviceBufferF32 da(kN);
  cuda::nn::DeviceBufferF32 db(kN);
  cuda::nn::DeviceBufferF32 dout(kN);
  da.Upload(ha);
  db.Upload(hb);

  cuda::nn::Mul(da.get(), db.get(), dout.get(), static_cast<std::int64_t>(kN));
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  ExpectVectorsNear(dout.Download(), expected);
}

TEST_F(MlOpsMulTest, InplaceMatchesOutOfPlace) {
  constexpr std::size_t kN = 4096;
  const auto            ha = MakePattern(kN, 3U);
  const auto            hb = MakePattern(kN, 4U);
  const auto            expected = CpuMul(ha, hb);

  cuda::nn::DeviceBufferF32 a(kN);
  cuda::nn::DeviceBufferF32 b(kN);
  cuda::nn::DeviceBufferF32 a2(kN);
  a.Upload(ha);
  a2.Upload(ha);
  b.Upload(hb);

  cuda::nn::Mul(a.get(), b.get(), a.get(), static_cast<std::int64_t>(kN));
  cuda::nn::MulInplace(a2.get(), b.get(), static_cast<std::int64_t>(kN));
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  ExpectVectorsNear(a.Download(), expected);
  ExpectVectorsNear(a2.Download(), expected);
}

TEST_F(MlOpsMulTest, EmptyAndTiny) {
  cuda::nn::Mul(static_cast<const float*>(nullptr), static_cast<const float*>(nullptr),
                static_cast<float*>(nullptr), 0);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cuda::nn::DeviceBufferF32 a(1);
  cuda::nn::DeviceBufferF32 b(1);
  cuda::nn::DeviceBufferF32 o(1);
  a.Upload({3.0f});
  b.Upload({-2.0f});
  cuda::nn::Mul(a.get(), b.get(), o.get(), 1);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  EXPECT_FLOAT_EQ(o.Download()[0], -6.0f);

  cuda::nn::DeviceBufferF32 a3(3);
  cuda::nn::DeviceBufferF32 b3(3);
  a3.Upload({1.0f, -2.0f, 0.5f});
  b3.Upload({2.0f, 3.0f, 4.0f});
  cuda::nn::MulInplace(a3.get(), b3.get(), 3);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  ExpectVectorsNear(a3.Download(), {2.0f, -6.0f, 2.0f});
}

TEST_F(MlOpsMulTest, NchwTensorMatchesCpu) {
  // Bayer split/mul shape: N=1, C=64, modest spatial
  constexpr std::int64_t n = 1;
  constexpr std::int64_t c = 64;
  constexpr std::int64_t h = 17;  // odd
  constexpr std::int64_t w = 23;  // odd
  const std::size_t      numel = static_cast<std::size_t>(n * c * h * w);

  const auto ha = MakePattern(numel, 50U);
  const auto hb = MakePattern(numel, 51U);
  const auto expected = CpuMul(ha, hb);

  cuda::nn::DeviceBufferF32 da(numel);
  cuda::nn::DeviceBufferF32 db(numel);
  cuda::nn::DeviceBufferF32 dout(numel);
  da.Upload(ha);
  db.Upload(hb);

  auto ta = cuda::nn::DeviceTensor::Contiguous(da.get(), {n, c, h, w});
  auto tb = cuda::nn::DeviceTensor::Contiguous(db.get(), {n, c, h, w});
  auto to = cuda::nn::DeviceTensor::Contiguous(dout.get(), {n, c, h, w});

  cuda::nn::Mul(ta, tb, to);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  ExpectVectorsNear(dout.Download(), expected);
}

TEST_F(MlOpsMulTest, StridedViewsMatchContiguous) {
  // Model Bayer split: contiguous 128-ch, mul of two 64-ch channel views.
  constexpr std::int64_t N = 1;
  constexpr std::int64_t C = 4;
  constexpr std::int64_t H = 3;
  constexpr std::int64_t W = 5;
  const std::size_t      numel = static_cast<std::size_t>(N * C * H * W);

  std::vector<float> host(numel);
  for (std::size_t i = 0; i < numel; ++i) {
    host[i] = static_cast<float>(i) * 0.1f - 1.0f;
  }

  cuda::nn::DeviceBufferF32 d(numel);
  d.Upload(host);
  auto full = cuda::nn::DeviceTensor::Contiguous(d.get(), {N, C, H, W});

  // Strided channel views: channels 0..1 and 2..3 (each C'=2)
  cuda::nn::DeviceTensor a;
  a.data       = d.get();
  a.rank       = 4;
  a.shape[0]   = N;
  a.shape[1]   = 2;
  a.shape[2]   = H;
  a.shape[3]   = W;
  a.strides[0] = C * H * W;
  a.strides[1] = H * W;
  a.strides[2] = W;
  a.strides[3] = 1;

  cuda::nn::DeviceTensor b = a;
  b.data                   = d.get() + 2 * H * W;

  cuda::nn::DeviceBufferF32 dout(static_cast<std::size_t>(N * 2 * H * W));
  auto to = cuda::nn::DeviceTensor::Contiguous(dout.get(), {N, 2, H, W});

  cuda::nn::Mul(a, b, to);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = dout.Download();
  for (std::int64_t c = 0; c < 2; ++c) {
    for (std::int64_t y = 0; y < H; ++y) {
      for (std::int64_t x = 0; x < W; ++x) {
        const std::size_t ia =
            static_cast<std::size_t>(c * H * W + y * W + x);
        const std::size_t ib =
            static_cast<std::size_t>((c + 2) * H * W + y * W + x);
        const std::size_t io = ia;
        EXPECT_FLOAT_EQ(got[io], host[ia] * host[ib]);
      }
    }
  }
  (void)full;
}

TEST_F(MlOpsMulTest, ContiguousBandwidthIsSane) {
  // Mul is bandwidth-bound: read A, read B, write Out → 3 × bytes (out-of-place).
  constexpr std::size_t kN      = 16U * 1024U * 1024U;
  constexpr int         kWarmup = 5;
  constexpr int         kIters  = 20;
  constexpr double      kMinGBs = 50.0;

  const auto ha = MakePattern(kN, 1U);
  const auto hb = MakePattern(kN, 2U);
  cuda::nn::DeviceBufferF32 da(kN);
  cuda::nn::DeviceBufferF32 db(kN);
  cuda::nn::DeviceBufferF32 dout(kN);
  da.Upload(ha);
  db.Upload(hb);

  for (int i = 0; i < kWarmup; ++i) {
    cuda::nn::Mul(da.get(), db.get(), dout.get(), static_cast<std::int64_t>(kN));
  }
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start = nullptr;
  cudaEvent_t stop  = nullptr;
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);

  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::Mul(da.get(), db.get(), dout.get(), static_cast<std::int64_t>(kN));
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float elapsed_ms = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&elapsed_ms, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  const double bytes =
      static_cast<double>(kIters) * static_cast<double>(kN) * sizeof(float) * 3.0;
  const double seconds = static_cast<double>(elapsed_ms) * 1.0e-3;
  const double gbs     = (bytes / seconds) / (1024.0 * 1024.0 * 1024.0);

  std::cout << "[MlOpsMul] contiguous out-of-place: n=" << kN << " iters=" << kIters
            << " elapsed_ms=" << elapsed_ms << " effective_GBs=" << gbs << std::endl;

  EXPECT_GT(gbs, kMinGBs) << "Mul bandwidth far below expectation; check float4 path";

  // Correctness smoke on prefix.
  const auto got = dout.Download();
  for (std::size_t i = 0; i < 64; ++i) {
    EXPECT_FLOAT_EQ(got[i], ha[i] * hb[i]);
  }
}

TEST_F(MlOpsMulTest, InplaceBandwidthIsSane) {
  constexpr std::size_t kN      = 16U * 1024U * 1024U;
  constexpr int         kWarmup = 5;
  constexpr int         kIters  = 20;
  constexpr double      kMinGBs = 40.0;

  cuda::nn::DeviceBufferF32 a(kN);
  cuda::nn::DeviceBufferF32 b(kN);
  a.Upload(MakePattern(kN, 7U));
  b.Upload(MakePattern(kN, 8U));

  for (int i = 0; i < kWarmup; ++i) {
    // Reload a each warmup would dominate; just run mul on current a.
    cuda::nn::MulInplace(a.get(), b.get(), static_cast<std::int64_t>(kN));
  }
  // Reset a for the timed loop so values stay finite.
  a.Upload(MakePattern(kN, 7U));
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start = nullptr;
  cudaEvent_t stop  = nullptr;
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);

  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::MulInplace(a.get(), b.get(), static_cast<std::int64_t>(kN));
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float elapsed_ms = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&elapsed_ms, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  // Inplace: load a, load b, store a → still ~3 traffic (or 2 if cache hits on a).
  const double bytes =
      static_cast<double>(kIters) * static_cast<double>(kN) * sizeof(float) * 3.0;
  const double seconds = static_cast<double>(elapsed_ms) * 1.0e-3;
  const double gbs     = (bytes / seconds) / (1024.0 * 1024.0 * 1024.0);

  std::cout << "[MlOpsMul] contiguous inplace: n=" << kN << " iters=" << kIters
            << " elapsed_ms=" << elapsed_ms << " effective_GBs=" << gbs << std::endl;

  EXPECT_GT(gbs, kMinGBs);
}

TEST_F(MlOpsMulTest, RejectsShapeMismatch) {
  cuda::nn::DeviceBufferF32 a(8);
  cuda::nn::DeviceBufferF32 b(8);
  cuda::nn::DeviceBufferF32 o(8);
  auto ta = cuda::nn::DeviceTensor::Contiguous(a.get(), {2, 4});
  auto tb = cuda::nn::DeviceTensor::Contiguous(b.get(), {4, 2});
  auto to = cuda::nn::DeviceTensor::Contiguous(o.get(), {2, 4});
  EXPECT_THROW(cuda::nn::Mul(ta, tb, to), std::runtime_error);
}

}  // namespace alcedo
