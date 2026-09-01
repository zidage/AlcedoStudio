//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda_workspace_test_support.hpp"

#include <stdexcept>

#include "edit/runtime/content_key.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

using cuda_workspace_test::CudaWorkspaceFixture;

constexpr std::uint32_t kWidth  = 8;
constexpr std::uint32_t kHeight = 8;

auto Value() -> GraphValueId { return {NodeId{"develop"}, PortId{"sensor_linear"}}; }

auto PublishWrite(CudaRenderDevice& device, const GraphValueId& id, ContentKey key) {
  auto& workspace = device.Workspace();
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, key, ImageExtent{kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
}

TEST_F(CudaWorkspaceFixture, ResultCacheDoesNotTreatReusedTextureAllocationAsContentHit) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  workspace.Textures().SetByteBudget(static_cast<std::size_t>(kWidth) * kHeight * 16);
  const GraphValueId id    = Value();
  const ContentKey   key_a{11};
  const ContentKey   key_b{22};
  PublishWrite(device, id, key_a);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, key_a, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, completed));
  const auto resource_a = workspace.Images().Find(id)->Texture().ResourceId();

  device.BeginRender();
  auto& write = workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  EXPECT_EQ(write.Texture().ResourceId(), resource_a);
  EXPECT_FALSE(workspace.Images().FindValidResult(id, key_a, {kWidth, kHeight},
                                                  TextureFormat::Rgba32f, completed));
  workspace.Images().RecordUnpublished(id, key_b, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
  const auto done = workspace.Device().CompletedSubmission();
  EXPECT_FALSE(workspace.Images().FindValidResult(id, key_a, {kWidth, kHeight},
                                                  TextureFormat::Rgba32f, done));
  EXPECT_TRUE(workspace.Images().FindValidResult(id, key_b, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, done));
}

TEST_F(CudaWorkspaceFixture, FailedSubmissionDoesNotPublishResultContentKey) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId id  = Value();
  const ContentKey   key{31};
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, key, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_FALSE(workspace.Images().FindValidResult(id, key, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                                  workspace.Device().CompletedSubmission()));
  EXPECT_EQ(workspace.Images().PublishedCount(), 0U);
}

TEST_F(CudaWorkspaceFixture, CancelledSubmissionKeepsPreviouslyCompletedCacheEntriesUsable) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId id     = Value();
  const ContentKey   first{41};
  const ContentKey   second{42};
  PublishWrite(device, id, first);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, completed));

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, second, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  device.CancelRender();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, first, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f,
                                                 workspace.Device().CompletedSubmission()));
  EXPECT_FALSE(workspace.Images().FindValidResult(id, second, {kWidth, kHeight},
                                                  TextureFormat::Rgba32f,
                                                  workspace.Device().CompletedSubmission()));
}

TEST_F(CudaWorkspaceFixture, AliasTextureFromSharesOneAllocationAcrossTwoValueIds) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId sensor{NodeId{"develop"}, PortId{"sensor_linear"}};
  const GraphValueId geometry{NodeId{"geometry"}, PortId{"scene_source"}};
  const ContentKey   sensor_key{61};
  const ContentKey   geometry_key{62};

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(sensor, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(sensor, sensor_key, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  const auto used_before = workspace.Textures().UsedBytes();
  const auto entries_before = workspace.Textures().EntryCount();
  const auto sensor_id =
      workspace.Images().Find(sensor)->Texture().ResourceId();
  (void)workspace.AliasImageFrom(geometry, sensor);
  workspace.Images().RecordUnpublished(geometry, geometry_key, {kWidth, kHeight},
                                       TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  EXPECT_EQ(workspace.Images().Find(geometry)->Texture().ResourceId(), sensor_id);
  EXPECT_EQ(workspace.Textures().UsedBytes(), used_before);
  EXPECT_EQ(workspace.Textures().EntryCount(), entries_before);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();

  const auto completed = workspace.Device().CompletedSubmission();
  EXPECT_TRUE(workspace.Images().FindValidResult(sensor, sensor_key, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(geometry, geometry_key, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, completed));
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

TEST_F(CudaWorkspaceFixture, UnpublishedWriteIsNotAContentHitUntilPublish) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId id  = Value();
  const ContentKey   key{51};
  device.BeginRender();
  (void)workspace.AcquireImageForWrite(id, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(id, key, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  EXPECT_FALSE(workspace.Images().FindValidResult(id, key, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                                  workspace.Device().CompletedSubmission()));
  ASSERT_NE(workspace.Images().Find(id), nullptr);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();
  EXPECT_TRUE(workspace.Images().FindValidResult(id, key, {kWidth, kHeight}, TextureFormat::Rgba32f,
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
  const GraphValueId id     = Value();
  const ContentKey   first{81};
  const ContentKey   second{82};
  PublishWrite(device, id, first);
  const auto completed = workspace.Device().CompletedSubmission();
  ASSERT_TRUE(workspace.Images().FindValidResult(id, first, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, completed));

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

TEST_F(CudaWorkspaceFixture, SharedInputSurvivesBothBranchReaders) {
  CudaRenderDevice device;
  auto&            workspace = device.Workspace();
  const GraphValueId develop{NodeId{"develop"}, PortId{"image"}};
  const GraphValueId grade_a{NodeId{"grade.a"}, PortId{"image"}};
  const GraphValueId grade_b{NodeId{"grade.b"}, PortId{"image"}};
  const ContentKey   develop_key{91};
  const ContentKey   a_key{92};
  const ContentKey   b_key{93};

  device.BeginRender();
  (void)workspace.AcquireImageForWrite(develop, {kWidth, kHeight, TextureFormat::Rgba32f});
  workspace.Images().RecordUnpublished(develop, develop_key, {kWidth, kHeight},
                                       TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  const auto develop_id = workspace.Images().Find(develop)->Texture().ResourceId();
  (void)workspace.AliasImageFrom(grade_a, develop);
  workspace.Images().RecordUnpublished(grade_a, a_key, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  (void)workspace.AliasImageFrom(grade_b, develop);
  workspace.Images().RecordUnpublished(grade_b, b_key, {kWidth, kHeight}, TextureFormat::Rgba32f,
                                       device.CommandContext().SubmissionId());
  EXPECT_EQ(workspace.Images().Find(grade_a)->Texture().ResourceId(), develop_id);
  EXPECT_EQ(workspace.Images().Find(grade_b)->Texture().ResourceId(), develop_id);
  device.EndRender();
  device.PublishResults();
  device.WaitIdle();

  const auto completed = workspace.Device().CompletedSubmission();
  EXPECT_TRUE(workspace.Images().FindValidResult(develop, develop_key, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(grade_a, a_key, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, completed));
  EXPECT_TRUE(workspace.Images().FindValidResult(grade_b, b_key, {kWidth, kHeight},
                                                 TextureFormat::Rgba32f, completed));
  EXPECT_EQ(workspace.Images().Find(grade_a)->Texture().ResourceId(),
            workspace.Images().Find(develop)->Texture().ResourceId());
  EXPECT_EQ(workspace.Textures().EntryCount(), 1U);
}

}  // namespace
}  // namespace alcedo
