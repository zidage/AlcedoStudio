//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ui/editor_rhi/editor_backend.hpp"
#include "ui/editor_rhi/frame_presentation_broker.hpp"
#include "ui/editor_rhi/frame_presentation_lease.hpp"
#include "ui/editor_rhi/harness_fixtures.hpp"
#include "ui/editor_rhi/native_resource_counters.hpp"

namespace alcedo::editor_rhi {
namespace {

TEST(EditorBackendParseTest, ParsesEqualsAndSeparateForms) {
  {
    char arg0[] = "EditorRhiHarness";
    char arg1[] = "--editor-backend=cuda";
    char* argv[] = {arg0, arg1};
    const auto result = ParseEditorBackendArgs(2, argv);
    ASSERT_TRUE(result.present);
    ASSERT_TRUE(result.backend.has_value());
    EXPECT_EQ(*result.backend, EditorBackend::Cuda);
    EXPECT_TRUE(result.error.empty());
  }
  {
    char arg0[] = "EditorRhiHarness";
    char arg1[] = "--editor-backend";
    char arg2[] = "opencl";
    char* argv[] = {arg0, arg1, arg2};
    const auto result = ParseEditorBackendArgs(3, argv);
    ASSERT_TRUE(result.backend.has_value());
    EXPECT_EQ(*result.backend, EditorBackend::OpenCl);
  }
  {
    char arg0[] = "EditorRhiHarness";
    char arg1[] = "--editor-backend=metal";
    char* argv[] = {arg0, arg1};
    const auto result = ParseEditorBackendArgs(2, argv);
    ASSERT_TRUE(result.backend.has_value());
    EXPECT_EQ(*result.backend, EditorBackend::Metal);
  }
}

TEST(EditorBackendParseTest, RejectsUnknownBackendToken) {
  char arg0[] = "EditorRhiHarness";
  char arg1[] = "--editor-backend=vulkan";
  char* argv[] = {arg0, arg1};
  const auto result = ParseEditorBackendArgs(2, argv);
  EXPECT_TRUE(result.present);
  EXPECT_FALSE(result.backend.has_value());
  EXPECT_FALSE(result.error.empty());
}

TEST(EditorBackendParseTest, GraphicsApiNamesMatchStartupContract) {
  EXPECT_STREQ(QtGraphicsApiName(EditorBackend::Cuda), "Direct3D11");
  EXPECT_STREQ(QtGraphicsApiName(EditorBackend::OpenCl), "OpenGL");
  EXPECT_STREQ(QtGraphicsApiName(EditorBackend::Metal), "Metal");
}

TEST(EditorBackendActiveTest, SetActiveEditorBackendIsReadable) {
  SetActiveEditorBackend(EditorBackend::OpenCl);
  EXPECT_TRUE(HasActiveEditorBackend());
  EXPECT_EQ(ActiveEditorBackend(), EditorBackend::OpenCl);
  SetActiveEditorBackend(EditorBackend::Cuda);
  EXPECT_EQ(ActiveEditorBackend(), EditorBackend::Cuda);
}

TEST(HarnessFixturesTest, GradientIsDeterministicAndNormalized) {
  const auto a = MakeFp32Gradient(4, 3);
  const auto b = MakeFp32Gradient(4, 3);
  ASSERT_EQ(a.pixels.size(), 12u);
  EXPECT_EQ(a.pixels.size(), b.pixels.size());
  EXPECT_EQ(std::memcmp(a.data(), b.data(), a.byte_size()), 0);
  EXPECT_FLOAT_EQ(a.pixels.front().r, 0.0f);
  EXPECT_FLOAT_EQ(a.pixels.front().g, 0.0f);
  EXPECT_FLOAT_EQ(a.pixels.back().a, 1.0f);
  EXPECT_NEAR(a.pixels.back().r, 1.0f, 1e-6f);
  EXPECT_NEAR(a.pixels.back().g, 1.0f, 1e-6f);
}

TEST(HarnessFixturesTest, CheckerboardAndOddSizedAndRoi) {
  const auto checker = MakeCheckerboard(16, 16, 8);
  EXPECT_EQ(checker.width, 16);
  EXPECT_FLOAT_EQ(checker.pixels.front().r, 0.9f);

  const auto odd = MakeOddSized(62, 46);
  EXPECT_EQ(odd.width % 2, 1);
  EXPECT_EQ(odd.height % 2, 1);

  const auto roi = MakeRoiPatch(32, 32, 8, 8, 4, 4, {1.0f, 0.0f, 0.0f, 1.0f});
  const auto& p = roi.pixels[static_cast<std::size_t>(8) * 32 + 8];
  EXPECT_FLOAT_EQ(p.r, 1.0f);
  EXPECT_FLOAT_EQ(p.g, 0.0f);
}

TEST(HarnessFixturesTest, MaxAbsPixelErrorWithinTolerance) {
  const auto expected = MakeFixture(HarnessFixtureKind::Fp32Gradient);
  std::vector<float> actual(expected.byte_size() / sizeof(float));
  std::memcpy(actual.data(), expected.data(), expected.byte_size());
  actual[0] += kHarnessPixelAbsTolerance * 0.5f;
  const float err =
      MaxAbsPixelError(expected, actual.data(), expected.width, expected.height,
                       expected.row_bytes());
  EXPECT_GE(err, 0.0f);
  EXPECT_LE(err, kHarnessPixelAbsTolerance);
}

TEST(FramePresentationLeaseTest, MetalLeaseContractIsDefinedWithoutImplementationClaim) {
  MetalSharedTextureLeasePayload metal{};
  metal.mtl_texture      = 0x1;
  metal.width            = 64;
  metal.height           = 48;
  metal.mtl_pixel_format = 0;

  WritableTargetLease lease;
  lease.backend       = EditorBackend::Metal;
  lease.handle_kind   = LeaseHandleKindForBackend(EditorBackend::Metal);
  lease.writable_kind = LeaseWritableKindForBackend(EditorBackend::Metal);
  lease.dimensions    = {metal.width, metal.height};
  lease.generation    = {1, 1, 1, 42};
  lease.native_handle = metal.mtl_texture;
  lease.writable_resource = metal.mtl_texture;
  lease.lifetime_token = std::make_shared<LeaseLifetimeToken>();
  EXPECT_EQ(lease.handle_kind, LeaseNativeHandleKind::MetalTexture);
  EXPECT_TRUE(lease.valid());
  const std::string desc = DescribeLease(lease);
  EXPECT_NE(desc.find("metal"), std::string::npos);
}

auto MakeTestLease(std::uintptr_t native_handle, std::uint64_t target_generation,
                   std::uint64_t image_generation, std::uint64_t image_identity = 0,
                   int width = 64, int height = 48) -> WritableTargetLease {
  WritableTargetLease lease;
  lease.backend = EditorBackend::Cuda;
  lease.handle_kind = LeaseNativeHandleKind::D3D11Texture2D;
  lease.writable_kind = LeaseWritableResourceKind::CudaArray;
  lease.dimensions = {width, height};
  lease.generation = {target_generation, image_generation, 0, image_identity};
  lease.native_handle = native_handle;
  lease.writable_resource = native_handle + 0x1000;
  lease.lifetime_token = std::make_shared<LeaseLifetimeToken>();
  return lease;
}

auto MakeCompleted(const WritableTargetLease& target, std::uint64_t preview,
                   LeaseFrameLayer layer = LeaseFrameLayer::QualityBase,
                   std::uint64_t detail_serial = 0) -> CompletedFrameLease {
  CompletedFrameLease frame;
  frame.target = target;
  frame.layer = layer;
  // generation must match the acquired lease identity; preview is separate.
  frame.generation = target.generation;
  frame.preview_generation = preview;
  frame.detail_serial = detail_serial;
  frame.producer_complete = true;
  return frame;
}

TEST(FramePresentationBrokerTest, AcquiresSubmitsAndConsumesNewestCompatibleLayer) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.InvalidateImageGeneration(7, 100);
  broker.InvalidateTargetGeneration();
  const auto gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0x10, gen, 7, 100)));
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0x11, gen, 7, 100)));

  WritableTargetRequest req;
  req.layer = LeaseFrameLayer::QualityBase;
  req.dimensions = {64, 48};
  req.image_generation = 7;
  req.image_identity = 100;
  req.layer_generation = 1;

  // Available targets form a shared same-size pool. A quality request must be
  // considered ready even when the target was originally tagged interactive;
  // acquisition retags it for the producer.
  EXPECT_TRUE(broker.HasWritableTarget(req));
  auto target = broker.TryAcquireWritableTarget(req);
  ASSERT_TRUE(target.has_value());
  target->layer = LeaseFrameLayer::QualityBase;

  CompletedFrameLease older = MakeCompleted(*target, 1);
  ASSERT_TRUE(broker.SubmitCompletedFrame(older));

  auto second_target = broker.TryAcquireWritableTarget(req);
  ASSERT_TRUE(second_target.has_value());
  second_target->layer = LeaseFrameLayer::QualityBase;
  CompletedFrameLease newer = MakeCompleted(*second_target, 2);
  ASSERT_TRUE(broker.SubmitCompletedFrame(newer));

  auto frame = broker.ConsumeNewestCompletedFrame(
      TargetGeneration{gen, 7, 0, 100}, LeaseFrameLayer::QualityBase);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->preview_generation, 2u);
  EXPECT_GE(broker.DiagnosticsSnapshot().dropped_stale_frame_count, 1u);
  frame->presentation_request_id = 42;
  EXPECT_TRUE(broker.AcknowledgeFramePresented(*frame));
  EXPECT_FALSE(broker.AcknowledgeFramePresented(*frame));
  broker.CompleteRendererConsumption(*frame);
  EXPECT_EQ(broker.DiagnosticsSnapshot().last_presented_image_generation, 7u);
  EXPECT_EQ(broker.DiagnosticsSnapshot().last_presented_request_id, 42u);
  EXPECT_EQ(broker.DiagnosticsSnapshot().presented_frame_count, 1u);
}

TEST(FramePresentationBrokerTest, DroppedCompletedFramesReturnTargetsToAvailable) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.InvalidateImageGeneration(1, 1);
  broker.InvalidateTargetGeneration();
  const auto gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xA0, gen, 1, 1)));
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xA1, gen, 1, 1)));
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xA2, gen, 1, 1)));

  WritableTargetRequest req;
  req.dimensions = {64, 48};
  req.image_generation = 1;
  req.image_identity = 1;
  req.layer = LeaseFrameLayer::InteractivePrimary;

  std::uint64_t preview = 1;
  for (int round = 0; round < 6; ++round) {
    auto t1 = broker.TryAcquireWritableTarget(req);
    ASSERT_TRUE(t1.has_value()) << "round " << round << " first acquire";
    ASSERT_TRUE(broker.SubmitCompletedFrame(
        MakeCompleted(*t1, preview++, LeaseFrameLayer::InteractivePrimary)));
    auto t2 = broker.TryAcquireWritableTarget(req);
    ASSERT_TRUE(t2.has_value()) << "round " << round << " second acquire";
    ASSERT_TRUE(broker.SubmitCompletedFrame(
        MakeCompleted(*t2, preview++, LeaseFrameLayer::InteractivePrimary)));
    const auto newest = broker.ConsumeNewestCompletedFrame(
        TargetGeneration{gen, 1, 0, 1}, LeaseFrameLayer::InteractivePrimary);
    ASSERT_TRUE(newest.has_value());
    // Older completed frame must have been recycled so the pool does not drain.
    EXPECT_EQ(broker.DiagnosticsSnapshot().live_target_count, 3u) << "round " << round;
    broker.CompleteRendererConsumption(*newest);
  }
}

TEST(FramePresentationBrokerTest, LateOlderPreviewIsRejectedEvenIfSubmittedLater) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.InvalidateImageGeneration(5, 50);
  broker.InvalidateTargetGeneration();
  const auto gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xB0, gen, 5, 50)));
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xB1, gen, 5, 50)));

  WritableTargetRequest req;
  req.dimensions = {64, 48};
  req.image_generation = 5;
  req.image_identity = 50;
  req.layer = LeaseFrameLayer::QualityBase;

  auto newer_target = broker.TryAcquireWritableTarget(req);
  ASSERT_TRUE(newer_target.has_value());
  ASSERT_TRUE(broker.SubmitCompletedFrame(
      MakeCompleted(*newer_target, 20, LeaseFrameLayer::QualityBase)));

  auto older_target = broker.TryAcquireWritableTarget(req);
  ASSERT_TRUE(older_target.has_value());
  // Older preview arrives after newer — must be rejected.
  EXPECT_FALSE(broker.SubmitCompletedFrame(
      MakeCompleted(*older_target, 10, LeaseFrameLayer::QualityBase)));

  const auto frame = broker.ConsumeNewestCompletedFrame(
      TargetGeneration{gen, 5, 0, 50}, LeaseFrameLayer::QualityBase);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->preview_generation, 20u);
}

TEST(FramePresentationBrokerTest, ImageIdentityRejectsPriorSessionAfterAToBToA) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.InvalidateImageGeneration(1, 10);  // first open of image 10
  broker.InvalidateTargetGeneration();
  auto gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xC0, gen, 1, 10)));

  WritableTargetRequest req;
  req.dimensions = {64, 48};
  req.image_generation = 1;
  req.image_identity = 10;
  req.layer = LeaseFrameLayer::InteractivePrimary;
  auto first = broker.TryAcquireWritableTarget(req);
  ASSERT_TRUE(first.has_value());

  // Switch to B then back to A with new session generation.
  broker.InvalidateImageGeneration(2, 20);
  broker.InvalidateImageGeneration(3, 10);
  // Late frame from first A session must be rejected.
  EXPECT_FALSE(broker.SubmitCompletedFrame(
      MakeCompleted(*first, 1, LeaseFrameLayer::InteractivePrimary)));

  broker.InvalidateTargetGeneration();
  gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xC1, gen, 3, 10)));
  req.image_generation = 3;
  auto second = broker.TryAcquireWritableTarget(req);
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(broker.SubmitCompletedFrame(
      MakeCompleted(*second, 2, LeaseFrameLayer::InteractivePrimary)));
  const auto frame = broker.ConsumeNewestCompletedFrame(
      TargetGeneration{gen, 3, 0, 10}, LeaseFrameLayer::InteractivePrimary);
  ASSERT_TRUE(frame.has_value());
  EXPECT_EQ(frame->generation.image_generation, 3u);
}

TEST(FramePresentationBrokerTest, InvalidateDoesNotDestroyProducerWritingUntilAbandon) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.InvalidateImageGeneration(1, 1);
  broker.InvalidateTargetGeneration();
  const auto gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xD0, gen, 1, 1)));
  auto lease = broker.TryAcquireWritableTarget();
  ASSERT_TRUE(lease.has_value());
  EXPECT_EQ(broker.DiagnosticsSnapshot().producer_writing_count, 1u);

  broker.InvalidateTargetGeneration();
  // Still held by producer — not yet in release queue.
  EXPECT_TRUE(broker.DrainReleasedTargets().empty());
  EXPECT_EQ(broker.DiagnosticsSnapshot().producer_writing_count, 1u);

  broker.AbandonProducerWrite(*lease);
  EXPECT_EQ(broker.DrainReleasedTargets().size(), 1u);
  EXPECT_EQ(broker.DiagnosticsSnapshot().live_target_count, 0u);
}

TEST(FramePresentationBrokerTest, InvalidatesGenerationsAndNeverBlocksHiddenProducer) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.InvalidateImageGeneration(9, 9);
  broker.InvalidateTargetGeneration();
  const auto gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0x20, gen, 9, 9)));
  auto writing = broker.TryAcquireWritableTarget();
  ASSERT_TRUE(writing.has_value());

  broker.SetConsumerAvailable(false);
  EXPECT_FALSE(broker.TryAcquireWritableTarget().has_value());
  // Producer-writing target waits for abandon; then release queue drains.
  broker.AbandonProducerWrite(*writing);
  EXPECT_EQ(broker.DiagnosticsSnapshot().live_target_count, 0u);
  EXPECT_EQ(broker.DrainReleasedTargets().size(), 1u);

  broker.SetConsumerAvailable(true);
  broker.InvalidateImageGeneration(10, 10);
  broker.InvalidateTargetGeneration();
  const auto gen2 = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0x21, gen2, 10, 10)));
  broker.InvalidateImageGeneration(11, 11);
  EXPECT_FALSE(broker.TryAcquireWritableTarget().has_value());
}

TEST(FramePresentationBrokerTest, RejectsPriorImageAfterGenerationChange) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.InvalidateImageGeneration(20, 20);
  broker.InvalidateTargetGeneration();
  const auto gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0x30, gen, 20, 20)));

  broker.InvalidateImageGeneration(21, 21);
  EXPECT_FALSE(broker.PublishWritableTarget(MakeTestLease(0x31, gen, 20, 20)));
  EXPECT_EQ(broker.CurrentImageGeneration(), 21u);
  EXPECT_GE(broker.DiagnosticsSnapshot().dropped_stale_frame_count, 1u);
}

TEST(FramePresentationBrokerTest, AcquireRequestRecordsPendingWhenNoSizeMatch) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.InvalidateImageGeneration(1, 1);
  broker.InvalidateTargetGeneration();
  const auto gen = broker.CurrentTargetGeneration();
  ASSERT_TRUE(broker.PublishWritableTarget(MakeTestLease(0xE0, gen, 1, 1, 64, 48)));

  WritableTargetRequest req;
  req.dimensions = {128, 96};  // different size
  req.image_generation = 1;
  req.image_identity = 1;
  EXPECT_FALSE(broker.TryAcquireWritableTarget(req).has_value());
  const auto pending = broker.DrainTargetRequests();
  ASSERT_FALSE(pending.empty());
  EXPECT_EQ(pending.front().dimensions.width, 128);
}

TEST(FramePresentationBrokerTest, TargetRequestSurvivesInitialConsumerExposure) {
  FramePresentationBroker broker(EditorBackend::Cuda);
  broker.SetConsumerAvailable(false);

  WritableTargetRequest request;
  request.dimensions = {1536, 1024};
  request.image_generation = 4;
  request.image_identity = 12;
  broker.NoteTargetRequest(request);

  broker.SetConsumerAvailable(true);
  const auto pending = broker.DrainTargetRequests();
  ASSERT_EQ(pending.size(), 1u);
  EXPECT_EQ(pending.front().dimensions.width, 1536);
  EXPECT_EQ(pending.front().image_generation, 4u);
}

TEST(FramePresentationBrokerTest, BackendMismatchRejectsPublish) {
  FramePresentationBroker broker(EditorBackend::OpenCl);
  auto lease = MakeTestLease(0xF0, 1, 1, 1);
  lease.backend = EditorBackend::Cuda;
  EXPECT_FALSE(broker.PublishWritableTarget(lease));
  EXPECT_EQ(broker.DrainReleasedTargets().size(), 1u);
}

TEST(NativeResourceCountersTest, LiveTotalTracksCreateDestroy) {
  auto& counters = NativeResourceCounters::Instance();
  counters.ResetForTest();
  EXPECT_EQ(counters.LiveTotal(), 0);
  counters.OnCreateSharedTexture();
  counters.OnCreateImportedQRhiTexture();
  EXPECT_EQ(counters.LiveTotal(), 2);
  counters.OnDestroySharedTexture();
  counters.OnDestroyImportedQRhiTexture();
  EXPECT_EQ(counters.LiveTotal(), 0);
}

TEST(HarnessFixturesTest, SmallRealRawFixturePathIsStable) {
  const auto path = SmallRealRawFixtureRelativePath();
  EXPECT_FALSE(path.empty());
  EXPECT_NE(path.find(".ARW"), std::string::npos);
}

TEST(FramePresentationBrokerTest, WritableAndSyncFieldsAreDistinctOnLease) {
  auto lease = MakeTestLease(0x50, 1, 1, 1);
  lease.writable_resource = 0xABCDu;
  lease.sync_object = 0x1234u;
  EXPECT_NE(lease.writable_resource, lease.sync_object);
  EXPECT_NE(lease.writable_resource, lease.native_handle);
  EXPECT_EQ(lease.writable_kind, LeaseWritableResourceKind::CudaArray);
}

}  // namespace
}  // namespace alcedo::editor_rhi
