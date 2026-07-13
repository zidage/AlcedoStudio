//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <iostream>
#include <random>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/concat.hpp"
#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

// CPU NCHW concat along C.
auto CpuConcatChannels(const std::vector<float>& a, int N, int Ca, int H, int W,
                       const std::vector<float>& b, int Cb) -> std::vector<float> {
  const int Cout = Ca + Cb;
  std::vector<float> out(static_cast<std::size_t>(N) * Cout * H * W);
  for (int n = 0; n < N; ++n) {
    for (int c = 0; c < Ca; ++c) {
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          const std::size_t ia = static_cast<std::size_t>(((n * Ca + c) * H + y) * W + x);
          const std::size_t io = static_cast<std::size_t>(((n * Cout + c) * H + y) * W + x);
          out[io] = a[ia];
        }
      }
    }
    for (int c = 0; c < Cb; ++c) {
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          const std::size_t ib = static_cast<std::size_t>(((n * Cb + c) * H + y) * W + x);
          const std::size_t io =
              static_cast<std::size_t>(((n * Cout + (Ca + c)) * H + y) * W + x);
          out[io] = b[ib];
        }
      }
    }
  }
  return out;
}

auto FillIota(std::size_t n, float start = 0.0f) -> std::vector<float> {
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = start + static_cast<float>(i);
  }
  return v;
}

}  // namespace

class MlOpsConcatTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsConcatTest, BayerStyleThreePlusThree) {
  // demosaicnet Bayer full-res: crop mosaic [1,3,H,W] + up [1,3,H,W] → [1,6,H,W]
  constexpr int N = 1;
  constexpr int Ca = 3;
  constexpr int Cb = 3;
  constexpr int H = 7;
  constexpr int W = 9;
  const auto ha = FillIota(static_cast<std::size_t>(N * Ca * H * W), 0.0f);
  const auto hb = FillIota(static_cast<std::size_t>(N * Cb * H * W), 1000.0f);
  const auto expected = CpuConcatChannels(ha, N, Ca, H, W, hb, Cb);

  cuda::nn::DeviceBufferF32 da(ha.size());
  cuda::nn::DeviceBufferF32 db(hb.size());
  cuda::nn::DeviceBufferF32 dout(expected.size());
  da.Upload(ha);
  db.Upload(hb);

  auto ta = cuda::nn::DeviceTensor::Contiguous(da.get(), {N, Ca, H, W});
  auto tb = cuda::nn::DeviceTensor::Contiguous(db.get(), {N, Cb, H, W});
  auto to = cuda::nn::DeviceTensor::Contiguous(dout.get(), {N, Ca + Cb, H, W});

  cuda::nn::ConcatChannels(ta, tb, to);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = dout.Download();
  ASSERT_EQ(got.size(), expected.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]) << "i=" << i;
  }
  EXPECT_TRUE(to.IsContiguous());
}

TEST_F(MlOpsConcatTest, XTransStyleThreePlusSixtyFour) {
  constexpr int N  = 1;
  constexpr int Ca = 3;
  constexpr int Cb = 64;
  constexpr int H  = 5;
  constexpr int W  = 6;
  const auto ha = FillIota(static_cast<std::size_t>(N * Ca * H * W), 1.0f);
  const auto hb = FillIota(static_cast<std::size_t>(N * Cb * H * W), -50.0f);
  const auto expected = CpuConcatChannels(ha, N, Ca, H, W, hb, Cb);

  cuda::nn::DeviceBufferF32 da(ha.size());
  cuda::nn::DeviceBufferF32 db(hb.size());
  cuda::nn::DeviceBufferF32 dout(expected.size());
  da.Upload(ha);
  db.Upload(hb);

  auto ta = da.AsTensor({N, Ca, H, W});
  auto tb = db.AsTensor({N, Cb, H, W});
  auto to = dout.AsTensor({N, Ca + Cb, H, W});

  cuda::nn::ConcatChannels(ta, tb, to);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = dout.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

TEST_F(MlOpsConcatTest, OddSpatialAndMultiBatch) {
  constexpr int N  = 2;
  constexpr int Ca = 2;
  constexpr int Cb = 5;
  constexpr int H  = 3;
  constexpr int W  = 4;
  const auto ha = FillIota(static_cast<std::size_t>(N * Ca * H * W), 0.25f);
  const auto hb = FillIota(static_cast<std::size_t>(N * Cb * H * W), -0.5f);
  const auto expected = CpuConcatChannels(ha, N, Ca, H, W, hb, Cb);

  cuda::nn::DeviceBufferF32 da(ha.size());
  cuda::nn::DeviceBufferF32 db(hb.size());
  cuda::nn::DeviceBufferF32 dout(expected.size());
  da.Upload(ha);
  db.Upload(hb);

  auto ta = da.AsTensor({N, Ca, H, W});
  auto tb = db.AsTensor({N, Cb, H, W});
  auto to = dout.AsTensor({N, Ca + Cb, H, W});
  cuda::nn::ConcatChannels(ta, tb, to);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = dout.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

TEST_F(MlOpsConcatTest, EmptySpatial) {
  cuda::nn::DeviceBufferF32 da(0);
  cuda::nn::DeviceBufferF32 db(0);
  cuda::nn::DeviceBufferF32 dout(0);
  // Zero-size buffers: construct empty tensors carefully.
  float* nullp = nullptr;
  auto ta = cuda::nn::DeviceTensor::Contiguous(nullp, {1, 3, 0, 0});
  auto tb = cuda::nn::DeviceTensor::Contiguous(nullp, {1, 3, 0, 0});
  auto to = cuda::nn::DeviceTensor::Contiguous(nullp, {1, 6, 0, 0});
  // null data with numel 0 — our ValidateRank4Nchw rejects null data.
  // Use tiny non-empty and skip; or allocate 1 float dummy.
  cuda::nn::DeviceBufferF32 dummy(1);
  ta = cuda::nn::DeviceTensor::Contiguous(dummy.get(), {1, 3, 0, 4});
  tb = cuda::nn::DeviceTensor::Contiguous(dummy.get(), {1, 3, 0, 4});
  to = cuda::nn::DeviceTensor::Contiguous(dummy.get(), {1, 6, 0, 4});
  // Numel is 0 — should no-op without error.
  EXPECT_EQ(ta.Numel(), 0);
  cuda::nn::ConcatChannels(ta, tb, to);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
}

TEST_F(MlOpsConcatTest, RejectsSpatialMismatch) {
  cuda::nn::DeviceBufferF32 da(3 * 4 * 5);
  cuda::nn::DeviceBufferF32 db(3 * 3 * 5);
  cuda::nn::DeviceBufferF32 dout(6 * 4 * 5);
  auto ta = da.AsTensor({1, 3, 4, 5});
  auto tb = db.AsTensor({1, 3, 3, 5});
  auto to = dout.AsTensor({1, 6, 4, 5});
  EXPECT_THROW(cuda::nn::ConcatChannels(ta, tb, to), std::runtime_error);
}

TEST_F(MlOpsConcatTest, RejectsBadOutputShape) {
  cuda::nn::DeviceBufferF32 da(3 * 2 * 2);
  cuda::nn::DeviceBufferF32 db(3 * 2 * 2);
  cuda::nn::DeviceBufferF32 dout(5 * 2 * 2);
  auto ta = da.AsTensor({1, 3, 2, 2});
  auto tb = db.AsTensor({1, 3, 2, 2});
  auto to = dout.AsTensor({1, 5, 2, 2});  // should be 6
  EXPECT_THROW(cuda::nn::ConcatChannels(ta, tb, to), std::runtime_error);
}

TEST_F(MlOpsConcatTest, BandwidthSmokeLarge) {
  // Large concat like XTrans skip: 3 + 64 channels at 256²
  constexpr int N  = 1;
  constexpr int Ca = 3;
  constexpr int Cb = 64;
  constexpr int H  = 256;
  constexpr int W  = 256;
  constexpr int kWarmup = 3;
  constexpr int kIters  = 10;
  constexpr double kMinGBs = 20.0;  // soft floor; pure bandwidth copy-ish

  const std::size_t na = static_cast<std::size_t>(N * Ca * H * W);
  const std::size_t nb = static_cast<std::size_t>(N * Cb * H * W);
  const std::size_t no = static_cast<std::size_t>(N * (Ca + Cb) * H * W);

  cuda::nn::DeviceBufferF32 da(na);
  cuda::nn::DeviceBufferF32 db(nb);
  cuda::nn::DeviceBufferF32 dout(no);
  da.FillZero();
  db.FillZero();

  auto ta = da.AsTensor({N, Ca, H, W});
  auto tb = db.AsTensor({N, Cb, H, W});
  auto to = dout.AsTensor({N, Ca + Cb, H, W});

  for (int i = 0; i < kWarmup; ++i) {
    cuda::nn::ConcatChannels(ta, tb, to);
  }
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  cudaEvent_t start = nullptr;
  cudaEvent_t stop  = nullptr;
  ASSERT_EQ(::cudaEventCreate(&start), cudaSuccess);
  ASSERT_EQ(::cudaEventCreate(&stop), cudaSuccess);
  ASSERT_EQ(::cudaEventRecord(start), cudaSuccess);
  for (int i = 0; i < kIters; ++i) {
    cuda::nn::ConcatChannels(ta, tb, to);
  }
  ASSERT_EQ(::cudaEventRecord(stop), cudaSuccess);
  ASSERT_EQ(::cudaEventSynchronize(stop), cudaSuccess);

  float elapsed_ms = 0.0f;
  ASSERT_EQ(::cudaEventElapsedTime(&elapsed_ms, start, stop), cudaSuccess);
  ::cudaEventDestroy(start);
  ::cudaEventDestroy(stop);

  // Read a + read b + write out.
  const double bytes = static_cast<double>(kIters) *
                       static_cast<double>((na + nb + no) * sizeof(float));
  const double seconds = static_cast<double>(elapsed_ms) * 1.0e-3;
  const double gbs     = (bytes / seconds) / (1024.0 * 1024.0 * 1024.0);

  std::cout << "[MlOpsConcat] large: " << Ca << "+" << Cb << " C @ " << H << "x" << W
            << " elapsed_ms=" << elapsed_ms << " effective_GBs=" << gbs << std::endl;
  EXPECT_GT(gbs, kMinGBs);
}

}  // namespace alcedo
