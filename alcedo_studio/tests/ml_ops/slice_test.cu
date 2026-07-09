//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/mul.hpp"
#include "cuda/nn/slice.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto FillIota(std::size_t n, float start = 0.0f) -> std::vector<float> {
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = start + static_cast<float>(i);
  }
  return v;
}

// CPU channel slice from contiguous NCHW.
auto CpuSliceChannels(const std::vector<float>& in, int N, int C, int H, int W, int start,
                      int count) -> std::vector<float> {
  std::vector<float> out(static_cast<std::size_t>(N) * count * H * W);
  for (int n = 0; n < N; ++n) {
    for (int c = 0; c < count; ++c) {
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          const int src_c = start + c;
          const std::size_t ii =
              static_cast<std::size_t>(((n * C + src_c) * H + y) * W + x);
          const std::size_t io =
              static_cast<std::size_t>(((n * count + c) * H + y) * W + x);
          out[io] = in[ii];
        }
      }
    }
  }
  return out;
}

}  // namespace

class MlOpsSliceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsSliceTest, ContiguousViewIsZeroCopy) {
  constexpr int N = 1;
  constexpr int C = 8;
  constexpr int H = 4;
  constexpr int W = 5;
  const auto host = FillIota(static_cast<std::size_t>(N * C * H * W));

  cuda::nn::DeviceBufferF32 d(host.size());
  d.Upload(host);
  auto full = d.AsTensor({N, C, H, W});

  auto view = cuda::nn::SliceChannelsView(full, 2, 3);
  EXPECT_EQ(view.rank, 4);
  EXPECT_EQ(view.shape[0], N);
  EXPECT_EQ(view.shape[1], 3);
  EXPECT_EQ(view.shape[2], H);
  EXPECT_EQ(view.shape[3], W);
  // N=1 contiguous channel slice starts at offset 2*H*W.
  EXPECT_EQ(view.data, d.get() + 2 * H * W);
  EXPECT_TRUE(view.IsContiguous());  // N=1 → contiguous block

  // Download via a materializing slice and compare.
  cuda::nn::DeviceBufferF32 dout(static_cast<std::size_t>(N * 3 * H * W));
  auto to = dout.AsTensor({N, 3, H, W});
  cuda::nn::SliceChannels(full, to, 2, 3);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto expected = CpuSliceChannels(host, N, C, H, W, 2, 3);
  const auto got      = dout.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

TEST_F(MlOpsSliceTest, BayerStyleSplit128To64Plus64) {
  constexpr int N = 1;
  constexpr int C = 128;
  constexpr int H = 6;
  constexpr int W = 7;
  const auto host = FillIota(static_cast<std::size_t>(N * C * H * W), -10.0f);

  cuda::nn::DeviceBufferF32 d(host.size());
  d.Upload(host);
  auto full = d.AsTensor({N, C, H, W});

  auto [filters, masks] = cuda::nn::SplitChannelsView(full, 64);
  EXPECT_EQ(filters.shape[1], 64);
  EXPECT_EQ(masks.shape[1], 64);
  EXPECT_EQ(filters.data, d.get());
  EXPECT_EQ(masks.data, d.get() + 64 * H * W);
  EXPECT_TRUE(filters.IsContiguous());
  EXPECT_TRUE(masks.IsContiguous());

  // Mul of the two halves (Bayer residual path pattern).
  cuda::nn::DeviceBufferF32 dprod(static_cast<std::size_t>(N * 64 * H * W));
  auto tprod = dprod.AsTensor({N, 64, H, W});
  cuda::nn::Mul(filters, masks, tprod);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = dprod.Download();
  for (int c = 0; c < 64; ++c) {
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        const std::size_t i0 =
            static_cast<std::size_t>((c * H + y) * W + x);
        const std::size_t i1 =
            static_cast<std::size_t>(((64 + c) * H + y) * W + x);
        EXPECT_FLOAT_EQ(got[i0], host[i0] * host[i1]);
      }
    }
  }
}

TEST_F(MlOpsSliceTest, SplitChannelsPreferView) {
  constexpr int N = 1;
  constexpr int C = 10;
  constexpr int H = 2;
  constexpr int W = 3;
  cuda::nn::DeviceBufferF32 d(static_cast<std::size_t>(N * C * H * W));
  d.Upload(FillIota(d.size()));
  auto full = d.AsTensor({N, C, H, W});

  cuda::nn::DeviceTensor first;
  cuda::nn::DeviceTensor second;
  cuda::nn::SplitChannels(full, first, second, 4, nullptr, /*prefer_view=*/true);
  EXPECT_EQ(first.shape[1], 4);
  EXPECT_EQ(second.shape[1], 6);
  EXPECT_EQ(first.data, d.get());
  EXPECT_EQ(second.data, d.get() + 4 * H * W);
}

TEST_F(MlOpsSliceTest, MaterializingSplitWithoutView) {
  constexpr int N = 2;
  constexpr int C = 6;
  constexpr int H = 3;
  constexpr int W = 4;
  const auto host = FillIota(static_cast<std::size_t>(N * C * H * W), 0.5f);

  cuda::nn::DeviceBufferF32 d(host.size());
  d.Upload(host);
  auto full = d.AsTensor({N, C, H, W});

  cuda::nn::DeviceBufferF32 d0(static_cast<std::size_t>(N * 2 * H * W));
  cuda::nn::DeviceBufferF32 d1(static_cast<std::size_t>(N * 4 * H * W));
  auto t0 = d0.AsTensor({N, 2, H, W});
  auto t1 = d1.AsTensor({N, 4, H, W});

  cuda::nn::SplitChannels(full, t0, t1, 2, nullptr, /*prefer_view=*/false);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto e0 = CpuSliceChannels(host, N, C, H, W, 0, 2);
  const auto e1 = CpuSliceChannels(host, N, C, H, W, 2, 4);
  const auto g0 = d0.Download();
  const auto g1 = d1.Download();
  for (std::size_t i = 0; i < e0.size(); ++i) {
    EXPECT_FLOAT_EQ(g0[i], e0[i]);
  }
  for (std::size_t i = 0; i < e1.size(); ++i) {
    EXPECT_FLOAT_EQ(g1[i], e1[i]);
  }
}

TEST_F(MlOpsSliceTest, MultiBatchViewHasCorrectStrides) {
  // N>1: channel mid-slice is not a single contiguous region.
  constexpr int N = 2;
  constexpr int C = 4;
  constexpr int H = 2;
  constexpr int W = 2;
  const auto host = FillIota(static_cast<std::size_t>(N * C * H * W));

  cuda::nn::DeviceBufferF32 d(host.size());
  d.Upload(host);
  auto full = d.AsTensor({N, C, H, W});

  auto view = cuda::nn::SliceChannelsView(full, 1, 2);
  EXPECT_EQ(view.shape[0], N);
  EXPECT_EQ(view.shape[1], 2);
  // strides: batch still spans full C
  EXPECT_EQ(view.strides[0], C * H * W);
  EXPECT_EQ(view.strides[1], H * W);
  // Not contiguous for N>1 mid-slice.
  EXPECT_FALSE(view.IsContiguous());

  // Materialize and check values.
  cuda::nn::DeviceBufferF32 dout(static_cast<std::size_t>(N * 2 * H * W));
  auto to = dout.AsTensor({N, 2, H, W});
  cuda::nn::SliceChannels(full, to, 1, 2);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  const auto expected = CpuSliceChannels(host, N, C, H, W, 1, 2);
  const auto got      = dout.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

TEST_F(MlOpsSliceTest, RejectsOutOfRange) {
  cuda::nn::DeviceBufferF32 d(1 * 4 * 2 * 2);
  auto t = d.AsTensor({1, 4, 2, 2});
  EXPECT_THROW(cuda::nn::SliceChannelsView(t, 3, 2), std::runtime_error);
  EXPECT_THROW(cuda::nn::SplitChannelsView(t, 5), std::runtime_error);
}

TEST_F(MlOpsSliceTest, RejectsNonContiguousForView) {
  cuda::nn::DeviceBufferF32 d(1 * 4 * 2 * 2);
  cuda::nn::DeviceTensor t;
  t.data       = d.get();
  t.rank       = 4;
  t.shape[0]   = 1;
  t.shape[1]   = 4;
  t.shape[2]   = 2;
  t.shape[3]   = 2;
  t.strides[0] = 16;
  t.strides[1] = 4;
  t.strides[2] = 2;
  t.strides[3] = 2;  // non-unit W stride → non-contiguous
  EXPECT_FALSE(t.IsContiguous());
  EXPECT_THROW(cuda::nn::SliceChannelsView(t, 0, 2), std::runtime_error);
}

}  // namespace alcedo
