//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/crop.hpp"
#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

// Exact demosaicnet / PyTorch center-crop on contiguous NCHW host buffer.
auto CpuCenterCrop(const std::vector<float>& in, int N, int C, int src_h, int src_w, int tgt_h,
                   int tgt_w) -> std::vector<float> {
  const int crop_h = src_h - tgt_h;
  const int crop_t = crop_h / 2;
  const int crop_w = src_w - tgt_w;
  const int crop_l = crop_w / 2;

  std::vector<float> out(static_cast<std::size_t>(N) * C * tgt_h * tgt_w);
  for (int n = 0; n < N; ++n) {
    for (int c = 0; c < C; ++c) {
      for (int y = 0; y < tgt_h; ++y) {
        for (int x = 0; x < tgt_w; ++x) {
          const int sy = y + crop_t;
          const int sx = x + crop_l;
          const std::size_t ii =
              static_cast<std::size_t>(((n * C + c) * src_h + sy) * src_w + sx);
          const std::size_t io =
              static_cast<std::size_t>(((n * C + c) * tgt_h + y) * tgt_w + x);
          out[io] = in[ii];
        }
      }
    }
  }
  return out;
}

auto FillIota(std::size_t n) -> std::vector<float> {
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = static_cast<float>(i);
  }
  return v;
}

}  // namespace

class MlOpsCropTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsCropTest, ComputeOffsetsMatchDemosaicnet) {
  // Even residual: equal margins.
  auto o = cuda::nn::ComputeCenterCropOffsets(10, 10, 6, 6);
  EXPECT_EQ(o.top, 2);
  EXPECT_EQ(o.left, 2);

  // Odd residual: floor on top/left, remainder on bottom/right.
  // crop_h = 5, crop_t = 2, crop_b = 3
  o = cuda::nn::ComputeCenterCropOffsets(11, 9, 6, 4);
  EXPECT_EQ(o.top, 2);   // 5/2
  EXPECT_EQ(o.left, 2);  // 5/2
  // bottom = 11-6-2 = 3, right = 9-4-2 = 3

  o = cuda::nn::ComputeCenterCropOffsets(8, 8, 8, 8);
  EXPECT_EQ(o.top, 0);
  EXPECT_EQ(o.left, 0);

  EXPECT_THROW(cuda::nn::ComputeCenterCropOffsets(4, 4, 5, 4), std::runtime_error);
}

TEST_F(MlOpsCropTest, MaterializeMatchesCpuOddSizes) {
  constexpr int N = 1;
  constexpr int C = 3;
  constexpr int src_h = 11;
  constexpr int src_w = 9;
  constexpr int tgt_h = 6;
  constexpr int tgt_w = 4;
  const auto host = FillIota(static_cast<std::size_t>(N * C * src_h * src_w));
  const auto expected = CpuCenterCrop(host, N, C, src_h, src_w, tgt_h, tgt_w);

  cuda::nn::DeviceBufferF32 din(host.size());
  cuda::nn::DeviceBufferF32 dout(expected.size());
  din.Upload(host);
  auto tin  = din.AsTensor({N, C, src_h, src_w});
  auto tout = dout.AsTensor({N, C, tgt_h, tgt_w});

  cuda::nn::CenterCropSpatial(tin, tout, tgt_h, tgt_w);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = dout.Download();
  ASSERT_EQ(got.size(), expected.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]) << "i=" << i;
  }
}

TEST_F(MlOpsCropTest, CenterCropLikeUsesRefSpatial) {
  constexpr int N = 1;
  constexpr int C = 3;
  constexpr int src_h = 16;
  constexpr int src_w = 20;
  constexpr int tgt_h = 10;
  constexpr int tgt_w = 12;
  const auto host = FillIota(static_cast<std::size_t>(N * C * src_h * src_w));
  const auto expected = CpuCenterCrop(host, N, C, src_h, src_w, tgt_h, tgt_w);

  cuda::nn::DeviceBufferF32 din(host.size());
  cuda::nn::DeviceBufferF32 dref(static_cast<std::size_t>(N * 64 * tgt_h * tgt_w));
  cuda::nn::DeviceBufferF32 dout(expected.size());
  din.Upload(host);
  auto tin  = din.AsTensor({N, C, src_h, src_w});
  auto tref = dref.AsTensor({N, 64, tgt_h, tgt_w});
  auto tout = dout.AsTensor({N, C, tgt_h, tgt_w});

  cuda::nn::CenterCropLike(tin, tref, tout);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = dout.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

TEST_F(MlOpsCropTest, IdentityCropIsMemcpy) {
  constexpr int N = 1;
  constexpr int C = 2;
  constexpr int H = 5;
  constexpr int W = 7;
  const auto host = FillIota(static_cast<std::size_t>(N * C * H * W));

  cuda::nn::DeviceBufferF32 din(host.size());
  cuda::nn::DeviceBufferF32 dout(host.size());
  din.Upload(host);
  auto tin  = din.AsTensor({N, C, H, W});
  auto tout = dout.AsTensor({N, C, H, W});

  cuda::nn::CenterCropSpatial(tin, tout, H, W);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);
  const auto got = dout.Download();
  for (std::size_t i = 0; i < host.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], host[i]);
  }
}

TEST_F(MlOpsCropTest, ViewMatchesMaterialized) {
  constexpr int N = 1;
  constexpr int C = 2;
  constexpr int src_h = 8;
  constexpr int src_w = 8;
  constexpr int tgt_h = 4;
  constexpr int tgt_w = 6;
  const auto host = FillIota(static_cast<std::size_t>(N * C * src_h * src_w));

  cuda::nn::DeviceBufferF32 din(host.size());
  din.Upload(host);
  auto tin = din.AsTensor({N, C, src_h, src_w});

  auto view = cuda::nn::CenterCropSpatialView(tin, tgt_h, tgt_w);
  EXPECT_EQ(view.shape[2], tgt_h);
  EXPECT_EQ(view.shape[3], tgt_w);
  // With left crop > 0, W view is not contiguous.
  EXPECT_FALSE(view.IsContiguous());

  cuda::nn::DeviceBufferF32 dout(static_cast<std::size_t>(N * C * tgt_h * tgt_w));
  auto tout = dout.AsTensor({N, C, tgt_h, tgt_w});
  cuda::nn::CenterCropSpatial(tin, tout, tgt_h, tgt_w);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  // Sample a few elements from the view via materialize of a 1-element... easier:
  // just compare known offsets on host.
  const auto expected = CpuCenterCrop(host, N, C, src_h, src_w, tgt_h, tgt_w);
  const auto got      = dout.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }

  // View data pointer should be base + top*stride_h + left*stride_w
  const auto offs = cuda::nn::ComputeCenterCropOffsets(src_h, src_w, tgt_h, tgt_w);
  EXPECT_EQ(view.data, din.get() + offs.top * src_w + offs.left);
}

TEST_F(MlOpsCropTest, MultiBatch) {
  constexpr int N = 2;
  constexpr int C = 3;
  constexpr int src_h = 9;
  constexpr int src_w = 9;
  constexpr int tgt_h = 5;
  constexpr int tgt_w = 5;
  const auto host = FillIota(static_cast<std::size_t>(N * C * src_h * src_w));
  const auto expected = CpuCenterCrop(host, N, C, src_h, src_w, tgt_h, tgt_w);

  cuda::nn::DeviceBufferF32 din(host.size());
  cuda::nn::DeviceBufferF32 dout(expected.size());
  din.Upload(host);
  auto tin  = din.AsTensor({N, C, src_h, src_w});
  auto tout = dout.AsTensor({N, C, tgt_h, tgt_w});
  cuda::nn::CenterCropSpatial(tin, tout, tgt_h, tgt_w);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = dout.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

TEST_F(MlOpsCropTest, RejectsTargetLargerThanSource) {
  cuda::nn::DeviceBufferF32 din(1 * 1 * 4 * 4);
  cuda::nn::DeviceBufferF32 dout(1 * 1 * 5 * 4);
  auto tin  = din.AsTensor({1, 1, 4, 4});
  auto tout = dout.AsTensor({1, 1, 5, 4});
  EXPECT_THROW(cuda::nn::CenterCropSpatial(tin, tout, 5, 4), std::runtime_error);
}

}  // namespace alcedo
