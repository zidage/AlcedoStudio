//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include <cuda_runtime.h>

#include "cuda/nn/device_buffer.hpp"
#include "cuda/nn/relu.hpp"
#include "cuda/nn/workspace.hpp"

namespace alcedo {
namespace {

auto HasCudaDevice() -> bool {
  int count = 0;
  return ::cudaGetDeviceCount(&count) == cudaSuccess && count > 0;
}

}  // namespace

class MlOpsWorkspaceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasCudaDevice()) {
      GTEST_SKIP() << "No CUDA device available.";
    }
  }
};

TEST_F(MlOpsWorkspaceTest, ReserveAndAllocateWithinCapacity) {
  cuda::nn::WorkspacePool pool;
  pool.Reserve(1024);
  EXPECT_GE(pool.capacity_bytes(), 1024U);
  EXPECT_EQ(pool.used_bytes(), 0U);

  void* a = pool.Allocate(128);
  void* b = pool.Allocate(64);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_NE(a, b);
  EXPECT_GT(pool.used_bytes(), 0U);
  EXPECT_GE(pool.remaining_bytes(), 0U);

  // Pointers lie inside the slab.
  const auto* base = static_cast<const char*>(pool.base());
  const auto* pa   = static_cast<const char*>(a);
  const auto* pb   = static_cast<const char*>(b);
  EXPECT_GE(pa, base);
  EXPECT_LT(pa, base + pool.capacity_bytes());
  EXPECT_GE(pb, base);
  EXPECT_LT(pb, base + pool.capacity_bytes());
}

TEST_F(MlOpsWorkspaceTest, ResetAllowsReuseWithoutMalloc) {
  cuda::nn::WorkspacePool pool(4096);
  const std::size_t cap0 = pool.capacity_bytes();
  void* first            = pool.Allocate(512);
  ASSERT_NE(first, nullptr);
  pool.Reset();
  EXPECT_EQ(pool.used_bytes(), 0U);
  void* second = pool.Allocate(512);
  // After Reset, bump starts at base again (alignment may place at base).
  EXPECT_EQ(second, first);
  EXPECT_EQ(pool.capacity_bytes(), cap0);
}

TEST_F(MlOpsWorkspaceTest, EmptyPoolGrowsOnFirstAllocate) {
  cuda::nn::WorkspacePool pool;
  EXPECT_EQ(pool.capacity_bytes(), 0U);
  float* ptr = pool.AllocateFloats(1000);
  ASSERT_NE(ptr, nullptr);
  EXPECT_GE(pool.capacity_bytes(), 1000U * sizeof(float));
  EXPECT_GE(pool.used_bytes(), 1000U * sizeof(float));
}

TEST_F(MlOpsWorkspaceTest, AllocateWithLiveDataDoesNotGrowSilently) {
  cuda::nn::WorkspacePool pool;
  pool.Reserve(256);
  void* live = pool.Allocate(128);
  ASSERT_NE(live, nullptr);
  // Request more than remaining capacity while offset_ != 0.
  EXPECT_THROW((void)pool.Allocate(pool.capacity_bytes()), std::runtime_error);
}

TEST_F(MlOpsWorkspaceTest, ReserveWhileLiveThrows) {
  cuda::nn::WorkspacePool pool(512);
  (void)pool.Allocate(64);
  EXPECT_THROW(pool.Reserve(pool.capacity_bytes() + 1), std::runtime_error);
}

TEST_F(MlOpsWorkspaceTest, WorkspaceScopeRewindsLifo) {
  cuda::nn::WorkspacePool pool(4096);
  void* outer = pool.Allocate(100);
  ASSERT_NE(outer, nullptr);
  const std::size_t mark_after_outer = pool.used_bytes();

  {
    cuda::nn::WorkspaceScope scope(pool);
    void* inner1 = pool.Allocate(200);
    void* inner2 = pool.Allocate(50);
    ASSERT_NE(inner1, nullptr);
    ASSERT_NE(inner2, nullptr);
    EXPECT_GT(pool.used_bytes(), mark_after_outer);
  }

  EXPECT_EQ(pool.used_bytes(), mark_after_outer);
  // Outer allocation still "live" from the pool's perspective (same bump region).
  void* outer2 = pool.Allocate(100);
  ASSERT_NE(outer2, nullptr);
}

TEST_F(MlOpsWorkspaceTest, AllocateTensorShape) {
  cuda::nn::WorkspacePool pool(64 * 1024);
  auto tensor = pool.AllocateTensor({1, 16, 8, 8});
  EXPECT_EQ(tensor.rank, 4);
  EXPECT_EQ(tensor.shape[0], 1);
  EXPECT_EQ(tensor.shape[1], 16);
  EXPECT_EQ(tensor.shape[2], 8);
  EXPECT_EQ(tensor.shape[3], 8);
  EXPECT_EQ(tensor.Numel(), 1 * 16 * 8 * 8);
  EXPECT_TRUE(tensor.IsContiguous());
  ASSERT_NE(tensor.data, nullptr);
}

TEST_F(MlOpsWorkspaceTest, AlignmentIsHonored) {
  cuda::nn::WorkspacePool pool(4096);
  // Force a non-aligned offset first with a small unaligned-looking request.
  (void)pool.Allocate(1, /*alignment=*/1);
  constexpr std::size_t kAlign = 256;
  void* aligned                = pool.Allocate(32, kAlign);
  ASSERT_NE(aligned, nullptr);
  const auto addr = reinterpret_cast<std::uintptr_t>(aligned);
  EXPECT_EQ(addr % kAlign, 0U);
}

TEST_F(MlOpsWorkspaceTest, ReluThroughWorkspaceTensor) {
  // End-to-end smoke: DeviceBuffer (owned input) + WorkspacePool (activation) + ReLU.
  constexpr std::int64_t n = 1;
  constexpr std::int64_t c = 8;
  constexpr std::int64_t h = 4;
  constexpr std::int64_t w = 4;
  const std::size_t      numel = static_cast<std::size_t>(n * c * h * w);

  std::vector<float> host_in(numel);
  for (std::size_t i = 0; i < numel; ++i) {
    host_in[i] = static_cast<float>(static_cast<int>(i) - 32);
  }

  cuda::nn::DeviceBufferF32 d_in(numel);
  d_in.Upload(host_in);

  cuda::nn::WorkspacePool workspace(numel * sizeof(float) + 1024);
  auto in_tensor  = d_in.AsTensor({n, c, h, w});
  auto out_tensor = workspace.AllocateTensor({n, c, h, w});

  cuda::nn::Relu(in_tensor, out_tensor);
  ASSERT_EQ(::cudaDeviceSynchronize(), cudaSuccess);

  // Download via a temporary DeviceBuffer is awkward; use raw D2H.
  std::vector<float> host_out(numel);
  ASSERT_EQ(::cudaMemcpy(host_out.data(), out_tensor.data, numel * sizeof(float),
                         cudaMemcpyDeviceToHost),
            cudaSuccess);

  for (std::size_t i = 0; i < numel; ++i) {
    EXPECT_FLOAT_EQ(host_out[i], host_in[i] > 0.0f ? host_in[i] : 0.0f);
  }

  workspace.Reset();
  EXPECT_EQ(workspace.used_bytes(), 0U);
}

TEST_F(MlOpsWorkspaceTest, MoveTransfersOwnership) {
  cuda::nn::WorkspacePool a(1024);
  void* ptr = a.Allocate(64);
  ASSERT_NE(ptr, nullptr);
  void* base = a.base();

  cuda::nn::WorkspacePool b(std::move(a));
  EXPECT_EQ(a.base(), nullptr);
  EXPECT_EQ(a.capacity_bytes(), 0U);
  EXPECT_EQ(b.base(), base);
  EXPECT_GE(b.used_bytes(), 64U);
}

TEST_F(MlOpsWorkspaceTest, ZeroByteAllocateReturnsNull) {
  cuda::nn::WorkspacePool pool(128);
  EXPECT_EQ(pool.Allocate(0), nullptr);
  EXPECT_EQ(pool.used_bytes(), 0U);
}

TEST_F(MlOpsWorkspaceTest, BadAlignmentThrows) {
  cuda::nn::WorkspacePool pool(128);
  EXPECT_THROW((void)pool.Allocate(16, /*alignment=*/3), std::runtime_error);
}

}  // namespace alcedo
