//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda_workspace_test_support.hpp"

#include <cstring>
#include <span>
#include <stdexcept>

#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"

namespace alcedo {
namespace {

using cuda_workspace_test::BindSharpen;
using cuda_workspace_test::CudaWorkspaceFixture;
using cuda_workspace_test::ExposureFieldBindings;
using cuda_workspace_test::UploadFullAndClearDirty;

TEST_F(CudaWorkspaceFixture, ParameterArenaKeepsStableOffsetsAcrossRenders) {
  CudaRenderDevice device;
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"sharpen"}};
  const auto first = BindSharpen(device.Workspace().Parameters(), key);

  device.BeginRender();
  device.EndRender();
  device.BeginRender();
  device.EndRender();
  device.WaitIdle();

  const auto& again = device.Workspace().Parameters().Binding(key);
  EXPECT_EQ(again.offset, first.offset);
  EXPECT_EQ(again.size, first.size);
  ASSERT_EQ(again.fields.size(), 3U);
  EXPECT_EQ(again.fields[0].destination_offset, first.fields[0].destination_offset);
}

TEST_F(CudaWorkspaceFixture, ParameterArenaUploadsOnlyDirtyFieldRanges) {
  CudaRenderDevice device;
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"sharpen"}};
  SharpenModel     model;
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
}

TEST_F(CudaWorkspaceFixture, AdjacentDirtyParameterRangesMergeBeforeCudaCopy) {
  CudaRenderDevice device;
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"sharpen"}};
  SharpenModel     model;
  BindSharpen(device.Workspace().Parameters(), key);
  ASSERT_TRUE(UploadFullAndClearDirty(device, key, model));

  model.SetAmount(8.0f);
  model.SetRadius(5.0f);
  auto pending = TakePendingParameterPatch(model);
  ASSERT_TRUE(pending.has_value());

  auto& backend = device.Workspace().Device();
  backend.ResetCounters();
  device.Workspace().Parameters().ApplyPatch(key, pending->Patch());
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  device.WaitIdle();
  pending->Commit();

  ASSERT_EQ(backend.LastHostToDeviceRanges().size(), 1U);
  EXPECT_EQ(backend.LastHostToDeviceRanges().front().size, 8U);
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 1U);
  EXPECT_EQ(backend.HostToDeviceBytes(), 8U);
}

TEST_F(CudaWorkspaceFixture, RepeatedDirtyWritesUseLatestValue) {
  CudaRenderDevice device;
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel    model;
  const auto       fields = ExposureFieldBindings();
  device.Workspace().Parameters().BindSlot(key, 4, fields);

  model.SetValue(0.25f);
  model.SetValue(1.5f);
  auto pending = TakePendingParameterPatch(model);
  ASSERT_TRUE(pending.has_value());
  device.Workspace().Parameters().ApplyPatch(key, pending->Patch());
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  device.WaitIdle();
  pending->Commit();

  float gpu_value = 0.0f;
  std::byte storage[4];
  device.Workspace().Parameters().Download(device.Workspace().Parameters().Binding(key).offset,
                                           std::span<std::byte>(storage, 4),
                                           device.CommandContext());
  std::memcpy(&gpu_value, storage, sizeof(gpu_value));
  EXPECT_FLOAT_EQ(gpu_value, 1.5f);
}

TEST_F(CudaWorkspaceFixture, CancelledCudaParameterCopyRestoresDirtyFields) {
  CudaRenderDevice device;
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel    model;
  const auto       fields = ExposureFieldBindings();
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

  auto retry = TakePendingParameterPatch(model);
  ASSERT_TRUE(retry.has_value());
  device.Workspace().Parameters().ApplyPatch(key, retry->Patch());
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  device.WaitIdle();
  retry->Commit();
  EXPECT_FALSE(model.IsDirty());
}

TEST_F(CudaWorkspaceFixture, UnchangedParametersIssueNoHostToDeviceCopy) {
  CudaRenderDevice device;
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel    model;
  const auto       fields = ExposureFieldBindings();
  device.Workspace().Parameters().BindSlot(key, 4, fields);
  ASSERT_TRUE(UploadFullAndClearDirty(device, key, model));

  auto& backend = device.Workspace().Device();
  backend.ResetCounters();
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  EXPECT_EQ(backend.HostToDeviceCopyCount(), 0U);
  EXPECT_EQ(backend.HostToDeviceBytes(), 0U);
}

TEST_F(CudaWorkspaceFixture, SameAdjustmentTypeUsesDistinctNodeSlots) {
  CudaRenderDevice device;
  ParameterSlotKey grade_a{NodeId{"grade.a"}, AdjustmentInstanceId{"exposure"}};
  ParameterSlotKey grade_b{NodeId{"grade.b"}, AdjustmentInstanceId{"exposure"}};
  const auto       fields = ExposureFieldBindings();
  device.Workspace().Parameters().BindSlot(grade_a, 4, fields);
  device.Workspace().Parameters().BindSlot(grade_b, 4, fields);
  EXPECT_NE(device.Workspace().Parameters().Binding(grade_a).offset,
            device.Workspace().Parameters().Binding(grade_b).offset);

  ExposureModel model_a;
  ExposureModel model_b;
  model_a.SetValue(0.25f);
  model_b.SetValue(1.75f);
  device.Workspace().Parameters().InitializeFromFullDto(grade_a, model_a.MakeFullDto());
  device.Workspace().Parameters().InitializeFromFullDto(grade_b, model_b.MakeFullDto());
  device.Workspace().Parameters().UploadDirty(device.CommandContext());
  device.WaitIdle();

  float gpu_a = 0.0f;
  float gpu_b = 0.0f;
  std::byte storage[4];
  device.Workspace().Parameters().Download(device.Workspace().Parameters().Binding(grade_a).offset,
                                           std::span<std::byte>(storage, 4),
                                           device.CommandContext());
  std::memcpy(&gpu_a, storage, sizeof(gpu_a));
  device.Workspace().Parameters().Download(device.Workspace().Parameters().Binding(grade_b).offset,
                                           std::span<std::byte>(storage, 4),
                                           device.CommandContext());
  std::memcpy(&gpu_b, storage, sizeof(gpu_b));
  EXPECT_FLOAT_EQ(gpu_a, 0.25f);
  EXPECT_FLOAT_EQ(gpu_b, 1.75f);
}

TEST_F(CudaWorkspaceFixture, ParameterUploadFailureRestoresPendingDirtyState) {
  CudaRenderDevice device;
  ParameterSlotKey key{NodeId{"grade.primary"}, AdjustmentInstanceId{"exposure"}};
  ExposureModel    model;
  const auto       fields = ExposureFieldBindings();
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

  float gpu_value = 0.0f;
  std::byte storage[4];
  device.Workspace().Parameters().Download(device.Workspace().Parameters().Binding(key).offset,
                                           std::span<std::byte>(storage, 4),
                                           device.CommandContext());
  std::memcpy(&gpu_value, storage, sizeof(gpu_value));
  EXPECT_FLOAT_EQ(gpu_value, 0.75f);
}

TEST_F(CudaWorkspaceFixture, RepeatedNodeRemovalReclaimsUnusedResources) {
  CudaRenderDevice device;
  const auto       fields = ExposureFieldBindings();
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
