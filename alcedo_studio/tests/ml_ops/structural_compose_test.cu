//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Composition tests: crop + concat as used by demosaicnet Bayer / XTrans skip paths,
// and split + mul as used by the Bayer residual branch. Keeps op-specific files smaller.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/concat.hpp"
#include "cuda/nn/crop.hpp"
#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/layout.hpp"
#include "cuda/nn/mul.hpp"
#include "cuda/nn/slice.hpp"
#include "cuda/nn/tensor.hpp"
#include "cuda/nn/workspace.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

auto FillIota(std::size_t n, float start = 0.0f) -> std::vector<float> {
  std::vector<float> v(n);
  for (std::size_t i = 0; i < n; ++i) {
    v[i] = start + static_cast<float>(i) * 0.01f;
  }
  return v;
}

auto CpuCenterCrop(const std::vector<float>& in, int N, int C, int src_h, int src_w, int tgt_h,
                   int tgt_w) -> std::vector<float> {
  const int crop_t = (src_h - tgt_h) / 2;
  const int crop_l = (src_w - tgt_w) / 2;
  std::vector<float> out(static_cast<std::size_t>(N) * C * tgt_h * tgt_w);
  for (int n = 0; n < N; ++n) {
    for (int c = 0; c < C; ++c) {
      for (int y = 0; y < tgt_h; ++y) {
        for (int x = 0; x < tgt_w; ++x) {
          const std::size_t ii = static_cast<std::size_t>(
              ((n * C + c) * src_h + (y + crop_t)) * src_w + (x + crop_l));
          const std::size_t io =
              static_cast<std::size_t>(((n * C + c) * tgt_h + y) * tgt_w + x);
          out[io] = in[ii];
        }
      }
    }
  }
  return out;
}

auto CpuConcat(const std::vector<float>& a, int N, int Ca, int H, int W,
               const std::vector<float>& b, int Cb) -> std::vector<float> {
  const int Cout = Ca + Cb;
  std::vector<float> out(static_cast<std::size_t>(N) * Cout * H * W);
  for (int n = 0; n < N; ++n) {
    for (int c = 0; c < Ca; ++c) {
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          out[static_cast<std::size_t>(((n * Cout + c) * H + y) * W + x)] =
              a[static_cast<std::size_t>(((n * Ca + c) * H + y) * W + x)];
        }
      }
    }
    for (int c = 0; c < Cb; ++c) {
      for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
          out[static_cast<std::size_t>(((n * Cout + Ca + c) * H + y) * W + x)] =
              b[static_cast<std::size_t>(((n * Cb + c) * H + y) * W + x)];
        }
      }
    }
  }
  return out;
}

}  // namespace

class MlOpsStructuralComposeTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

// Bayer / XTrans full-res skip: crop mosaic to up.spatial, then concat channels.
TEST_F(MlOpsStructuralComposeTest, CropThenConcatBayerStyle) {
  constexpr int N = 1;
  constexpr int C_mosaic = 3;
  constexpr int C_up     = 3;
  constexpr int src_h = 20;
  constexpr int src_w = 24;
  // After valid conv stack + upsample, up is smaller than mosaic.
  constexpr int up_h = 12;
  constexpr int up_w = 14;

  const auto mosaic_h = FillIota(static_cast<std::size_t>(N * C_mosaic * src_h * src_w), 0.0f);
  const auto up_hbuf  = FillIota(static_cast<std::size_t>(N * C_up * up_h * up_w), 100.0f);

  const auto cropped_cpu = CpuCenterCrop(mosaic_h, N, C_mosaic, src_h, src_w, up_h, up_w);
  const auto cat_cpu     = CpuConcat(cropped_cpu, N, C_mosaic, up_h, up_w, up_hbuf, C_up);

  cuda::nn::DeviceBufferF32 d_mosaic(mosaic_h.size());
  cuda::nn::DeviceBufferF32 d_up(up_hbuf.size());
  cuda::nn::DeviceBufferF32 d_crop(cropped_cpu.size());
  cuda::nn::DeviceBufferF32 d_cat(cat_cpu.size());
  d_mosaic.Upload(mosaic_h);
  d_up.Upload(up_hbuf);

  auto t_mosaic = d_mosaic.AsTensor({N, C_mosaic, src_h, src_w});
  auto t_up     = d_up.AsTensor({N, C_up, up_h, up_w});
  auto t_crop   = d_crop.AsTensor({N, C_mosaic, up_h, up_w});
  auto t_cat    = d_cat.AsTensor({N, C_mosaic + C_up, up_h, up_w});

  cuda::nn::CenterCropLike(t_mosaic, t_up, t_crop);
  cuda::nn::ConcatChannels(t_crop, t_up, t_cat);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_cat.Download();
  ASSERT_EQ(got.size(), cat_cpu.size());
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], cat_cpu[i]) << "i=" << i;
  }
}

// XTrans: crop mosaic [3] to match h spatial, concat with 64-ch features → 67.
TEST_F(MlOpsStructuralComposeTest, CropThenConcatXTransStyle) {
  constexpr int N = 1;
  constexpr int C_mosaic = 3;
  constexpr int C_feat   = 64;
  constexpr int src_h = 32;
  constexpr int src_w = 32;
  // 11 valid 3×3 → shrink by 22.
  constexpr int feat_h = 10;
  constexpr int feat_w = 10;

  const auto mosaic_h = FillIota(static_cast<std::size_t>(N * C_mosaic * src_h * src_w), 1.0f);
  const auto feat_hbuf =
      FillIota(static_cast<std::size_t>(N * C_feat * feat_h * feat_w), -2.0f);

  const auto cropped_cpu = CpuCenterCrop(mosaic_h, N, C_mosaic, src_h, src_w, feat_h, feat_w);
  const auto cat_cpu = CpuConcat(cropped_cpu, N, C_mosaic, feat_h, feat_w, feat_hbuf, C_feat);
  EXPECT_EQ(static_cast<int>(cat_cpu.size() / (feat_h * feat_w)), C_mosaic + C_feat);

  cuda::nn::DeviceBufferF32 d_mosaic(mosaic_h.size());
  cuda::nn::DeviceBufferF32 d_feat(feat_hbuf.size());
  cuda::nn::DeviceBufferF32 d_crop(cropped_cpu.size());
  cuda::nn::DeviceBufferF32 d_cat(cat_cpu.size());
  d_mosaic.Upload(mosaic_h);
  d_feat.Upload(feat_hbuf);

  auto t_mosaic = d_mosaic.AsTensor({N, C_mosaic, src_h, src_w});
  auto t_feat   = d_feat.AsTensor({N, C_feat, feat_h, feat_w});
  auto t_crop   = d_crop.AsTensor({N, C_mosaic, feat_h, feat_w});
  auto t_cat    = d_cat.AsTensor({N, C_mosaic + C_feat, feat_h, feat_w});

  cuda::nn::CenterCropSpatial(t_mosaic, t_crop, feat_h, feat_w);
  cuda::nn::ConcatChannels(t_crop, t_feat, t_cat);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_cat.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], cat_cpu[i]);
  }
}

// Bayer residual: split 128 → 64+64 (views), mul filters ⊙ masks.
TEST_F(MlOpsStructuralComposeTest, SplitThenMulBayerResidual) {
  constexpr int N = 1;
  constexpr int C = 128;
  constexpr int H = 8;
  constexpr int W = 9;
  const auto host = FillIota(static_cast<std::size_t>(N * C * H * W), -1.0f);

  cuda::nn::DeviceBufferF32 d(host.size());
  d.Upload(host);
  auto full = d.AsTensor({N, C, H, W});

  cuda::nn::DeviceTensor filters;
  cuda::nn::DeviceTensor masks;
  cuda::nn::SplitChannels(full, filters, masks, 64);

  cuda::nn::DeviceBufferF32 d_out(static_cast<std::size_t>(N * 64 * H * W));
  auto t_out = d_out.AsTensor({N, 64, H, W});
  cuda::nn::Mul(filters, masks, t_out);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  const auto got = d_out.Download();
  for (int c = 0; c < 64; ++c) {
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        const std::size_t i0 = static_cast<std::size_t>((c * H + y) * W + x);
        const std::size_t i1 = static_cast<std::size_t>(((64 + c) * H + y) * W + x);
        EXPECT_FLOAT_EQ(got[i0], host[i0] * host[i1]);
      }
    }
  }
}

// Workspace-backed intermediates: crop + concat without DeviceBuffer for scratch.
TEST_F(MlOpsStructuralComposeTest, CropConcatViaWorkspacePool) {
  constexpr int N = 1;
  constexpr int C = 3;
  constexpr int src_h = 16;
  constexpr int src_w = 16;
  constexpr int tgt_h = 8;
  constexpr int tgt_w = 8;

  const auto mosaic = FillIota(static_cast<std::size_t>(N * C * src_h * src_w));
  const auto up     = FillIota(static_cast<std::size_t>(N * C * tgt_h * tgt_w), 50.0f);
  const auto crop_e = CpuCenterCrop(mosaic, N, C, src_h, src_w, tgt_h, tgt_w);
  const auto cat_e  = CpuConcat(crop_e, N, C, tgt_h, tgt_w, up, C);

  cuda::nn::DeviceBufferF32 d_mosaic(mosaic.size());
  cuda::nn::DeviceBufferF32 d_up(up.size());
  d_mosaic.Upload(mosaic);
  d_up.Upload(up);
  auto t_mosaic = d_mosaic.AsTensor({N, C, src_h, src_w});
  auto t_up     = d_up.AsTensor({N, C, tgt_h, tgt_w});

  cuda::nn::WorkspacePool pool;
  pool.Reserve((crop_e.size() + cat_e.size()) * sizeof(float) + 1024);
  {
    cuda::nn::WorkspaceScope scope(pool);
    auto t_crop = pool.AllocateTensor({N, C, tgt_h, tgt_w});
    auto t_cat  = pool.AllocateTensor({N, 2 * C, tgt_h, tgt_w});

    cuda::nn::CenterCropLike(t_mosaic, t_up, t_crop);
    cuda::nn::ConcatChannels(t_crop, t_up, t_cat);
    ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

    std::vector<float> got(cat_e.size());
    ASSERT_EQ(::cudaMemcpy(got.data(), t_cat.data, got.size() * sizeof(float),
                           cudaMemcpyDeviceToHost),
              cudaSuccess);
    for (std::size_t i = 0; i < got.size(); ++i) {
      EXPECT_FLOAT_EQ(got[i], cat_e[i]);
    }
  }
  EXPECT_EQ(pool.used_bytes(), 0U);
}

// Boundary layout: pack HWC → NCHW, structural crop on NCHW, unpack.
TEST_F(MlOpsStructuralComposeTest, LayoutCropLayoutRoundTripSpatial) {
  constexpr int H = 10;
  constexpr int W = 10;
  constexpr int C = 3;
  constexpr int tgt_h = 6;
  constexpr int tgt_w = 6;

  std::vector<float> hwc(static_cast<std::size_t>(H * W * C));
  for (std::size_t i = 0; i < hwc.size(); ++i) {
    hwc[i] = static_cast<float>(i);
  }

  cuda::nn::DeviceBufferF32 d_hwc(hwc.size());
  cuda::nn::DeviceBufferF32 d_nchw(static_cast<std::size_t>(C * H * W));
  cuda::nn::DeviceBufferF32 d_crop(static_cast<std::size_t>(C * tgt_h * tgt_w));
  cuda::nn::DeviceBufferF32 d_out_hwc(static_cast<std::size_t>(tgt_h * tgt_w * C));
  d_hwc.Upload(hwc);

  auto t_hwc  = d_hwc.AsTensor({1, H, W, C});
  auto t_nchw = d_nchw.AsTensor({1, C, H, W});
  auto t_crop = d_crop.AsTensor({1, C, tgt_h, tgt_w});
  auto t_out  = d_out_hwc.AsTensor({1, tgt_h, tgt_w, C});

  cuda::nn::PackHwcToNchw(t_hwc, t_nchw);
  cuda::nn::CenterCropSpatial(t_nchw, t_crop, tgt_h, tgt_w);
  cuda::nn::UnpackNchwToHwc(t_crop, t_out);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  // CPU: pack → crop → unpack
  std::vector<float> nchw(static_cast<std::size_t>(C * H * W));
  for (int c = 0; c < C; ++c) {
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        nchw[static_cast<std::size_t>((c * H + y) * W + x)] =
            hwc[static_cast<std::size_t>((y * W + x) * C + c)];
      }
    }
  }
  const auto cropped = CpuCenterCrop(nchw, 1, C, H, W, tgt_h, tgt_w);
  std::vector<float> expected(static_cast<std::size_t>(tgt_h * tgt_w * C));
  for (int c = 0; c < C; ++c) {
    for (int y = 0; y < tgt_h; ++y) {
      for (int x = 0; x < tgt_w; ++x) {
        expected[static_cast<std::size_t>((y * tgt_w + x) * C + c)] =
            cropped[static_cast<std::size_t>((c * tgt_h + y) * tgt_w + x)];
      }
    }
  }

  const auto got = d_out_hwc.Download();
  for (std::size_t i = 0; i < got.size(); ++i) {
    EXPECT_FLOAT_EQ(got[i], expected[i]);
  }
}

}  // namespace alcedo
