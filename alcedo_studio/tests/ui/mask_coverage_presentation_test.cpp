//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file mask_coverage_presentation_test.cpp
/// @brief SM8.5: the dedicated coverage presentation channel of the editor viewport.
///
/// Each test runs the production path end to end with real resources of one backend through the
/// harness of mask_coverage_presentation_test_support.hpp, and compares the imported coverage with
/// the independent host reference of the raster Mask rule.

#include <functional>
#include <string>
#include <vector>

#include "edit/scope/final_display_frame_tap.hpp"
#include "mask_coverage_presentation_test_support.hpp"

namespace alcedo::editor_rhi {
namespace {

using namespace coverage_test;

/// Forwards to the viewport sink and switches the viewport to the next image after the device
/// write and before publication: a coverage frame that is in flight across an image change.
class ImageChangeBeforePublicationSink final : public IMaskCoverageSink {
 public:
  ImageChangeBeforePublicationSink(IMaskCoverageSink& sink, std::function<void()> change_image)
      : sink_(sink), change_image_(std::move(change_image)) {}

  auto MapCoverageTarget(const MaskCoveragePresentation& presentation, int width, int height,
                         FrameMemoryDomain domain) -> FrameWriteMapping override {
    return sink_.MapCoverageTarget(presentation, width, height, domain);
  }
  void UnmapCoverageTarget() override {
    sink_.UnmapCoverageTarget();
    change_image_();
  }
  auto PublishCoverage() -> MaskCoveragePublication override { return sink_.PublishCoverage(); }
  void AbandonCoverageTarget() override { sink_.AbandonCoverageTarget(); }
  void ClearCoverage() override { sink_.ClearCoverage(); }
#if defined(HAVE_METAL)
  auto SubmitMetalCoverage(const MaskCoveragePresentation& presentation,
                           const MaskCoverageMetalFrame&   frame)
      -> MaskCoveragePublication override {
    change_image_();
    return sink_.SubmitMetalCoverage(presentation, frame);
  }
#endif

 private:
  IMaskCoverageSink&    sink_;
  std::function<void()> change_image_;
};

class MaskCoveragePresentationTest : public CoverageChannelTest {};

auto SlotStates(const DirectPresentQueue& queue) -> std::vector<std::string> {
  std::vector<std::string> states;
  for (int i = 0; i < DirectPresentQueue::kSlotCount; ++i) {
    const auto slot = queue.SlotAt(i);
    states.push_back(std::to_string(static_cast<int>(slot->state)) + "/" +
                     std::to_string(slot->native.native_handle) + "/" +
                     std::to_string(slot->width) + "x" + std::to_string(slot->height) + "/" +
                     std::to_string(slot->session_epoch) + "/" +
                     std::to_string(slot->preview_metadata.presentation_request_id));
  }
  return states;
}

TEST_F(MaskCoveragePresentationTest, CoverageTransferLeavesPhotoSlotsUnchanged) {
  // A real photo target in the photo queue, as after the first photo frame.
  auto& photo_queue = *viewport_->present_queue();
  if (UsesQueuedTargets()) {
    const auto prepared = photo_queue.PrepareWrite(96, 64, kEpoch, kImage);
    ASSERT_TRUE(prepared.ok);
    auto photo =
        adapter_->CreateTarget(rhi_.get(), QSize(96, 64), TargetGeneration{1, kEpoch, 0, kImage},
                               LeaseFrameLayer::QualityBase);
    ASSERT_TRUE(photo.has_value()) << adapter_->lastError();
    DirectPresentQueue::SlotNative native;
    native.backend           = photo->backend;
    native.handle_kind       = photo->handle_kind;
    native.writable_kind     = photo->writable_kind;
    native.native_handle     = photo->native_handle;
    native.writable_resource = photo->writable_resource;
    ASSERT_TRUE(
        photo_queue.PublishCreatedSlot(prepared.slot_index, 96, 64, native, kEpoch, kImage));
  }
  const auto                 photo_slots_before  = SlotStates(photo_queue);
  const auto                 photo_diag_before   = photo_queue.DiagnosticsSnapshot();
  const auto                 photo_frames_before = viewport_->frameSink()->submitted_frame_count();

  const auto                 pixels              = MakeCandidatePixels(0);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(11)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());

  ExpectDisplayed(ExpectedCoverageR8(pixels, request), "candidate coverage");
  EXPECT_EQ(layer_.Presentation().identity.request_id, 11u);
  EXPECT_EQ(layer_.Presentation().geometry.render_extent, request.geometry.render_extent);
  EXPECT_EQ(layer_.Texture()->pixelSize(),
            QSize(static_cast<int>(request.geometry.render_extent.width),
                  static_cast<int>(request.geometry.render_extent.height)));

  // The photo queue, its native target, and the photo sink are untouched.
  EXPECT_EQ(SlotStates(photo_queue), photo_slots_before);
  const auto photo_diag_after = photo_queue.DiagnosticsSnapshot();
  EXPECT_EQ(photo_diag_after.live_target_count, photo_diag_before.live_target_count);
  EXPECT_EQ(photo_diag_after.ready_count, 0u);
  EXPECT_EQ(photo_diag_after.dropped_stale_frame_count,
            photo_diag_before.dropped_stale_frame_count);
  EXPECT_EQ(viewport_->frameSink()->submitted_frame_count(), photo_frames_before);
  if (UsesQueuedTargets()) {
    EXPECT_EQ(layer_.NativeTargetCount(), 1u);
    EXPECT_GE(layer_.SlotIndex(), 0);
  }
}

TEST_F(MaskCoveragePresentationTest, OldCoverageLivesUntilRendererRelease) {
  const auto                 first  = MakeCandidatePixels(0);
  const auto                 second = MakeCandidatePixels(2);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(first), request, Identity(21)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  auto*      first_texture   = layer_.Texture();
  const int  first_slot      = layer_.SlotIndex();
  const auto imported_before = NativeResourceCounters::Instance().LiveImportedQRhiTextures();

  // The replacement completes, but the renderer has not taken it yet.
  ASSERT_EQ(Present(Candidate(second), request, Identity(22)), MaskCoveragePublication::Published);
  EXPECT_EQ(layer_.Texture(), first_texture);
  EXPECT_EQ(layer_.Presentation().identity.request_id, 21u);
  ExpectDisplayed(ExpectedCoverageR8(first, request), "displayed coverage before the import");
  if (UsesQueuedTargets()) {
    const auto& queue = *viewport_->maskCoverageQueue();
    EXPECT_EQ(queue.SlotAt(first_slot)->state, DirectPresentQueue::SlotState::RendererReading);
    EXPECT_EQ(queue.DiagnosticsSnapshot().ready_count, 1u);
    EXPECT_EQ(queue.DiagnosticsSnapshot().live_target_count, 2u)
        << "the replacement must use a second target";
  } else {
    // Metal: the displayed frame holds its display target, so the replacement used another one.
    EXPECT_EQ(Counters().display_targets, 2u);
  }

  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 22u);
  ExpectDisplayed(ExpectedCoverageR8(second, request), "replacement coverage");
  // The previous import was destroyed after the replacement was imported.
  EXPECT_EQ(NativeResourceCounters::Instance().LiveImportedQRhiTextures(), imported_before);
  if (UsesQueuedTargets()) {
    EXPECT_EQ(viewport_->maskCoverageQueue()->SlotAt(first_slot)->state,
              DirectPresentQueue::SlotState::Available);
    EXPECT_NE(layer_.SlotIndex(), first_slot);
  } else {
    // The renderer released the first frame: the next write reuses its display target.
    const auto creates = Counters().display_target_create;
    ASSERT_EQ(Present(Candidate(first), request, Identity(23)), MaskCoveragePublication::Published);
    EXPECT_EQ(Counters().display_target_create, creates);
    ASSERT_TRUE(Consume());
    ExpectDisplayed(ExpectedCoverageR8(first, request), "coverage in the reused target");
  }

  layer_.Release();
  EXPECT_EQ(layer_.Texture(), nullptr);
  EXPECT_EQ(NativeResourceCounters::Instance().LiveImportedQRhiTextures(), imported_before - 1);
}

TEST_F(MaskCoveragePresentationTest, CoverageTransferDoesNotReadBackToHost) {
  const auto                 pixels = MakeCandidatePixels(1);
  MaskCoverageDisplayRequest plain{MakePhotoGeometry(1.0f), false, 1.0f};
  const auto                 start = Counters();
  ASSERT_EQ(Present(Candidate(pixels), plain, Identity(31)), MaskCoveragePublication::Published);
  const auto first = Counters();
  // The only host transfer is the original coverage upload into the device.
  EXPECT_EQ(first.device_to_host, start.device_to_host);
  EXPECT_EQ(first.host_to_device - start.host_to_device, 1u);

  // The same candidate with another invert and opacity: no upload and no readback.
  MaskCoverageDisplayRequest inverted{plain.geometry, true, 0.5f};
  ASSERT_EQ(Present(Candidate(pixels), inverted, Identity(32)), MaskCoveragePublication::Published);
  const auto second = Counters();
  EXPECT_EQ(second.device_to_host, start.device_to_host);
  EXPECT_EQ(second.host_to_device, first.host_to_device);
  ASSERT_TRUE(Consume());
  ExpectDisplayed(ExpectedCoverageR8(pixels, inverted), "inverted half-opacity coverage");
  if (UsesQueuedTargets()) {
    // The target is a device resource shared with the graphics API.
    const auto slot = viewport_->maskCoverageQueue()->SlotAt(layer_.SlotIndex());
    EXPECT_EQ(slot->native.writable_kind, g_backend == EditorBackend::Cuda
                                              ? LeaseWritableResourceKind::CudaArray
                                              : LeaseWritableResourceKind::OpenClImage);
    // CUDA and OpenCL copy into the native target and keep one display target.
    EXPECT_EQ(second.display_targets, 1u);
  }
}

TEST_F(MaskCoveragePresentationTest, CoverageChannelClosesBeforeImageReplacement) {
  const auto                 pixels = MakeCandidatePixels(0);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(41)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());

  // A frame of the current image is in flight when the viewport switches image.
  ImageChangeBeforePublicationSink switching(*viewport_->maskCoverageSink(), [this] {
    QMetaObject::invokeMethod(
        viewport_.get(),
        [this] {
          viewport_->setImageIdentity(kNextImage);
          viewport_->setSessionEpoch(kNextEpoch);
        },
        Qt::BlockingQueuedConnection);
  });
  EXPECT_EQ(Present(Candidate(MakeCandidatePixels(2)), request, Identity(42), &switching),
            MaskCoveragePublication::DiscardedForImageChange);
  // What EditorViewportRenderer::synchronize does on the session change.
  layer_.Release();
  EXPECT_FALSE(Consume()) << "no frame of the previous image may be imported";
  EXPECT_EQ(layer_.Texture(), nullptr);
  EXPECT_EQ(viewport_->maskCoverageQueue()->DiagnosticsSnapshot().ready_count, 0u);

  // A late frame for the previous image is refused before any device-to-target copy.
  EXPECT_EQ(Present(Candidate(pixels), request, Identity(43)),
            MaskCoveragePublication::DiscardedForImageChange);
  EXPECT_FALSE(Consume());

  // The new image starts with its own coverage.
  const auto next = MakeCandidatePixels(1);
  ASSERT_EQ(Present(Candidate(next), request, {44, kNextEpoch, kNextImage}),
            MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.image_identity, kNextImage);
  ExpectDisplayed(ExpectedCoverageR8(next, request), "coverage of the next image");
}

TEST_F(MaskCoveragePresentationTest, CoverageDoesNotReplaceScopePhotoInput) {
  // Scope analysis reads the current final photo frame of the tap in front of the photo sink
  // (SubmitCurrentDisplayFrameToScope). The frame stands for the last presented photo; its image
  // is not read here.
  FinalDisplayFrameTapSink tap(viewport_->frameSink(), nullptr);
  tap.SetFrameIdentity(kImage, kEpoch);
  FinalDisplayFrameView photo;
  photo.image.resource = std::make_shared<int>(1);
  photo.image.width    = 96;
  photo.image.height   = 64;
  photo.width          = 96;
  photo.height         = 64;
  tap.SubmitFinalDisplayFrame(photo);
  const auto display_before = tap.GetCurrentDisplayFrameView();
  ASSERT_TRUE(static_cast<bool>(display_before));

  const auto                 pixels = MakeCandidatePixels(1);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(51)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());

  const auto display_after = tap.GetCurrentDisplayFrameView();
  EXPECT_EQ(display_after.frame_id, display_before.frame_id);
  EXPECT_EQ(display_after.image.resource, display_before.image.resource);
  EXPECT_EQ(display_after.width, 96);
  EXPECT_EQ(viewport_->frameSink()->submitted_frame_count(), 0u);
  EXPECT_EQ(viewport_->present_queue()->DiagnosticsSnapshot().ready_count, 0u);
}

TEST_F(MaskCoveragePresentationTest, FailedTargetCreationKeepsDisplayedCoverage) {
  const auto                 pixels = MakeCandidatePixels(0);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(pixels), request, Identity(61)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  // A resized photo frame needs a new display target (and, for CUDA and OpenCL, a new viewport
  // target).
  MaskCoverageDisplayRequest resized{MakePhotoGeometry(0.5f), false, 1.0f};
  const auto                 targets_before = Counters().display_targets;

  // The device cannot allocate the display target.
  render_thread_.Post([this] { producer_->FailNextTextureAllocation(); }).get();
  try {
    (void)Present(Candidate(pixels), resized, Identity(62));
    ADD_FAILURE() << "the failed display target allocation was not reported";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("Mask coverage display: allocating"),
              std::string::npos)
        << error.what();
  }
  EXPECT_EQ(Counters().display_targets, targets_before);
  EXPECT_FALSE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 61u);
  ExpectDisplayed(ExpectedCoverageR8(pixels, request), "coverage kept after the device failure");

  if (UsesQueuedTargets()) {
    // The viewport render thread cannot create the coverage target.
    UnsupportedLeaseTargetAdapter failing(g_backend);
    EXPECT_THROW(
        (void)Present(Candidate(MakeCandidatePixels(2)), resized, Identity(63), nullptr, &failing),
        std::runtime_error);
    EXPECT_FALSE(layer_.LastError().empty());
    EXPECT_FALSE(Consume());
    EXPECT_EQ(layer_.Presentation().identity.request_id, 61u);
    ExpectDisplayed(ExpectedCoverageR8(pixels, request), "coverage kept after the target failure");
  }

  // The channel recovers with the next frame.
  ASSERT_EQ(Present(Candidate(pixels), resized, Identity(64)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 64u);
  ExpectDisplayed(ExpectedCoverageR8(pixels, resized), "coverage after the recovery");
}

// SM8.7 ClearMaskCoveragePreview: a clear hides the displayed frame and the frame that was
// published before it but not displayed yet; a frame published after the clear is displayed.
TEST_F(MaskCoveragePresentationTest, ClearHidesOnlyFramesPublishedBeforeIt) {
  const auto                 first  = MakeCandidatePixels(0);
  const auto                 second = MakeCandidatePixels(1);
  const auto                 third  = MakeCandidatePixels(2);
  MaskCoverageDisplayRequest request{MakePhotoGeometry(1.0f), false, 1.0f};
  ASSERT_EQ(Present(Candidate(first), request, Identity(71)), MaskCoveragePublication::Published);
  ASSERT_TRUE(Consume());
  // Published but not imported when the clear arrives.
  ASSERT_EQ(Present(Candidate(second), request, Identity(72)), MaskCoveragePublication::Published);

  viewport_->maskCoverageSink()->ClearCoverage();
  EXPECT_TRUE(layer_.ApplyClear());
  EXPECT_EQ(layer_.Texture(), nullptr);
  EXPECT_FALSE(Consume()) << "a frame published before the clear must not be displayed";
  EXPECT_EQ(layer_.Texture(), nullptr);
  if (UsesQueuedTargets()) {
    // Both slots returned to the producer.
    EXPECT_EQ(viewport_->maskCoverageQueue()->DiagnosticsSnapshot().ready_count, 0u);
  }

  ASSERT_EQ(Present(Candidate(third), request, Identity(73)), MaskCoveragePublication::Published);
  EXPECT_FALSE(layer_.ApplyClear());
  ASSERT_TRUE(Consume());
  EXPECT_EQ(layer_.Presentation().identity.request_id, 73u);
  ExpectDisplayed(ExpectedCoverageR8(third, request), "coverage published after the clear");
}

}  // namespace
}  // namespace alcedo::editor_rhi

int main(int argc, char** argv) {
  return alcedo::editor_rhi::coverage_test::RunCoverageChannelTests(argc, argv);
}
