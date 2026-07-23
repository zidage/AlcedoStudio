//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_render_controller.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_lifecycle.hpp"

namespace alcedo {
namespace {

class EditorSessionRenderControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    pipeline_    = std::make_shared<EditorSessionBootstrapPipelinePort>();
    history_     = std::make_shared<EditorSessionBootstrapHistoryPort>();
    scheduler_   = std::make_shared<EditorSessionBootstrapSchedulerPort>();
    coordinator_ = std::make_shared<EditorRenderCoordinator>(scheduler_);

    EditorSessionLifecycle::Dependencies life_deps;
    life_deps.pipeline = pipeline_;
    life_deps.history  = history_;
    lifecycle_         = std::make_unique<EditorSessionLifecycle>(std::move(life_deps));

    EditorSessionRenderController::Dependencies render_deps{
        coordinator_,
        *lifecycle_,
        [this](const EditorRenderEvent& event) { events_.push_back(event); },
    };
    render_ = std::make_unique<EditorSessionRenderController>(std::move(render_deps));

    // Wire coordinator results into the render controller (same as the
    // production EditorSessionRuntime observer wiring).
    EditorSessionRenderController* render_ptr = render_.get();
    coordinator_->SetResultObserver([render_ptr](const EditorRenderResult& result) {
      if (render_ptr) {
        render_ptr->NotifyRenderResult(result);
      }
    });

    render_->SetPresentationSinkId(1);
    render_->SetPresentationSize(640, 480);
  }

  void OpenImage() {
    lifecycle_->AdvanceSessionGeneration(1, 2);
    lifecycle_->TransitionTo(EditorSessionState::Loading, EditorSessionResultKind::StateChanged,
                             "Loading");
    render_->MarkImageAcquired();
    render_->RouteInitialRender(EditorRenderReason::InitialFrame);
  }

  void PresentFirstFrame() {
    auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
    ASSERT_NE(sched, nullptr);
    ASSERT_FALSE(sched->scheduled().empty());
    const auto request_id = sched->scheduled().front().request_id;
    coordinator_->NotifySchedulerCompleted(request_id, true);
    coordinator_->NotifyFrameSubmitted(request_id);
    coordinator_->NotifyFramePresented(request_id);
    // Presenting the first frame auto-routes a QualityBase follow-up. Drive it
    // to completion so the coordinator is idle for view-change tests.
    if (sched->scheduled().size() > 1u) {
      const auto quality_request_id = sched->scheduled().back().request_id;
      coordinator_->NotifySchedulerCompleted(quality_request_id, true);
      coordinator_->NotifyFrameSubmitted(quality_request_id);
      coordinator_->NotifyFramePresented(quality_request_id);
    }
  }

  std::shared_ptr<EditorSessionBootstrapPipelinePort>  pipeline_;
  std::shared_ptr<EditorSessionBootstrapHistoryPort>   history_;
  std::shared_ptr<EditorSessionBootstrapSchedulerPort> scheduler_;
  std::shared_ptr<EditorRenderCoordinator>             coordinator_;
  std::unique_ptr<EditorSessionLifecycle>              lifecycle_;
  std::unique_ptr<EditorSessionRenderController>       render_;
  std::vector<EditorRenderEvent>                       events_;
};

TEST_F(EditorSessionRenderControllerTest, RouteInitialRenderSchedulesInteractivePrimary) {
  OpenImage();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_EQ(sched->scheduled().size(), 1u);
  EXPECT_EQ(sched->scheduled().front().intent.reason, EditorRenderReason::InitialFrame);
  EXPECT_EQ(sched->scheduled().front().intent.frame_role, FrameRole::InteractivePrimary);
  EXPECT_NE(render_->first_frame_request_id(), 0u);
}

TEST_F(EditorSessionRenderControllerTest, FirstFramePresentationEmitsInteractiveEvent) {
  OpenImage();
  EXPECT_LT(render_->first_frame_time_ms(), 0.0);
  PresentFirstFrame();
  EXPECT_GE(render_->first_frame_time_ms(), 0.0);
  bool saw_first_frame = false;
  for (const auto& event : events_) {
    if (event.kind == EditorRenderEventKind::FirstFramePresented) {
      saw_first_frame = true;
      EXPECT_EQ(event.state, EditorSessionState::Interactive);
    }
  }
  EXPECT_TRUE(saw_first_frame);
}

TEST_F(EditorSessionRenderControllerTest, QualityBaseFollowsInteractivePrimaryFirstFrame) {
  OpenImage();
  PresentFirstFrame();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_GE(sched->scheduled().size(), 2u);
  EXPECT_EQ(sched->scheduled().back().intent.frame_role, FrameRole::QualityBase);
  EXPECT_EQ(sched->scheduled().back().intent.quality, EditorRenderQuality::Quality);
}

TEST_F(EditorSessionRenderControllerTest, ViewChangeZoomPanIsReusedWithoutScheduling) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
                           "Interactive");
  auto*      sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  const auto scheduled_before = sched->scheduled().size();

  const auto event            = render_->RouteViewChange(EditorRenderReason::ZoomPan, std::nullopt);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderReused);
  EXPECT_EQ(sched->scheduled().size(), scheduled_before);
  EXPECT_FALSE(render_->render_busy());
}

TEST_F(EditorSessionRenderControllerTest, ViewChangeDetailRefreshSchedulesDetailPatch) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
                           "Interactive");
  auto*                sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());

  ViewportRenderRegion region{};
  region.x_                = 7;
  region.y_                = 0;
  region.scale_x_          = 2.0f;
  region.scale_y_          = 2.0f;
  region.reference_width_  = 640;
  region.reference_height_ = 480;
  region.target_width_     = 320;
  region.target_height_    = 240;
  const auto event         = render_->RouteViewChange(EditorRenderReason::DetailRefresh, region);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderRouted);
  ASSERT_FALSE(sched->scheduled().empty());
  const auto& scheduled = sched->scheduled().back().intent;
  EXPECT_EQ(scheduled.frame_role, FrameRole::DetailPatch);
  EXPECT_EQ(scheduled.reason, EditorRenderReason::DetailRefresh);
  ASSERT_TRUE(scheduled.view_region.has_value());
  EXPECT_EQ(scheduled.view_region->x_, 7);
}

TEST_F(EditorSessionRenderControllerTest, ViewChangeCropRotateAdvancesRenderGeneration) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
                           "Interactive");
  const auto render_gen_before = lifecycle_->identity().render_generation;

  const auto event = render_->RouteViewChange(EditorRenderReason::CropRotate, std::nullopt);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderRouted);
  EXPECT_GT(lifecycle_->identity().render_generation, render_gen_before);
}

TEST_F(EditorSessionRenderControllerTest, ViewChangeRejectedWhenNotInteractive) {
  const auto event = render_->RouteViewChange(EditorRenderReason::ZoomPan, std::nullopt);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderRejected);
}

TEST_F(EditorSessionRenderControllerTest, RenderBusyTransitionsAroundViewChange) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
                           "Interactive");
  EXPECT_FALSE(render_->render_busy());

  ViewportRenderRegion region{};
  region.x_        = 3;
  const auto event = render_->RouteViewChange(EditorRenderReason::DetailRefresh, region);
  ASSERT_EQ(event.kind, EditorRenderEventKind::RenderRouted);
  EXPECT_TRUE(render_->render_busy());

  coordinator_->NotifySchedulerCompleted(event.request_id, true);
  EXPECT_FALSE(render_->render_busy());
}

}  // namespace
}  // namespace alcedo