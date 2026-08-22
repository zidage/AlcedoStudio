//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "cuda_workspace_test_support.hpp"

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

}  // namespace
}  // namespace alcedo
