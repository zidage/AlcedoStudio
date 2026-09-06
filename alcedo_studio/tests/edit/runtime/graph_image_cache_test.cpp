//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda_workspace_test_support.hpp"

#include <stdexcept>

#include "edit/runtime/result_representation.hpp"
#include "edit/runtime/runtime_revision.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

using cuda_workspace_test::CudaWorkspaceFixture;

constexpr std::uint32_t kWidth  = 8;
constexpr std::uint32_t kHeight = 8;

auto Value() -> GraphValueId { return {NodeId{"develop"}, PortId{"sensor_linear"}}; }

auto ImageRepr(TextureFormat format = TextureFormat::Rgba32f) -> ResultRepresentation {
  return MakeExtentRepresentation({kWidth, kHeight}, format);
}

void PublishWrite(CudaRenderDevice& device, const GraphValueId& id, RuntimeRevision revision) {
  auto& workspace = device.Workspace();
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, revision, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
}

TEST_F(CudaWorkspaceFixture, ResultCacheDoesNotTreatReusedTextureAllocationAsContentHit) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  workspace.Textures().SetByteBudget(static_cast<std::size_t>(kWidth) * kHeight * 16);
  const GraphValueId    id         = Value();
  constexpr RuntimeRevision first  = 11;
  constexpr RuntimeRevision second = 22;
  PublishWrite(device, id, first);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(), completed));
  const auto resource_a = workspace.Images().Find(id)->Texture().ResourceId();

  device.BeginRender();
  auto& write = workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  EXPECT_EQ(write.Texture().ResourceId(), resource_a);
  EXPECT_FALSE(workspace.Images().FindValidResult(id, first, ImageRepr(), completed));
  workspace.Images().RecordUnpublished(id, second, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
  const auto done = workspace.Device().CompletedSubmission();
  EXPECT_FALSE(workspace.Images().FindValidResult(id, first, ImageRepr(), done));
  EXPECT_TRUE(workspace.Images().FindValidResult(id, second, ImageRepr(), done));
  EXPECT_EQ(workspace.Images().PublishedCount(), 1U);
}

TEST_F(CudaWorkspaceFixture, FailedSubmissionDoesNotPublishResultRevision) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId    id       = Value();
  constexpr RuntimeRevision revision = 31;
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, revision, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_FALSE(workspace.Images().FindValidResult(id, revision, ImageRepr(),
                                                  workspace.Device().CompletedSubmission()));
  EXPECT_EQ(workspace.Images().PublishedCount(), 0U);
}

TEST_F(CudaWorkspaceFixture, CancelledSubmissionKeepsPreviouslyCompletedCacheEntriesUsable) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId    id     = Value();
  constexpr RuntimeRevision first  = 41;
  constexpr RuntimeRevision second = 42;
  PublishWrite(device, id, first);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(), completed));

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, second, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(),
                                                 workspace.Device().CompletedSubmission()));
  EXPECT_FALSE(workspace.Images().FindValidResult(id, second, ImageRepr(),
                                                  workspace.Device().CompletedSubmission()));
}

TEST_F(CudaWorkspaceFixture, AliasTextureFromSharesOneAllocationAcrossTwoValueIds) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId sensor{NodeId{"develop"}, PortId{"sensor_linear"}};
  const GraphValueId geometry{NodeId{"geometry"}, PortId{"scene_source"}};
  constexpr RuntimeRevision sensor_rev   = 61;
  constexpr RuntimeRevision geometry_rev = 62;

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(sensor, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(sensor, sensor_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  const auto used_before    = workspace.Textures().UsedBytes();
  const auto entries_before = workspace.Textures().EntryCount();
  const auto sensor_id      = workspace.Images().Find(sensor)->Texture().ResourceId();
  (void)workspace.AliasImageFrom(geometry, sensor);
  workspace.Images().RecordUnpublished(geometry, geometry_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  EXPECT_EQ(workspace.Images().Find(geometry)->Texture().ResourceId(), sensor_id);
  EXPECT_EQ(workspace.Textures().UsedBytes(), used_before);
  EXPECT_EQ(workspace.Textures().EntryCount(), entries_before);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();

  const auto completed = workspace.Device().CompletedSubmission();
  EXPECT_TRUE(workspace.Images().FindValidResult(sensor, sensor_rev, ImageRepr(), completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(geometry, geometry_rev, ImageRepr(), completed));
  EXPECT_EQ(workspace.Images().Find(sensor)->Texture().ResourceId(),
            workspace.Images().Find(geometry)->Texture().ResourceId());
  EXPECT_EQ(workspace.Images().PublishedCount(), 2U);
  EXPECT_EQ(workspace.Textures().EntryCount(), 1U);
}

TEST_F(CudaWorkspaceFixture, AliasTextureFromRejectsMissingSourceAndSelfAlias) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId sensor{NodeId{"develop"}, PortId{"sensor_linear"}};
  const GraphValueId geometry{NodeId{"geometry"}, PortId{"scene_source"}};
  device.BeginRender();
  EXPECT_THROW((void)workspace.AliasImageFrom(geometry, sensor), std::runtime_error);
  (void)workspace.AcquireImageForWrite(sensor, {kWidth, kHeight, TextureFormat::Rgba32f});
  EXPECT_THROW((void)workspace.AliasImageFrom(sensor, sensor), std::runtime_error);
  device.CancelRender();
}

TEST_F(CudaWorkspaceFixture, UnpublishedWriteIsNotValidUntilPublish) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId    id       = Value();
  constexpr RuntimeRevision revision = 51;
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, revision, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  EXPECT_FALSE(workspace.Images().FindValidResult(id, revision, ImageRepr(),
                                                  workspace.Device().CompletedSubmission()));
  ASSERT_NE(workspace.Images().Find(id), nullptr);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, revision, ImageRepr(),
                                                 workspace.Device().CompletedSubmission()));
}

TEST_F(CudaWorkspaceFixture, BackgroundIntermediateImagesReleaseAfterLastConsumer) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId develop{NodeId{"develop"}, PortId{"image"}};
  const GraphValueId grade{NodeId{"grade.primary"}, PortId{"image"}};
  const GraphValueId mask{NodeId{"mask"}, PortId{"mask"}};
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(develop, {kWidth, kHeight, TextureFormat::Rgba32f});
  (void)workspace.AcquireImageForWrite(grade, {kWidth, kHeight, TextureFormat::Rgba32f});
  (void)workspace.AcquireImageForWrite(mask, {kWidth, kHeight, TextureFormat::R8});
  EXPECT_EQ(workspace.Images().UnpublishedWriteCount(), 3U);
  device.WaitIdle();
  workspace.ReleaseConsumedImage(develop);
  EXPECT_EQ(workspace.Images().UnpublishedWriteCount(), 2U);
  EXPECT_EQ(workspace.Images().Find(develop), nullptr);
  ASSERT_NE(workspace.Images().Find(grade), nullptr);
  ASSERT_NE(workspace.Images().Find(mask), nullptr);
  workspace.ReleaseConsumedImage(mask);
  EXPECT_EQ(workspace.Images().UnpublishedWriteCount(), 1U);
  EXPECT_NE(workspace.Images().Find(grade), nullptr);
  device.CancelRender();
}

TEST_F(CudaWorkspaceFixture, SinkFailurePublishesNoNewResults) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId    id     = Value();
  constexpr RuntimeRevision first  = 81;
  constexpr RuntimeRevision second = 82;
  PublishWrite(device, id, first);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(), completed));

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, second, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, first, ImageRepr(),
                                                 workspace.Device().CompletedSubmission()));
  EXPECT_FALSE(workspace.Images().FindValidResult(id, second, ImageRepr(),
                                                  workspace.Device().CompletedSubmission()));
  EXPECT_EQ(workspace.Images().UnpublishedCount(), 0U);
}

TEST_F(CudaWorkspaceFixture, SharedInputSurvivesBothBranchReaders) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId develop{NodeId{"develop"}, PortId{"image"}};
  const GraphValueId grade_a{NodeId{"grade.a"}, PortId{"image"}};
  const GraphValueId grade_b{NodeId{"grade.b"}, PortId{"image"}};
  constexpr RuntimeRevision develop_rev = 91;
  constexpr RuntimeRevision a_rev       = 92;
  constexpr RuntimeRevision b_rev       = 93;

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(develop, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(develop, develop_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  const auto develop_id = workspace.Images().Find(develop)->Texture().ResourceId();
  (void)workspace.AliasImageFrom(grade_a, develop);
  workspace.Images().RecordUnpublished(grade_a, a_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  (void)workspace.AliasImageFrom(grade_b, develop);
  workspace.Images().RecordUnpublished(grade_b, b_rev, ImageRepr(),
                                       device.CommandContext().SubmissionId());
  EXPECT_EQ(workspace.Images().Find(grade_a)->Texture().ResourceId(), develop_id);
  EXPECT_EQ(workspace.Images().Find(grade_b)->Texture().ResourceId(), develop_id);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();

  const auto completed = workspace.Device().CompletedSubmission();
  EXPECT_TRUE(workspace.Images().FindValidResult(develop, develop_rev, ImageRepr(), completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(grade_a, a_rev, ImageRepr(), completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(grade_b, b_rev, ImageRepr(), completed));
  EXPECT_EQ(workspace.Images().Find(grade_a)->Texture().ResourceId(),
            workspace.Images().Find(develop)->Texture().ResourceId());
  EXPECT_EQ(workspace.Textures().EntryCount(), 1U);
}

}  // namespace
}  // namespace alcedo
