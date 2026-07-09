//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/tensor.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

}  // namespace

class MlOpsDeviceBufferTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsDeviceBufferTest, EmptyBufferIsSafe) {
  cuda::nn::DeviceBuffer<float> buf;
  EXPECT_TRUE(buf.empty());
  EXPECT_EQ(buf.size(), 0U);
  EXPECT_EQ(buf.get(), nullptr);
  buf.Upload(std::vector<float>{});
  EXPECT_TRUE(buf.Download().empty());
  buf.FillZero();
  buf.Reset();
}

TEST_F(MlOpsDeviceBufferTest, UploadDownloadRoundTrip) {
  const std::vector<float> host_in = {-1.5f, 0.0f, 2.25f, 3.0f, -8.0f};
  cuda::nn::DeviceBufferF32 d(host_in.size());
  ASSERT_EQ(d.size(), host_in.size());
  ASSERT_FALSE(d.empty());

  d.Upload(host_in);
  const auto host_out = d.Download();
  ASSERT_EQ(host_out.size(), host_in.size());
  for (std::size_t i = 0; i < host_in.size(); ++i) {
    EXPECT_FLOAT_EQ(host_out[i], host_in[i]);
  }
}

TEST_F(MlOpsDeviceBufferTest, AsyncUploadDownload) {
  const std::vector<float> host_in = {1.0f, 2.0f, 3.0f, 4.0f};
  cuda::nn::DeviceBuffer<float> d(host_in.size());

  cudaStream_t stream = nullptr;
  ASSERT_EQ(::cudaStreamCreate(&stream), cudaSuccess);

  d.Upload(host_in, stream);
  std::vector<float> host_out(host_in.size(), -99.0f);
  d.Download(host_out.data(), host_out.size(), stream);
  ASSERT_EQ(::cudaStreamSynchronize(stream), cudaSuccess);
  ::cudaStreamDestroy(stream);

  for (std::size_t i = 0; i < host_in.size(); ++i) {
    EXPECT_FLOAT_EQ(host_out[i], host_in[i]);
  }
}

TEST_F(MlOpsDeviceBufferTest, FillZero) {
  cuda::nn::DeviceBuffer<float> d(16);
  d.Upload(std::vector<float>(16, 7.0f));
  d.FillZero();
  const auto got = d.Download();
  for (float v : got) {
    EXPECT_FLOAT_EQ(v, 0.0f);
  }
}

TEST_F(MlOpsDeviceBufferTest, MoveTransfersOwnership) {
  cuda::nn::DeviceBuffer<float> a(4);
  a.Upload({1.0f, 2.0f, 3.0f, 4.0f});
  float* raw = a.get();

  cuda::nn::DeviceBuffer<float> b(std::move(a));
  EXPECT_EQ(a.get(), nullptr);
  EXPECT_EQ(a.size(), 0U);
  EXPECT_EQ(b.get(), raw);
  EXPECT_EQ(b.size(), 4U);
  EXPECT_FLOAT_EQ(b.Download()[2], 3.0f);

  cuda::nn::DeviceBuffer<float> c;
  c = std::move(b);
  EXPECT_EQ(b.get(), nullptr);
  EXPECT_EQ(c.get(), raw);
  EXPECT_EQ(c.size(), 4U);
}

TEST_F(MlOpsDeviceBufferTest, AsTensorMatchesShape) {
  cuda::nn::DeviceBuffer<float> d(2 * 3 * 4);
  d.Upload(std::vector<float>(d.size(), 1.0f));

  auto tensor = d.AsTensor({2, 3, 4});
  EXPECT_EQ(tensor.rank, 3);
  EXPECT_EQ(tensor.shape[0], 2);
  EXPECT_EQ(tensor.shape[1], 3);
  EXPECT_EQ(tensor.shape[2], 4);
  EXPECT_EQ(tensor.data, d.get());
  EXPECT_TRUE(tensor.IsContiguous());
  EXPECT_EQ(tensor.Numel(), static_cast<std::int64_t>(d.size()));
}

TEST_F(MlOpsDeviceBufferTest, AsTensorRejectsMismatchedNumel) {
  cuda::nn::DeviceBuffer<float> d(8);
  EXPECT_THROW((void)d.AsTensor({2, 2}), std::runtime_error);
}

TEST_F(MlOpsDeviceBufferTest, UploadSizeMismatchThrows) {
  cuda::nn::DeviceBuffer<float> d(4);
  EXPECT_THROW(d.Upload(std::vector<float>{1.0f, 2.0f}), std::runtime_error);
}

}  // namespace alcedo
