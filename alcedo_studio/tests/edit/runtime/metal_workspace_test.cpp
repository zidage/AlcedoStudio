//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal_workspace_test_support.hpp"

namespace alcedo {
namespace {

using metal_workspace_test::BindSharpen;
using metal_workspace_test::ExposureFieldBindings;
using metal_workspace_test::MetalWorkspaceFixture;
using metal_workspace_test::UploadFullAndClearDirty;

TEST_F(MetalWorkspaceFixture, MetalParameterArenaUploadsOnlyDirtyRanges) {
  MetalRenderDevice device;
  ParameterSlotKey  key{NodeId{"grade.primary"}, AdjustmentInstanceId{"sharpen"}};
  SharpenModel      model;
  BindSharpen(device.Workspace().Parameters(), key);
  ASSERT_TRUE(UploadFullAndClearDirty(device, key, model));

  model.SetAmount(12.0f);
  auto pending = TakePendingParameterPatch(model);
  ASSERT_TRUE(pending.has_value());

  auto& backend = device.Workspace().Device();
  backend.ResetCounters();
  device.Workspace().Parameters().ApplyPatch(key, pending->Patch());
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  device.WaitIdle();
  pending->Commit();

  ASSERT_EQ(backend.LastHostToDeviceRanges().size(), 1U);
  EXPECT_EQ(backend.LastHostToDeviceRanges().front().size, 4U);
  EXPECT_EQ(backend.HostToDeviceBytes(), 4U);
  EXPECT_LT(backend.HostToDeviceBytes(), sizeof(SharpenPayload));

  backend.ResetCounters();
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  EXPECT_EQ(backend.HostToDeviceBytes(), 0U);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
}

TEST_F(MetalWorkspaceFixture, MetalTransientArenaRewindsWithoutReallocatingItsSlab) {
  MetalRenderDevice device;
  auto&             transients = device.Workspace().TransientBuffers();
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
  EXPECT_EQ(device.Workspace().Device().BufferCreateCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().HeapCreateCount(), 0U);
}

TEST_F(MetalWorkspaceFixture, MetalTexturePoolReusesMatchingPrivateTextures) {
  MetalRenderDevice device;
  auto&             textures = device.Workspace().Textures();
  textures.SetByteBudget(64 * 64 * 16);

  device.BeginRender();
  auto first = textures.Acquire({32, 16, TextureFormat::Rgba32f});
  ASSERT_FALSE(first.Empty());
  const auto             resource_id = first.Texture().ResourceId();
  const auto             width       = first.Texture().Width();
  std::vector<std::byte> pixels(static_cast<std::size_t>(32) * 16 * 16, std::byte{0x3F});
  device.Workspace().Device().UploadTexture2D(first.Texture(), pixels, device.CommandContext());
  device.EndRender();
  first.Release();
  device.WaitIdle();
  device.Workspace().Device().ResetCounters();

  device.BeginRender();
  auto second = textures.Acquire({32, 16, TextureFormat::Rgba32f});
  EXPECT_EQ(second.Texture().ResourceId(), resource_id);
  EXPECT_EQ(second.Texture().Width(), width);
  device.EndRender();
  device.WaitIdle();
  EXPECT_EQ(device.Workspace().Device().TextureCreateCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().HeapCreateCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().MallocCount(), 0U);
  EXPECT_EQ(device.Workspace().Device().FreeCount(), 0U);

  device.BeginRender();
  auto               r32 = textures.Acquire({8, 8, TextureFormat::R32f});
  std::vector<float> r32_host(64, 2.5f);
  auto               r32_bytes = std::as_bytes(std::span<const float>(r32_host));
  device.Workspace().Device().UploadTexture2D(r32.Texture(), r32_bytes, device.CommandContext());
  std::vector<float> r32_back(64, 0.0f);
  device.EndRender();
  device.WaitIdle();
  device.Workspace().Device().DownloadTexture2D(
      r32.Texture(), std::as_writable_bytes(std::span<float>(r32_back)), device.CommandContext());
  EXPECT_FLOAT_EQ(r32_back[0], 2.5f);
  EXPECT_FLOAT_EQ(r32_back[63], 2.5f);
}

TEST_F(MetalWorkspaceFixture, MetalTexturePoolDoesNotEvictBusySubmissionResources) {
  MetalRenderDevice       device;
  auto&                   textures = device.Workspace().Textures();
  constexpr std::uint32_t kSize    = 64;
  textures.SetByteBudget(static_cast<std::size_t>(kSize) * kSize);

  device.BeginRender();
  auto       lease_a  = textures.Acquire({kSize, kSize, TextureFormat::R8});
  const auto handle_a = lease_a.Handle();
  auto       lease_b  = textures.Acquire({kSize, kSize, TextureFormat::R8});
  EXPECT_TRUE(textures.Contains(handle_a));
  device.EndRender();

  lease_a.Release();
  lease_b.Release();
  textures.EvictUntil(static_cast<std::size_t>(kSize) * kSize);
  EXPECT_TRUE(textures.Contains(handle_a));

  device.BeginRender();
  textures.EvictUntil(static_cast<std::size_t>(kSize) * kSize);
  EXPECT_FALSE(textures.Contains(handle_a));
  device.EndRender();
  device.WaitIdle();
}

TEST_F(MetalWorkspaceFixture, MetalMaskTextureCacheUsesOneWorkspaceByteBudget) {
  MetalRenderDevice device;
  auto&             masks = device.Workspace().MaskTextures();
  const Extent2D    extent{8, 8};
  std::size_t       chain_bytes = 0;
  auto              level       = extent;
  while (true) {
    chain_bytes += static_cast<std::size_t>(level.width) * level.height;
    if (level.width == 1 && level.height == 1) {
      break;
    }
    level.width  = std::max<std::uint32_t>(level.width / 2, 1);
    level.height = std::max<std::uint32_t>(level.height / 2, 1);
  }
  masks.SetByteBudget(chain_bytes);

  {
    auto first = masks.Acquire(MaskAssetKey{"mask.a"}, extent);
    EXPECT_EQ(masks.EntryCount(), 1U);
    EXPECT_LE(masks.UsedBytes(), chain_bytes);
  }
  device.BeginRender();
  device.EndRender();
  device.WaitIdle();
  auto second = masks.Acquire(MaskAssetKey{"mask.b"}, extent);
  EXPECT_EQ(masks.EntryCount(), 1U);
  EXPECT_FALSE(masks.Contains(MaskAssetKey{"mask.a"}));
  EXPECT_TRUE(masks.Contains(MaskAssetKey{"mask.b"}));
  EXPECT_LE(masks.UsedBytes(), chain_bytes);
}

TEST_F(MetalWorkspaceFixture, MetalSecondEmptyRenderCreatesNoBufferTextureHeapOrPipelineState) {
  MetalRenderDevice         device;
  auto&                     workspace = device.Workspace();
  const MetalPipelineWarmup warmup{ALCEDO_METAL_UTILS_METALLIB_PATH, "convert_r32f_to_r32f",
                                   "MetalM1WarmUp"};
  workspace.Device().WarmUpPipelines(std::span<const MetalPipelineWarmup>(&warmup, 1));
  workspace.Device().WarmUpPipelines(std::span<const MetalPipelineWarmup>(&warmup, 1));
  EXPECT_GE(workspace.Device().PipelineCreateCount(), 1U);
  EXPECT_GE(workspace.Device().PipelineHitCount(), 1U);

  workspace.Parameters().Reserve(256);
  workspace.TransientBuffers().Reserve(1 << 20);
  workspace.Textures().SetByteBudget(64 * 64);
  workspace.MaskTextures().SetByteBudget(64 * 64);

  device.BeginRender();
  {
    auto texture = workspace.Textures().Acquire({64, 64, TextureFormat::R8});
    auto mask    = workspace.MaskTextures().Acquire(MaskAssetKey{"mask.stable"}, {32, 32});
    ASSERT_NE(workspace.TransientBuffers().Allocate(2048), nullptr);
    auto buffer = workspace.Device().CreateBuffer(64);
    workspace.Values().Store({NodeId{"grade.primary"}, PortId{"commands"}}, std::move(buffer));
    device.EndRender();
  }
  device.WaitIdle();
  workspace.Device().ResetCounters();

  device.BeginRender();
  {
    auto texture = workspace.Textures().Acquire({64, 64, TextureFormat::R8});
    auto mask    = workspace.MaskTextures().Acquire(MaskAssetKey{"mask.stable"}, {32, 32});
    ASSERT_NE(workspace.TransientBuffers().Allocate(2048), nullptr);
    device.EndRender();
  }
  device.WaitIdle();

  EXPECT_EQ(workspace.Device().BufferCreateCount(), 0U);
  EXPECT_EQ(workspace.Device().TextureCreateCount(), 0U);
  EXPECT_EQ(workspace.Device().HeapCreateCount(), 0U);
  EXPECT_EQ(workspace.Device().PipelineCreateCount(), 0U);
  EXPECT_EQ(workspace.Device().MallocCount(), 0U);
  EXPECT_EQ(workspace.Device().FreeCount(), 0U);
  EXPECT_EQ(workspace.Device().HostToDeviceBytes(), 0U);
}

TEST_F(MetalWorkspaceFixture, MetalFailedUploadRestoresDirtyFieldsAndPublishesNoResult) {
  MetalRenderDevice device;
  ParameterSlotKey  key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel     model;
  const auto        fields = ExposureFieldBindings();
  device.Workspace().Parameters().BindSlot(key, 4, fields);
  ASSERT_TRUE(UploadFullAndClearDirty(device, key, model));

  model.SetValue(0.75f);
  {
    auto pending = TakePendingParameterPatch(model);
    ASSERT_TRUE(pending.has_value());
    device.Workspace().Device().FailNextUpload();
    device.Workspace().Parameters().ApplyPatch(key, pending->Patch());
    EXPECT_THROW(device.Workspace().Parameters().UploadDirty(device.CommandContext()),
                 std::runtime_error);
  }
  EXPECT_TRUE(model.IsDirty());

  const GraphValueId id{NodeId{"develop"}, PortId{"sensor_linear"}};
  const ContentKey   content{41};
  device.BeginRender();
  (void)device.Workspace().AcquireImageForWrite(id, {8, 8, TextureFormat::Rgba32f});
  device.Workspace().Images().RecordUnpublished(id, content, {8, 8}, TextureFormat::Rgba32f,
                                                device.CommandContext().SubmissionId());
  device.Workspace().Device().FailNextUpload();
  std::vector<std::byte> pixels(8 * 8 * 16, std::byte{1});
  EXPECT_THROW(
      device.Workspace().Device().UploadTexture2D(device.Workspace().Images().Find(id)->Texture(),
                                                  pixels, device.CommandContext()),
      std::runtime_error);
  device.CancelRender();
  device.WaitIdle();
  EXPECT_FALSE(device.Workspace().Images().FindValidResult(
      id, content, {8, 8}, TextureFormat::Rgba32f,
      device.Workspace().Device().CompletedSubmission()));
  EXPECT_EQ(device.Workspace().Images().PublishedCount(), 0U);
}

TEST_F(MetalWorkspaceFixture, MetalRenderDeviceUsesThePresentationDevice) {
  ASSERT_NE(presentation_device_, nullptr);
  MetalRenderDevice device;
  EXPECT_EQ(device.Workspace().Device().NativeDevice(), presentation_device_);
  EXPECT_EQ(MetalPresentationDeviceHandle(), presentation_device_);
  EXPECT_GT(device.Workspace().Device().WorkingSetBudgetBytes(), 0U);
}

TEST_F(MetalWorkspaceFixture, ParameterUploadFailureRestoresPendingDirtyState) {
  MetalRenderDevice device;
  ParameterSlotKey  key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel     model;
  const auto        fields = ExposureFieldBindings();
  device.Workspace().Parameters().BindSlot(key, 4, fields);
  ASSERT_TRUE(UploadFullAndClearDirty(device, key, model));

  model.SetValue(0.75f);
  auto pending = TakePendingParameterPatch(model);
  ASSERT_TRUE(pending.has_value());
  device.Workspace().Device().FailNextUpload();
  device.Workspace().Parameters().ApplyPatch(key, pending->Patch());
  EXPECT_TRUE(device.Workspace().Parameters().HasPendingUpload());
  EXPECT_THROW(device.Workspace().Parameters().UploadDirty(device.CommandContext()),
               std::runtime_error);
  EXPECT_TRUE(device.Workspace().Parameters().HasPendingUpload());

  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  device.WaitIdle();
  pending->Commit();
  EXPECT_FALSE(model.IsDirty());
  EXPECT_FALSE(device.Workspace().Parameters().HasPendingUpload());

  float     gpu_value = 0.0f;
  std::byte storage[4];
  device.Workspace().Parameters().Download(device.Workspace().Parameters().Binding(key).offset,
                                           std::span<std::byte>(storage, 4),
                                           device.CommandContext());
  std::memcpy(&gpu_value, storage, sizeof(gpu_value));
  EXPECT_FLOAT_EQ(gpu_value, 0.75f);
}

TEST_F(MetalWorkspaceFixture, SinkFailurePublishesNoNewResults) {
  MetalRenderDevice device;
  auto&             workspace = device.Workspace();
  const GraphValueId id{NodeId{"develop"}, PortId{"sensor_linear"}};
  const ContentKey   first{81};
  const ContentKey   second{82};
  constexpr std::uint32_t kWidth  = 8;
  constexpr std::uint32_t kHeight = 8;
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, first, ImageExtent{kWidth, kHeight},
                                       TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f,
                                                 workspace.Device().CompletedSubmission()));

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, second, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, first, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                                 workspace.Device().CompletedSubmission()));
  EXPECT_FALSE(workspace.Images().FindValidResult(id, second, {kWidth, kHeight},
                                                  TextureFormat::Rgba32f,
                                                  workspace.Device().CompletedSubmission()));
  EXPECT_EQ(workspace.Images().UnpublishedCount(), 0U);
}

TEST_F(MetalWorkspaceFixture, RepeatedNodeRemovalReclaimsUnusedResources) {
  MetalRenderDevice device;
  const auto        fields = ExposureFieldBindings();
  device.Workspace().AlignParameterLayout(1);
  device.Workspace().Parameters().BindSlot({NodeId{"grade.a"}, AdjustmentInstanceId{"exposure"}}, 4,
                                           fields);
  device.Workspace().Parameters().BindSlot({NodeId{"grade.b"}, AdjustmentInstanceId{"exposure"}}, 4,
                                           fields);
  device.Workspace().Parameters().BindSlot({NodeId{"grade.c"}, AdjustmentInstanceId{"exposure"}}, 4,
                                           fields);
  const auto used_three = device.Workspace().Parameters().used_bytes();
  EXPECT_EQ(device.Workspace().Parameters().SlotCount(), 3U);
  EXPECT_GT(used_three, 0U);

  device.WaitIdle();
  device.Workspace().AlignParameterLayout(2);
  EXPECT_EQ(device.Workspace().Parameters().SlotCount(), 0U);
  EXPECT_EQ(device.Workspace().Parameters().used_bytes(), 0U);
  EXPECT_EQ(device.Workspace().ParameterLayoutHash(), 2U);

  device.Workspace().Parameters().BindSlot({NodeId{"grade.a"}, AdjustmentInstanceId{"exposure"}}, 4,
                                           fields);
  EXPECT_EQ(device.Workspace().Parameters().SlotCount(), 1U);
  EXPECT_LT(device.Workspace().Parameters().used_bytes(), used_three);
}

}  // namespace
}  // namespace alcedo
