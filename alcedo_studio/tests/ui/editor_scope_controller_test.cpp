//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "edit/scope/final_display_frame_tap.hpp"

namespace alcedo::ui::test {
namespace {

class RecordingScopeAnalyzer final : public IScopeAnalyzer {
 public:
  void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest& request) override {
    submitted_frames.push_back(frame);
    if (on_submit) {
      on_submit(frame, request);
    }
  }

  auto GetLatestOutput() -> ScopeOutputSet override { return latest_output; }

  void ResizeResources(const ScopeRequest&) override { ++resize_count; }

  void ReleaseResources() override { ++release_count; }

  std::vector<FinalDisplayFrameView>                                     submitted_frames;
  ScopeOutputSet                                                         latest_output{};
  std::function<void(const FinalDisplayFrameView&, const ScopeRequest&)> on_submit;
  int                                                                    resize_count  = 0;
  int                                                                    release_count = 0;
};

class RecordingSink final : public IFrameSink {
 public:
  void EnsureSize(int width, int height) override {
    width_  = width;
    height_ = height;
  }

  auto MapResourceForWrite(FrameMemoryDomain) -> FrameWriteMapping override { return {}; }
  void UnmapResource() override {}
  void NotifyFrameReady() override {}
  int  GetWidth() const override { return width_; }
  int  GetHeight() const override { return height_; }

  void SetNextFramePreviewMetadata(const FramePreviewMetadata& metadata) override {
    last_metadata = metadata;
    ++metadata_count;
  }

  FramePreviewMetadata last_metadata{};
  int                  metadata_count = 0;

 private:
  int width_  = 0;
  int height_ = 0;
};

class DeferredRecordingScopeAnalyzer final : public IScopeAnalyzer {
 public:
  void SubmitFrame(const FinalDisplayFrameView&, const ScopeRequest&) override {
    submitted_count.fetch_add(1, std::memory_order_relaxed);
  }

  auto            GetLatestOutput() -> ScopeOutputSet override { return {}; }

  void            ResizeResources(const ScopeRequest&) override {}
  void            ReleaseResources() override {}

  std::atomic_int submitted_count = 0;
};

auto MakeFrame() -> FinalDisplayFrameView {
  FinalDisplayFrameView frame;
  frame.image.backend   = GpuBackend::Cuda;
  frame.image.resource  = std::make_shared<int>(1);
  frame.image.width     = 4;
  frame.image.height    = 4;
  frame.image.row_bytes = 4U * sizeof(float) * 4U;
  frame.width           = 4;
  frame.height          = 4;
  return frame;
}

// Stages a distinct analyzer-owned frame so tests can prove the deferred
// SubmitFrame analyzes the staged frame, not the pipeline's reused source.
class StagingRecordingAnalyzer final : public IScopeAnalyzer {
 public:
  auto StageFrame(const FinalDisplayFrameView& frame, const ScopeRequest&)
      -> FinalDisplayFrameView override {
    std::lock_guard<std::mutex> lock(mutex_);
    staged_inputs_.push_back(frame);
    FinalDisplayFrameView staged = frame;
    staged.image.resource        = std::make_shared<int>(2);  // owned marker
    staged.ready_signal.resource = std::make_shared<int>(2);
    return staged;
  }

  void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    submitted_frames_.push_back(frame);
  }

  auto               GetLatestOutput() -> ScopeOutputSet override { return {}; }
  void               ResizeResources(const ScopeRequest&) override {}
  void               ReleaseResources() override {}

  [[nodiscard]] auto staged_inputs() const -> std::vector<FinalDisplayFrameView> {
    std::lock_guard<std::mutex> lock(mutex_);
    return staged_inputs_;
  }
  [[nodiscard]] auto submitted_frames() const -> std::vector<FinalDisplayFrameView> {
    std::lock_guard<std::mutex> lock(mutex_);
    return submitted_frames_;
  }

 private:
  mutable std::mutex                 mutex_;
  std::vector<FinalDisplayFrameView> staged_inputs_;
  std::vector<FinalDisplayFrameView> submitted_frames_;
};

// SubmitFrame blocks until Release() so a session-exit drain test can prove
// Shutdown waits for the in-flight scope worker before the pipeline releases.
class BlockingSubmitAnalyzer final : public IScopeAnalyzer {
 public:
  void SubmitFrame(const FinalDisplayFrameView&, const ScopeRequest&) override {
    submit_started_.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] { return release_.load(std::memory_order_acquire); });
    submitted_done_.store(true, std::memory_order_release);
  }
  auto GetLatestOutput() -> ScopeOutputSet override { return {}; }
  void ResizeResources(const ScopeRequest&) override {}
  void ReleaseResources() override {}

  void Release() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      release_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
  }
  [[nodiscard]] auto submit_started() const -> bool {
    return submit_started_.load(std::memory_order_acquire);
  }
  [[nodiscard]] auto submitted_done() const -> bool {
    return submitted_done_.load(std::memory_order_acquire);
  }

 private:
  std::mutex              mutex_;
  std::condition_variable cv_;
  std::atomic_bool        release_{false};
  std::atomic_bool        submit_started_{false};
  std::atomic_bool        submitted_done_{false};
};

// Records the collect/submit call order and the generation each call produced
// or consumed, so a test can prove the controller collects a completed result
// before submitting the next frame (1-tick pipeline, no dropped results).
class SequencedAnalyzer final : public IScopeAnalyzer {
 public:
  void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest&) override {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_output_.generation         = ++next_gen_;
    pending_output_.image_identity     = frame.image_identity;
    pending_output_.image_generation   = frame.image_generation;
    pending_output_.display_generation = frame.display_generation;
    call_log_.push_back("submit");
    submitted_gens_.push_back(pending_output_.generation);
  }

  auto GetLatestOutput() -> ScopeOutputSet override {
    std::lock_guard<std::mutex> lock(mutex_);
    call_log_.push_back("collect");
    collected_gens_.push_back(pending_output_.generation);
    return pending_output_;
  }

  void               ResizeResources(const ScopeRequest&) override {}
  void               ReleaseResources() override {}

  [[nodiscard]] auto call_log() const -> std::vector<std::string> {
    std::lock_guard<std::mutex> lock(mutex_);
    return call_log_;
  }
  [[nodiscard]] auto collected_gens() const -> std::vector<std::uint64_t> {
    std::lock_guard<std::mutex> lock(mutex_);
    return collected_gens_;
  }
  [[nodiscard]] auto submitted_gens() const -> std::vector<std::uint64_t> {
    std::lock_guard<std::mutex> lock(mutex_);
    return submitted_gens_;
  }

 private:
  mutable std::mutex         mutex_;
  ScopeOutputSet             pending_output_{};
  std::uint64_t              next_gen_ = 0;
  std::vector<std::string>   call_log_;
  std::vector<std::uint64_t> collected_gens_;
  std::vector<std::uint64_t> submitted_gens_;
};

auto MarkerValue(const std::shared_ptr<void>& resource) -> int {
  return *std::static_pointer_cast<int>(resource);
}

auto MakeValidSnapshot(std::uint64_t generation, std::uint64_t image_identity,
                       std::uint64_t image_generation, std::uint64_t display_generation)
    -> alcedo::ScopeRenderSnapshot {
  alcedo::ScopeRenderSnapshot snapshot;
  snapshot.generation         = generation;
  snapshot.image_identity     = image_identity;
  snapshot.image_generation   = image_generation;
  snapshot.display_generation = display_generation;
  snapshot.histogram.bins     = 4;
  snapshot.histogram.rgb      = {0.10f, 0.25f, 0.50f, 0.75f};
  snapshot.histogram.valid    = true;
  return snapshot;
}

}  // namespace

TEST(EditorScopeControllerTest,
     FinalDisplayFrameTapStampsImageAndDisplayGenerationsBeforeAnalyzerSubmission) {
  auto                     analyzer = std::make_shared<RecordingScopeAnalyzer>();
  RecordingSink            downstream;
  FinalDisplayFrameTapSink tap(&downstream, analyzer);

  tap.SetFrameIdentity(91, 7);
  tap.SetScopeActive(true);
  FramePreviewMetadata metadata;
  metadata.preview_generation = 23;
  metadata.image_identity     = 91;
  metadata.image_generation   = 7;
  tap.SetNextFramePreviewMetadata(metadata);
  tap.SubmitFinalDisplayFrame(MakeFrame());

  ASSERT_EQ(analyzer->submitted_frames.size(), 1U);
  const auto& submitted = analyzer->submitted_frames.front();
  EXPECT_EQ(submitted.image_identity, 91U);
  EXPECT_EQ(submitted.image_generation, 7U);
  EXPECT_EQ(submitted.display_generation, 23U);
  EXPECT_EQ(submitted.frame_id, 23U);
  EXPECT_EQ(downstream.metadata_count, 1);
  EXPECT_EQ(downstream.last_metadata.image_identity, 91U);
  EXPECT_EQ(downstream.last_metadata.image_generation, 7U);
  EXPECT_TRUE(downstream.last_metadata.scope_update_allowed);
}

TEST(EditorScopeControllerTest, ViewOnlyFrameKeepsTheLastContentFrameForScope) {
  auto                     analyzer = std::make_shared<RecordingScopeAnalyzer>();
  FinalDisplayFrameTapSink tap(nullptr, analyzer);
  tap.SetFrameIdentity(18, 4);
  tap.SetScopeActive(true);

  FramePreviewMetadata content_metadata;
  content_metadata.preview_generation = 10;
  content_metadata.image_identity     = 18;
  content_metadata.image_generation   = 4;
  tap.SetNextFramePreviewMetadata(content_metadata);
  tap.SubmitFinalDisplayFrame(MakeFrame());
  ASSERT_EQ(analyzer->submitted_frames.size(), 1U);

  FramePreviewMetadata view_metadata = content_metadata;
  view_metadata.preview_generation   = 11;
  view_metadata.scope_update_allowed = false;
  tap.SetNextFramePreviewMetadata(view_metadata);
  tap.SubmitFinalDisplayFrame(MakeFrame());

  EXPECT_EQ(tap.GetCurrentDisplayFrameView().display_generation, 11U);
  EXPECT_EQ(tap.GetCurrentScopeFrameView().display_generation, 10U);
  EXPECT_EQ(analyzer->submitted_frames.size(), 1U);

  ASSERT_TRUE(tap.SubmitCurrentDisplayFrameToScope());
  ASSERT_EQ(analyzer->submitted_frames.size(), 2U);
  EXPECT_EQ(analyzer->submitted_frames.back().display_generation, 10U);
}

TEST(EditorScopeControllerTest, SwitchingPlotRetainsAndResubmitsTheStagedFullFrame) {
  auto                     analyzer = std::make_shared<RecordingScopeAnalyzer>();
  FinalDisplayFrameTapSink tap(nullptr, analyzer);
  tap.SetFrameIdentity(18, 4);
  tap.SetScopeAnalysisDeferred(true);
  tap.SetScopeActive(true);

  ScopeRequest histogram_request;
  histogram_request.enabled_mask    = static_cast<uint32_t>(ScopeType::Histogram);
  histogram_request.waveform_width  = 320;
  histogram_request.waveform_height = 160;
  tap.SetScopeRequest(histogram_request);
  EXPECT_EQ(analyzer->resize_count, 1);

  FramePreviewMetadata metadata;
  metadata.preview_generation = 10;
  metadata.image_identity     = 18;
  metadata.image_generation   = 4;
  tap.SetNextFramePreviewMetadata(metadata);
  tap.SubmitFinalDisplayFrame(MakeFrame());
  ASSERT_TRUE(tap.GetCurrentScopeFrameView());

  ScopeRequest waveform_request = histogram_request;
  waveform_request.enabled_mask = static_cast<uint32_t>(ScopeType::Waveform);
  tap.SetScopeRequest(waveform_request);

  // enabled_mask changes dispatch only; destroying backend slots here would
  // invalidate the cached staged frame and leave the newly selected plot blank.
  EXPECT_EQ(analyzer->resize_count, 1);
  ASSERT_TRUE(tap.SubmitCurrentDisplayFrameToScope());
  ASSERT_EQ(analyzer->submitted_frames.size(), 1U);
  EXPECT_EQ(analyzer->submitted_frames.back().display_generation, 10U);
}

TEST(EditorScopeControllerTest, HiddenScopeStopsAnalysisAndResumesWithTheMostRecentDisplayFrame) {
  auto                     analyzer = std::make_shared<RecordingScopeAnalyzer>();
  FinalDisplayFrameTapSink tap(nullptr, analyzer);
  tap.SetFrameIdentity(18, 4);
  tap.SetScopeActive(true);

  FramePreviewMetadata first_metadata;
  first_metadata.preview_generation = 10;
  first_metadata.image_identity     = 18;
  first_metadata.image_generation   = 4;
  tap.SetNextFramePreviewMetadata(first_metadata);
  tap.SubmitFinalDisplayFrame(MakeFrame());
  ASSERT_EQ(analyzer->submitted_frames.size(), 1U);

  tap.SetScopeActive(false);
  FramePreviewMetadata hidden_metadata = first_metadata;
  hidden_metadata.preview_generation   = 11;
  tap.SetNextFramePreviewMetadata(hidden_metadata);
  tap.SubmitFinalDisplayFrame(MakeFrame());
  EXPECT_EQ(analyzer->submitted_frames.size(), 1U);
  EXPECT_EQ(analyzer->release_count, 0);

  tap.SetScopeActive(true);
  ASSERT_EQ(analyzer->submitted_frames.size(), 2U);
  EXPECT_EQ(analyzer->submitted_frames.back().display_generation, 11U);
}

TEST(EditorScopeControllerTest, ImageIdentityChangeDropsPendingFrameMetadata) {
  auto                     analyzer = std::make_shared<RecordingScopeAnalyzer>();
  FinalDisplayFrameTapSink tap(nullptr, analyzer);
  tap.SetFrameIdentity(18, 4);
  tap.SetScopeActive(true);

  FramePreviewMetadata stale_metadata;
  stale_metadata.preview_generation = 10;
  stale_metadata.image_identity     = 18;
  stale_metadata.image_generation   = 4;
  tap.SetNextFramePreviewMetadata(stale_metadata);

  tap.SetFrameIdentity(27, 5);
  tap.SubmitFinalDisplayFrame(MakeFrame());

  ASSERT_EQ(analyzer->submitted_frames.size(), 1U);
  const auto& submitted = analyzer->submitted_frames.front();
  EXPECT_EQ(submitted.image_identity, 27U);
  EXPECT_EQ(submitted.image_generation, 5U);
  EXPECT_EQ(submitted.display_generation, 0U);
}

TEST(EditorScopeControllerTest, VisualInactivityStopsPollingWithoutReleasingAnalyzerResources) {
  auto                  analyzer = std::make_shared<RecordingScopeAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(42, 3);

  auto* timer = controller.findChild<QTimer*>();
  ASSERT_NE(timer, nullptr);
  controller.set_visual_active(true);
  EXPECT_TRUE(timer->isActive());
  controller.set_visual_active(false);
  EXPECT_FALSE(timer->isActive());
  EXPECT_EQ(analyzer->release_count, 0);
}

TEST(EditorScopeControllerTest, UnifiedScopeSubmissionIsDeferredUntilPolling) {
  auto                  analyzer = std::make_shared<DeferredRecordingScopeAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(42, 3);
  controller.set_visual_active(true);

  auto* tap = dynamic_cast<FinalDisplayFrameTapSink*>(controller.frame_sink());
  ASSERT_NE(tap, nullptr);
  FramePreviewMetadata metadata;
  metadata.preview_generation = 30;
  metadata.image_identity     = 42;
  metadata.image_generation   = 3;
  tap->SetNextFramePreviewMetadata(metadata);
  tap->SubmitFinalDisplayFrame(MakeFrame());
  EXPECT_EQ(analyzer->submitted_count.load(std::memory_order_relaxed), 0);

  for (int attempt = 0;
       attempt < 40 && analyzer->submitted_count.load(std::memory_order_relaxed) == 0; ++attempt) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(5);
  }

  EXPECT_EQ(analyzer->submitted_count.load(std::memory_order_relaxed), 1);
  controller.set_visual_active(false);
}

TEST(EditorScopeControllerTest, PriorImageOutputIsRejectedAfterSessionIdentityChanges) {
  auto                  analyzer = std::make_shared<RecordingScopeAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(42, 3);
  controller.set_visual_active(true);

  auto* tap = dynamic_cast<FinalDisplayFrameTapSink*>(controller.frame_sink());
  ASSERT_NE(tap, nullptr);
  FramePreviewMetadata metadata;
  metadata.preview_generation = 30;
  metadata.image_identity     = 42;
  metadata.image_generation   = 3;
  tap->SetNextFramePreviewMetadata(metadata);
  tap->SubmitFinalDisplayFrame(MakeFrame());

  analyzer->latest_output.generation         = 9;
  analyzer->latest_output.image_identity     = 42;
  analyzer->latest_output.image_generation   = 3;
  analyzer->latest_output.display_generation = 30;
  controller.SetImageIdentity(99, 4);
  EXPECT_FALSE(controller.refreshSnapshot());
  EXPECT_FALSE(controller.has_snapshot());
  EXPECT_EQ(controller.image_identity(), 99U);
  EXPECT_EQ(controller.image_generation(), 4U);
}

TEST(EditorScopeControllerTest, EmptyScopeOutputIsRejectedWhileCurrentImageRemainsOpen) {
  auto                  analyzer = std::make_shared<RecordingScopeAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(42, 3);
  controller.set_visual_active(true);

  auto* tap = dynamic_cast<FinalDisplayFrameTapSink*>(controller.frame_sink());
  ASSERT_NE(tap, nullptr);
  FramePreviewMetadata metadata;
  metadata.preview_generation = 30;
  metadata.image_identity     = 42;
  metadata.image_generation   = 3;
  tap->SetNextFramePreviewMetadata(metadata);
  tap->SubmitFinalDisplayFrame(MakeFrame());

  analyzer->latest_output.generation         = 9;
  analyzer->latest_output.image_identity     = 42;
  analyzer->latest_output.image_generation   = 3;
  analyzer->latest_output.display_generation = 29;
  EXPECT_FALSE(controller.refreshSnapshot());
  EXPECT_FALSE(controller.has_snapshot());
  EXPECT_EQ(analyzer->release_count, 0);
}

/// Test seam granting the suite access to the private publishSnapshot filter,
/// the relaxed "latest completed for the current image" matching rule that
/// keeps the scope publishing during continuous dragging.
class EditorScopeControllerTestPeer {
 public:
  explicit EditorScopeControllerTestPeer(EditorScopeController& controller)
      : controller_(controller) {}

  auto PublishSnapshot(const alcedo::ScopeRenderSnapshot& snapshot, std::uint64_t image_identity,
                       std::uint64_t image_generation, std::uint64_t request_revision) -> bool {
    return controller_.publishSnapshot(snapshot, image_identity, image_generation,
                                       request_revision);
  }

 private:
  EditorScopeController& controller_;
};

TEST(EditorScopeControllerTest, SwitchingToUncomputedWaveformPublishesWaveformContent) {
  auto                  analyzer = std::make_shared<RecordingScopeAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(42, 3);
  controller.set_visual_active(true);
  EditorScopeControllerTestPeer peer(controller);

  auto* tap = dynamic_cast<FinalDisplayFrameTapSink*>(controller.frame_sink());
  ASSERT_NE(tap, nullptr);
  FramePreviewMetadata metadata;
  metadata.preview_generation = 30;
  metadata.image_identity     = 42;
  metadata.image_generation   = 3;
  tap->SetNextFramePreviewMetadata(metadata);
  tap->SubmitFinalDisplayFrame(MakeFrame());

  QObject::connect(&controller, &EditorScopeController::FrameRequested, &controller, [&]() {
    FramePreviewMetadata refreshed_metadata = metadata;
    refreshed_metadata.preview_generation   = 31;
    tap->SetNextFramePreviewMetadata(refreshed_metadata);
    tap->SubmitFinalDisplayFrame(MakeFrame());
  });
  analyzer->on_submit = [&peer](const FinalDisplayFrameView& frame, const ScopeRequest& request) {
    if ((request.enabled_mask & static_cast<uint32_t>(ScopeType::Waveform)) == 0U) {
      return;
    }
    auto waveform            = MakeValidSnapshot(10, frame.image_identity, frame.image_generation,
                                                 frame.display_generation);
    waveform.histogram       = {};
    waveform.waveform.width  = 2;
    waveform.waveform.height = 2;
    waveform.waveform.rgba   = {0.1f, 0.2f, 0.3f, 0.4f};
    waveform.waveform.valid  = true;
    (void)peer.PublishSnapshot(waveform, frame.image_identity, frame.image_generation, 1);
  };

  controller.set_active_view(1);
  (void)controller.refreshSnapshot();

  ASSERT_TRUE(controller.has_snapshot());
  const auto snapshot = controller.snapshot();
  EXPECT_TRUE(snapshot.waveform.valid);
  EXPECT_EQ(snapshot.waveform.width, 2);
  EXPECT_EQ(snapshot.waveform.height, 2);
  EXPECT_FALSE(snapshot.waveform.rgba.empty());
  controller.set_visual_active(false);
}

TEST(EditorScopeControllerTest, InactivePlotOutputDoesNotMarkSelectedPlotReady) {
  auto                  analyzer = std::make_shared<RecordingScopeAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(42, 3);
  controller.set_visual_active(true);
  EditorScopeControllerTestPeer peer(controller);

  controller.set_active_view(1);
  const auto histogram_only = MakeValidSnapshot(9, 42, 3, 30);
  EXPECT_TRUE(peer.PublishSnapshot(histogram_only, 42, 3, 1));
  EXPECT_FALSE(controller.has_snapshot());

  auto waveform            = histogram_only;
  waveform.generation      = 10;
  waveform.waveform.width  = 2;
  waveform.waveform.height = 2;
  waveform.waveform.rgba   = {0.1f, 0.2f, 0.3f, 0.4f};
  waveform.waveform.valid  = true;
  EXPECT_TRUE(peer.PublishSnapshot(waveform, 42, 3, 1));
  EXPECT_TRUE(controller.has_snapshot());
  controller.set_visual_active(false);
}

TEST(EditorScopeControllerTest, DeferredScopeAnalyzesStagedFrameNotPipelineSource) {
  auto                     analyzer = std::make_shared<StagingRecordingAnalyzer>();
  FinalDisplayFrameTapSink tap(nullptr, analyzer);
  tap.SetFrameIdentity(18, 4);
  tap.SetScopeActive(true);
  tap.SetScopeAnalysisDeferred(true);

  FramePreviewMetadata metadata;
  metadata.preview_generation = 10;
  metadata.image_identity     = 18;
  metadata.image_generation   = 4;
  tap.SetNextFramePreviewMetadata(metadata);
  tap.SubmitFinalDisplayFrame(MakeFrame());  // MakeFrame() carries marker int(1)

  // StageFrame ran synchronously on the render thread with the pipeline source.
  ASSERT_EQ(analyzer->staged_inputs().size(), 1U);
  EXPECT_EQ(MarkerValue(analyzer->staged_inputs().front().image.resource), 1);
  // The tap retains the staged, analyzer-owned frame (marker 2), never the
  // pipeline's reused source buffer (marker 1).
  const auto scope_frame = tap.GetCurrentScopeFrameView();
  ASSERT_TRUE(scope_frame);
  EXPECT_EQ(MarkerValue(scope_frame.image.resource), 2);
  // The deferred submit analyzes the staged frame, not the pipeline source, so
  // a later pipeline buffer reuse can never reach the scope analyzer.
  EXPECT_TRUE(tap.SubmitCurrentDisplayFrameToScope());
  ASSERT_EQ(analyzer->submitted_frames().size(), 1U);
  EXPECT_EQ(MarkerValue(analyzer->submitted_frames().front().image.resource), 2);
}

TEST(EditorScopeControllerTest, LatestCompletedOutputIsPublishedWhenRenderGenerationAdvanced) {
  auto                  analyzer = std::make_shared<RecordingScopeAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(42, 3);
  controller.set_visual_active(true);
  EditorScopeControllerTestPeer peer(controller);

  // A completed output whose display_generation (29) lags the newest render
  // (30) is published for the current image instead of being dropped, so the
  // scope keeps updating during continuous dragging.
  EXPECT_TRUE(peer.PublishSnapshot(MakeValidSnapshot(9, 42, 3, 29), 42, 3, 0));
  EXPECT_TRUE(controller.has_snapshot());
  EXPECT_EQ(controller.scope_generation(), 9U);

  // A newer completed output replaces the published snapshot.
  EXPECT_TRUE(peer.PublishSnapshot(MakeValidSnapshot(10, 42, 3, 30), 42, 3, 0));
  EXPECT_EQ(controller.scope_generation(), 10U);

  // The same generation/content is not re-published.
  EXPECT_FALSE(peer.PublishSnapshot(MakeValidSnapshot(10, 42, 3, 30), 42, 3, 0));
  EXPECT_EQ(controller.scope_generation(), 10U);

  // An output from a different image is rejected.
  EXPECT_FALSE(peer.PublishSnapshot(MakeValidSnapshot(11, 99, 4, 31), 42, 3, 0));
  EXPECT_EQ(controller.scope_generation(), 10U);
}

TEST(EditorScopeControllerTest, RefreshCollectsLatestCompletedBeforeSubmittingNext) {
  auto                  analyzer = std::make_shared<SequencedAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(18, 4);
  controller.set_visual_active(true);

  auto* tap = dynamic_cast<FinalDisplayFrameTapSink*>(controller.frame_sink());
  ASSERT_NE(tap, nullptr);
  auto submit_frame = [&](std::uint64_t display_gen) {
    FramePreviewMetadata metadata;
    metadata.preview_generation = display_gen;
    metadata.image_identity     = 18;
    metadata.image_generation   = 4;
    tap->SetNextFramePreviewMetadata(metadata);
    tap->SubmitFinalDisplayFrame(MakeFrame());
  };

  auto wait_for = [&](auto predicate) {
    for (int i = 0; i < 100 && !predicate(); ++i) {
      QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
      QThread::msleep(5);
    }
  };

  submit_frame(10);
  wait_for([&] { return analyzer->call_log().size() >= 2U; });
  // The controller collects a completed result before submitting the next
  // frame, so the first analyzer call is "collect" (not "submit").
  ASSERT_GE(analyzer->call_log().size(), 2U);
  EXPECT_EQ(analyzer->call_log().front(), "collect");

  // A second frame and poll: the result submitted on the first tick is
  // collected on the second tick (1-tick pipeline, no dropped result).
  submit_frame(11);
  wait_for([&] { return analyzer->call_log().size() >= 4U; });
  ASSERT_GE(analyzer->submitted_gens().size(), 1U);
  ASSERT_GE(analyzer->collected_gens().size(), 2U);
  EXPECT_EQ(analyzer->collected_gens().at(1), analyzer->submitted_gens().front());
  controller.set_visual_active(false);
}

TEST(EditorScopeControllerTest, ScopeWorkerDrainsBeforeSessionRelease) {
  auto                  analyzer = std::make_shared<BlockingSubmitAnalyzer>();
  EditorScopeController controller(analyzer);
  controller.SetImageIdentity(42, 3);
  controller.set_visual_active(true);

  auto* tap = dynamic_cast<FinalDisplayFrameTapSink*>(controller.frame_sink());
  ASSERT_NE(tap, nullptr);
  FramePreviewMetadata metadata;
  metadata.preview_generation = 30;
  metadata.image_identity     = 42;
  metadata.image_generation   = 3;
  tap->SetNextFramePreviewMetadata(metadata);
  tap->SubmitFinalDisplayFrame(MakeFrame());

  // Start the poll timer; the worker posts to the dedicated pool and blocks
  // inside SubmitFrame, simulating a scope task mid-analysis at session exit.
  for (int i = 0; i < 100 && !analyzer->submit_started(); ++i) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    QThread::msleep(5);
  }
  ASSERT_TRUE(analyzer->submit_started());

  // Release the worker shortly; Shutdown must block until it finishes rather
  // than leaving it running against a pipeline being torn down.
  std::thread releaser([&] {
    QThread::msleep(40);
    analyzer->Release();
  });
  const auto  t0 = std::chrono::steady_clock::now();
  controller.Shutdown();
  const auto t1 = std::chrono::steady_clock::now();
  releaser.join();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  EXPECT_TRUE(analyzer->submitted_done());
  EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(), 35);
}

}  // namespace alcedo::ui::test
