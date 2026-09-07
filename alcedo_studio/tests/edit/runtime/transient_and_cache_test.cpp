//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda_workspace_test_support.hpp"

#include <stdexcept>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/texture_format.hpp"
#include "gpu/gpu_pool_trace.hpp"

namespace alcedo {
namespace {

using cuda_workspace_test::CudaWorkspaceFixture;

TEST_F(CudaWorkspaceFixture, WorkspaceResetRewindsTransientMemoryWithoutFreeingCudaAllocation) {
  CudaRenderDevice device;
  auto&            transients = device.Workspace().TransientBuffers();
  transients.Reserve(4096);
  device.Workspace().Device().ResetCounters();

  void* first = transients.Allocate(512);
  ASSERT_NE(first, nullptr);
  const auto capacity = transients.capacity_bytes();
  transients.Reset();
  EXPECT_EQ(transients.used_bytes(), 0U);
  EXPECT_EQ(transients.capacity_bytes(), capacity);
  void* second = transients.Allocate(512);
  EXPECT_EQ(second, first);
  EXPECT_EQ(device.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().FreeCount(), 0U);
}

TEST_F(CudaWorkspaceFixture, WorkspaceCannotReplaceReservedSlabWhileTransientPointersAreLive) {
  CudaRenderDevice device;
  auto&            transients = device.Workspace().TransientBuffers();
  transients.Reserve(256);
  void* live = transients.Allocate(64);
  ASSERT_NE(live, nullptr);
  EXPECT_THROW(transients.Reserve(1024), std::runtime_error);
  device.Workspace().Device().ResetCounters();
  void* extra = transients.Allocate(transients.capacity_bytes());
  ASSERT_NE(extra, nullptr);
  EXPECT_NE(extra, live);
  EXPECT_GE(device.Workspace().Device().MallocCount(), 1U);
}

TEST_F(CudaWorkspaceFixture, NodeResultCacheReturnsValueByProducerNodeAndPort) {
  CudaRenderDevice device;
  auto             buffer = device.Workspace().Device().CreateBuffer(32);
  ASSERT_FALSE(buffer.Empty());
  const void* ptr = buffer.DevicePointer();
  const GraphValueId id{NodeId{"develop"}, PortId{"image"}};
  device.Workspace().Values().Store(id, std::move(buffer));

  auto* found = device.Workspace().Values().Find(NodeId{"develop"}, PortId{"image"});
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->DevicePointer(), ptr);

  EXPECT_EQ(device.Workspace().Values().Find(NodeId{"develop"}, PortId{"mask"}), nullptr);
  EXPECT_EQ(device.Workspace().Values().Find(NodeId{"drt"}, PortId{"image"}), nullptr);
}

TEST_F(CudaWorkspaceFixture, SecondRenderUsesNoCudaAllocationAfterPeakReserve) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  workspace.Parameters().Reserve(256);
  workspace.TransientBuffers().Reserve(1 << 20);

  device.BeginRender();
  {
    auto lease = workspace.Textures().Acquire({64, 64, TextureFormat::R8});
    ASSERT_FALSE(lease.Empty());
    ASSERT_NE(workspace.TransientBuffers().Allocate(2048), nullptr);
    device.EndRender();
  }
  device.WaitIdle();
  workspace.Device().ResetCounters();

  device.BeginRender();
  {
    auto lease = workspace.Textures().Acquire({64, 64, TextureFormat::R8});
    ASSERT_FALSE(lease.Empty());
    ASSERT_NE(workspace.TransientBuffers().Allocate(2048), nullptr);
    device.EndRender();
  }
  device.WaitIdle();

  EXPECT_EQ(workspace.Device().MallocCount(), 0U);
  EXPECT_EQ(workspace.Device().FreeCount(), 0U);
}

TEST(GpuDagGpuPoolTrace, LargeAllocThresholdIsSixteenMebibytes) {
  EXPECT_TRUE(ShouldTraceGpuPoolAlloc(kGpuPoolTraceMinAllocBytes));
  EXPECT_TRUE(ShouldTraceGpuPoolAlloc(kGpuPoolTraceMinAllocBytes + 1));
  if (!GpuPoolTraceEnvEnabled()) {
    EXPECT_FALSE(ShouldTraceGpuPoolAlloc(kGpuPoolTraceMinAllocBytes - 1));
    EXPECT_FALSE(GpuPoolTraceVerbose());
  } else {
    EXPECT_TRUE(GpuPoolTraceVerbose());
  }
}

TEST_F(CudaWorkspaceFixture, DumpGpuPoolsPrintsResidentTexturesAndTransientsWithoutThrowing) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  device.BeginRender();
  const GraphValueId id{NodeId{"develop"}, PortId{"sensor_linear"}};
  (void)workspace.AcquireImageForWrite(id, {8, 8, TextureFormat::Rgba32f});
  (void)workspace.TransientBuffers().Allocate(512);
  workspace.DumpGpuPools("test");
  device.CancelRender();
}

}  // namespace
}  // namespace alcedo
