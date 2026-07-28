//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_scope_controller.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QThread>
#include <atomic>
#include <memory>
#include <optional>
#include <vector>

#include "edit/scope/final_display_frame_tap.hpp"

namespace alcedo::ui::test {
namespace {

class RecordingScopeAnalyzer final : public IScopeAnalyzer {
 public:
  void SubmitFrame(const FinalDisplayFrameView& frame, const ScopeRequest&) override {
    submitted_frames.push_back(frame);
  }

  auto GetLatestOutput() -> ScopeOutputSet override { return latest_output; }

  void ResizeResources(const ScopeRequest&) override { ++resize_count; }

  void ReleaseResources() override { ++release_count; }

  std::vector<FinalDisplayFrameView> submitted_frames;
  ScopeOutputSet                     latest_output{};
  int                                resize_count  = 0;
  int                                release_count = 0;
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

TEST(EditorScopeControllerTest, OlderDisplayOutputIsRejectedWhileCurrentImageRemainsOpen) {
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

}  // namespace alcedo::ui::test
