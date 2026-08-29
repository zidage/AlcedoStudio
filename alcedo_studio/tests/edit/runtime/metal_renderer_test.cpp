//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_renderer.hpp"

#include <gtest/gtest.h>

#include <alcedo/metal/Metal.hpp>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "../graph/test_camera_profile.hpp"
#include "../input/prepared_raw_test_support.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/raw_input_loader.hpp"
#include "edit/scope/detail/scope_metal_shared.hpp"
#include "edit/scope/final_display_frame_tap.hpp"
#include "edit/scope/scope_analyzer.hpp"
#include "image/image_buffer.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
namespace {

auto HasMetalDevice() -> bool {
  try {
    return BindSystemDefaultMetalPresentationDevice() != nullptr;
  } catch (...) {
    return false;
  }
}

auto MakeEncodedImage(std::uint8_t tag) -> std::shared_ptr<ImageBuffer> {
  std::vector<std::uint8_t> bytes(64, tag);
  bytes[0] = tag;
  bytes[1] = 0x5A;
  return std::make_shared<ImageBuffer>(std::move(bytes));
}

auto MakeUnpacker() -> PreparedSourceCache::UnpackFn {
  return [](std::span<const std::byte>, DecodeRes decode_res) {
    const auto pattern = gpu_dag_test::MakeRggbPattern();
    return RawInputLoader::FromUnpackedCfa(gpu_dag_test::MakeU16CfaPlane(32, 32, pattern), pattern,
                                           gpu_dag_test::DefaultLinearization(),
                                           gpu_dag_test::FullSensor(32, 32), decode_res);
  };
}

enum class PresentEvent {
  Bind,
  Ensure,
  Map,
  SubmitFinal,
  SubmitMetal,
  SubmitHost,
  Unmap,
  Notify,
};

class RecordingMetalPresentSink : public IFrameSink {
 public:
  void EnsureSize(int width, int height) override {
    events_.push_back(PresentEvent::Ensure);
    width_  = width;
    height_ = height;
  }

  auto MapResourceForWrite(FrameMemoryDomain) -> FrameWriteMapping override {
    events_.push_back(PresentEvent::Map);
    return {};
  }

  void UnmapResource() override { events_.push_back(PresentEvent::Unmap); }

  void BindFrameSubmission(const FrameCompletionSubmission& submission) override {
    events_.push_back(PresentEvent::Bind);
    last_bound_ = submission;
  }

  void SubmitHostFrame(const ViewerFrame&) override { events_.push_back(PresentEvent::SubmitHost); }

  void SubmitMetalFrame(const ViewerMetalFrame& frame) override {
    events_.push_back(PresentEvent::SubmitMetal);
    last_metal_ = frame;
    if (last_frame_.ready_signal.resource) {
      const auto* signal = static_cast<scope::metal_detail::MetalCommandBufferSignalResource*>(
          last_frame_.ready_signal.resource.get());
      viewer_texture_completed_before_submit_ =
          signal != nullptr && signal->command_buffer &&
          signal->command_buffer->status() == MTL::CommandBufferStatusCompleted;
    }
  }

  void SubmitFinalDisplayFrame(const FinalDisplayFrameView& frame) override {
    events_.push_back(PresentEvent::SubmitFinal);
    last_frame_ = frame;
    ++submit_count_;
  }

  void NotifyFrameReady(const FrameCompletionSubmission& submission) override {
    events_.push_back(PresentEvent::Notify);
    last_notified_ = submission;
  }

  auto                      GetWidth() const -> int override { return width_; }
  auto                      GetHeight() const -> int override { return height_; }

  std::vector<PresentEvent> events_;
  FinalDisplayFrameView     last_frame_{};
  ViewerMetalFrame          last_metal_{};
  FrameCompletionSubmission last_bound_{};
  FrameCompletionSubmission last_notified_{};
  int                       submit_count_                           = 0;
  bool                      viewer_texture_completed_before_submit_ = false;

 private:
  int width_  = 0;
  int height_ = 0;
};

class ThrowingMetalPresentSink final : public RecordingMetalPresentSink {
 public:
  void SubmitMetalFrame(const ViewerMetalFrame&) override {
    throw std::runtime_error("present sink rejected the Metal frame");
  }
};

class MetalRendererFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!HasMetalDevice()) {
      GTEST_SKIP() << "No Metal device available.";
    }
    (void)BindSystemDefaultMetalPresentationDevice();
    document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
    gpu_dag_test::EnsureTestCameraProfile(*document_);
    renderer_ = std::make_unique<MetalRenderer>(document_, MakeUnpacker());
    image_    = MakeEncodedImage(91);
  }

  auto RenderHost(bool session = true) -> std::shared_ptr<ImageBuffer> {
    return renderer_->Render(
        image_, DecodeRes::FULL, RenderRequest{}, nullptr, {}, true,
        session ? RenderCachePolicy::UseSessionCache : RenderCachePolicy::BypassSessionCache);
  }

  auto RenderTo(IFrameSink& sink, const FrameCompletionSubmission& submission = {})
      -> std::shared_ptr<ImageBuffer> {
    return renderer_->Render(image_, DecodeRes::FULL, RenderRequest{}, &sink, submission, false);
  }

  std::shared_ptr<PipelineDocument> document_;
  std::unique_ptr<MetalRenderer>    renderer_;
  std::shared_ptr<ImageBuffer>      image_;
};

TEST_F(MetalRendererFixture, MetalRendererPresentsWorkspaceTextureWithoutHostDownload) {
  RecordingMetalPresentSink sink;
  FrameCompletionSubmission submission;
  submission.metadata.image_identity          = 18;
  submission.metadata.session_epoch           = 4;
  submission.metadata.presentation_request_id = 249;
  ASSERT_NE(RenderTo(sink, submission), nullptr);

  const std::vector<PresentEvent> expected = {PresentEvent::Bind, PresentEvent::Ensure,
                                              PresentEvent::SubmitFinal, PresentEvent::SubmitMetal,
                                              PresentEvent::Notify};
  EXPECT_EQ(sink.events_, expected);
  EXPECT_EQ(sink.submit_count_, 1);
  EXPECT_EQ(sink.last_bound_.metadata.presentation_request_id, 249U);
  EXPECT_EQ(sink.last_notified_.metadata.presentation_request_id, 249U);
  ASSERT_TRUE(static_cast<bool>(sink.last_frame_));
  EXPECT_EQ(sink.last_frame_.image.backend, GpuBackend::Metal);
  EXPECT_EQ(sink.last_frame_.format, FramePixelFormat::RGBA32F);
  EXPECT_EQ(sink.last_frame_.domain, AnalysisDomain::DisplayEncoded);
  EXPECT_GT(sink.last_frame_.width, 0);
  EXPECT_GT(sink.last_frame_.height, 0);
  ASSERT_TRUE(static_cast<bool>(sink.last_metal_));
  EXPECT_EQ(sink.last_metal_.texture_handle,
            sink.last_frame_.image.resource
                ? static_cast<scope::metal_detail::MetalTextureImageResource*>(
                      sink.last_frame_.image.resource.get())
                      ->native_object
                : 0U);
  EXPECT_TRUE(sink.viewer_texture_completed_before_submit_)
      << "Qt Quick must not import a Metal texture before the DAG command buffer completes";

  renderer_->Device().WaitIdle();
  auto* display =
      renderer_->Device().Workspace().Images().Find(GraphValueId{NodeId{"drt"}, PortId{"display"}});
  ASSERT_NE(display, nullptr);
  EXPECT_EQ(sink.last_metal_.texture_handle,
            reinterpret_cast<std::uintptr_t>(display->Texture().Native()));
}

TEST_F(MetalRendererFixture, MetalRoiKeepsNativePixelsWhenViewportTargetIsLarger) {
  RecordingMetalPresentSink sink;
  RenderRequest             request;
  request.view.visible_rect_in_edit_space = NormalizedRect{0.25f, 0.25f, 0.25f, 0.25f};
  request.view.viewport_extent            = Extent2D{100, 100};

  ASSERT_NE(renderer_->Render(image_, DecodeRes::FULL, request, &sink, {}, false), nullptr);

  // The prepared Bayer fixture has a 24x24 full-reference image after the demosaic border.
  // Its quarter-size ROI therefore contains 6x6 native pixels. The viewer may enlarge those
  // pixels with nearest-neighbor sampling; the Metal DAG must not interpolate them to 100x100.
  EXPECT_EQ(sink.GetWidth(), 6);
  EXPECT_EQ(sink.GetHeight(), 6);
  EXPECT_EQ(sink.last_metal_.width, 6);
  EXPECT_EQ(sink.last_metal_.height, 6);
}

TEST_F(MetalRendererFixture, MetalScopeTapUsesTheFinalDisplayTextureAndSubmissionSignal) {
  RecordingMetalPresentSink downstream;
  FinalDisplayFrameTapSink  tap(&downstream, nullptr);
  FrameCompletionSubmission submission;
  submission.metadata.image_identity          = 7;
  submission.metadata.session_epoch           = 2;
  submission.metadata.presentation_request_id = 88;
  submission.metadata.scope_update_allowed    = true;
  ASSERT_NE(RenderTo(tap, submission), nullptr);

  const auto frame = tap.GetCurrentDisplayFrameView();
  ASSERT_TRUE(static_cast<bool>(frame));
  EXPECT_EQ(frame.image.backend, GpuBackend::Metal);
  EXPECT_EQ(frame.ready_signal.backend, GpuBackend::Metal);
  ASSERT_NE(frame.ready_signal.resource, nullptr);
  const auto* signal = static_cast<scope::metal_detail::MetalCommandBufferSignalResource*>(
      frame.ready_signal.resource.get());
  ASSERT_NE(signal, nullptr);
  EXPECT_NE(signal->command_buffer.get(), nullptr);
  ASSERT_TRUE(static_cast<bool>(downstream.last_metal_));
  const auto* image =
      static_cast<scope::metal_detail::MetalTextureImageResource*>(frame.image.resource.get());
  ASSERT_NE(image, nullptr);
  EXPECT_EQ(downstream.last_metal_.texture_handle, image->native_object);
}

TEST_F(MetalRendererFixture, MetalOneShotRenderDoesNotPublishIntoSessionCache) {
  ASSERT_NE(RenderHost(true), nullptr);
  const auto session_before = renderer_->SessionResources();
  EXPECT_GT(session_before.published_result_count, 0U);
  renderer_->ResetStats();

  ASSERT_NE(RenderHost(false), nullptr);
  EXPECT_EQ(renderer_->OneShotPublishedResultCount(), 0U);
  EXPECT_EQ(renderer_->SessionResources().published_result_count,
            session_before.published_result_count);
  EXPECT_EQ(renderer_->SessionResources().prepared_source_entry_count,
            session_before.prepared_source_entry_count);
  EXPECT_EQ(renderer_->Stats().prepared_source_hits, 0U);
  EXPECT_EQ(renderer_->Stats().plan_cache_hits, 0U);
  EXPECT_EQ(renderer_->Stats().pass.sensor_develop_execute, 0U);
}

TEST_F(MetalRendererFixture, MetalPipelineReturnReleasesSessionResourcesAfterGpuCompletion) {
  ASSERT_NE(RenderHost(true), nullptr);
  EXPECT_GT(renderer_->SessionResources().published_result_count, 0U);
  renderer_->ReleaseSessionCaches();
  EXPECT_EQ(renderer_->SessionResources().published_result_count, 0U);
  EXPECT_EQ(renderer_->SessionResources().prepared_source_entry_count, 0U);
  EXPECT_TRUE(renderer_->SessionResources().session_value_ids.empty());
  EXPECT_FALSE(renderer_->Device().Workspace().IsRendering());
}

TEST_F(MetalRendererFixture, MetalBackendFailureDoesNotEnterCpuOrLegacyMetalExecution) {
  ASSERT_NE(RenderHost(true), nullptr);
  const auto published_before    = renderer_->SessionResources().published_result_count;
  auto       develop             = document_->Develop()->Params().Params();
  develop.highlights_reconstruct = !develop.highlights_reconstruct;
  document_->Develop()->Params().ReplaceParams(develop);
  renderer_->Device().Workspace().Device().FailNextUpload();
  EXPECT_THROW(RenderHost(true), std::runtime_error);
  EXPECT_EQ(renderer_->SessionResources().published_result_count, published_before);
  EXPECT_FALSE(renderer_->Device().Workspace().IsRendering());

  ThrowingMetalPresentSink sink;
  EXPECT_THROW(RenderTo(sink), std::runtime_error);
  EXPECT_EQ(renderer_->Device().Workspace().Images().UnpublishedCount(), 0U);
}

TEST_F(MetalRendererFixture, MetalRealRawEditorUsesTheThreeNodeDag) {
  EXPECT_EQ(document_->Graph().Nodes().size(), 3U);
  EXPECT_NE(document_->Develop(), nullptr);
  EXPECT_NE(document_->PrimaryGrade(), nullptr);
  EXPECT_NE(document_->Drt(), nullptr);
  auto host = RenderHost(true);
  ASSERT_NE(host, nullptr);
  EXPECT_EQ(renderer_->Stats().pass.drt_execute, 1U);
  EXPECT_EQ(document_->Graph().Nodes().size(), 3U);
}

}  // namespace
}  // namespace alcedo
