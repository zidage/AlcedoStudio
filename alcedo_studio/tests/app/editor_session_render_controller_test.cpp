//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_render_controller.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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
        [this](const EditorRenderEvent& event) { events_.push_back(event); },
    };
    render_ = std::make_unique<EditorSessionRenderController>(std::move(render_deps));

    // Wire coordinator results into the render controller.
    EditorSessionRenderController* render_ptr = render_.get();
    coordinator_->SetResultObserver([render_ptr, this](const EditorRenderResult& result) {
      if (render_ptr) {
        render_ptr->NotifyRenderResult(result, lifecycle_->identity(), lifecycle_->state());
      }
    });

    render_->SetPresentationSinkId(1);
    render_->SetPresentationSize(640, 480);
  }

  auto MakeCommand(EditorRenderReason reason) const -> EditorRenderCommand {
    EditorRenderCommand command;
    command.reason = reason;
    return command;
  }

  void OpenImage() {
    // Use the semantic lifecycle API for image setup.
    ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
    ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
    lifecycle_->MarkImageReady();
    render_->MarkImageAcquired();

    EditorRenderCommand command;
    command.reason = EditorRenderReason::InitialFrame;
    render_->RouteInitialRender(command, lifecycle_->identity());
  }

  void PresentFirstFrame() {
    auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
    ASSERT_NE(sched, nullptr);
    ASSERT_FALSE(sched->scheduled().empty());
    const auto request_id = sched->scheduled().front().request_id;
    coordinator_->NotifySchedulerCompleted(request_id, true);
    coordinator_->NotifyFrameSubmitted(request_id);
    coordinator_->NotifyFramePresented(request_id);
    // The render controller auto-routes QualityBase after first frame.
    // Drive it to completion for subsequent tests.
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

TEST_F(EditorSessionRenderControllerTest, GeometryOverlayFlagIsStampedOnRenderIntent) {
  render_->SetGeometryOverlayActive(true);
  OpenImage();

  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_FALSE(sched->scheduled().empty());
  EXPECT_TRUE(sched->scheduled().front().intent.geometry_overlay_only);
}

TEST_F(EditorSessionRenderControllerTest, FirstFramePresentationEmitsEvent) {
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

TEST_F(EditorSessionRenderControllerTest, ViewChangeZoomPanIsReusedInInteractive) {
  OpenImage();
  PresentFirstFrame();
  // Transition lifecycle to Interactive via MarkFirstFramePresented.
  lifecycle_->MarkFirstFramePresented();
  auto*               sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  const auto          scheduled_before = sched->scheduled().size();

  EditorRenderCommand command;
  command.reason = EditorRenderReason::ZoomPan;
  const auto event =
      render_->RouteViewChange(command, lifecycle_->identity(), EditorSessionState::Interactive);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderReused);
  EXPECT_EQ(sched->scheduled().size(), scheduled_before);
}

TEST_F(EditorSessionRenderControllerTest, ViewChangeDetailRefreshSchedulesDetailPatch) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->MarkFirstFramePresented();
  auto*               sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());

  EditorRenderCommand command;
  command.reason = EditorRenderReason::DetailRefresh;
  ViewportRenderRegion region{};
  region.x_                = 7;
  region.y_                = 0;
  region.scale_x_          = 2.0f;
  region.scale_y_          = 2.0f;
  region.reference_width_  = 640;
  region.reference_height_ = 480;
  region.target_width_     = 320;
  region.target_height_    = 240;
  command.view_region      = region;

  const auto event =
      render_->RouteViewChange(command, lifecycle_->identity(), EditorSessionState::Interactive);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderRouted);
  ASSERT_FALSE(sched->scheduled().empty());
  const auto& scheduled = sched->scheduled().back().intent;
  EXPECT_EQ(scheduled.frame_role, FrameRole::DetailPatch);
  EXPECT_EQ(scheduled.reason, EditorRenderReason::DetailRefresh);
  ASSERT_TRUE(scheduled.view_region.has_value());
  EXPECT_EQ(scheduled.view_region->x_, 7);
}

TEST_F(EditorSessionRenderControllerTest, ViewChangeRejectedWhenNotInteractive) {
  EditorRenderCommand command;
  command.reason = EditorRenderReason::ZoomPan;
  const auto event =
      render_->RouteViewChange(command, lifecycle_->identity(), EditorSessionState::NoImage);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderRejected);
}

TEST_F(EditorSessionRenderControllerTest, RenderBusyTransitionsAroundViewChange) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->MarkFirstFramePresented();
  EXPECT_FALSE(render_->render_busy());

  EditorRenderCommand command;
  command.reason = EditorRenderReason::DetailRefresh;
  ViewportRenderRegion region{};
  region.x_           = 3;
  command.view_region = region;
  const auto event =
      render_->RouteViewChange(command, lifecycle_->identity(), EditorSessionState::Interactive);
  ASSERT_EQ(event.kind, EditorRenderEventKind::RenderRouted);
  EXPECT_TRUE(render_->render_busy());

  coordinator_->NotifySchedulerCompleted(event.request_id, true);
  EXPECT_FALSE(render_->render_busy());
}

// ── Adjustment snapshots and stale results ──────────────────────────────────

TEST_F(EditorSessionRenderControllerTest, InitialRenderWithAdjustmentCarriesSnapshot) {
  // Set up lifecycle for an image.
  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  lifecycle_->MarkImageReady();
  render_->MarkImageAcquired();

  EditorRenderCommand command;
  command.reason                         = EditorRenderReason::InitialFrame;
  command.adjustment.params_json         = R"({"exposure":1.5})";
  command.adjustment.snapshot_generation = 42;
  EditorAdjustmentPatch patch;
  patch.field_key   = "exposure";
  patch.params_json = R"({"exposure":1.5})";
  command.adjustment.patches.push_back(patch);

  const auto request_id = render_->RouteInitialRender(command, lifecycle_->identity());
  EXPECT_NE(request_id, 0u);

  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_FALSE(sched->scheduled().empty());
  const auto& intent = sched->scheduled().front().intent;
  EXPECT_EQ(intent.adjustment.params_json, R"({"exposure":1.5})");
  EXPECT_EQ(intent.adjustment.snapshot_generation, 42u);
  ASSERT_EQ(intent.adjustment.patches.size(), 1u);
  EXPECT_EQ(intent.adjustment.patches.front().field_key, "exposure");
}

TEST_F(EditorSessionRenderControllerTest, QualityBaseCarriesSameAdjustmentAfterFirstFrame) {
  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  lifecycle_->MarkImageReady();
  render_->MarkImageAcquired();

  EditorRenderCommand command;
  command.reason                 = EditorRenderReason::InitialFrame;
  command.adjustment.params_json = R"({"saturation":0.8})";
  render_->RouteInitialRender(command, lifecycle_->identity());

  // Drive first-frame to completion then presentation.
  auto*      sched         = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  const auto ff_request_id = sched->scheduled().front().request_id;
  coordinator_->NotifySchedulerCompleted(ff_request_id, true);
  coordinator_->NotifyFrameSubmitted(ff_request_id);
  coordinator_->NotifyFramePresented(ff_request_id);

  // The QualityBase follow-up should carry the same adjustment.
  ASSERT_GE(sched->scheduled().size(), 2u);
  const auto& qb_intent = sched->scheduled().back().intent;
  EXPECT_EQ(qb_intent.frame_role, FrameRole::QualityBase);
  EXPECT_EQ(qb_intent.adjustment.params_json, R"({"saturation":0.8})");
}

TEST_F(EditorSessionRenderControllerTest, StaleSessionGenerationResultDoesNotAdvanceFirstFrame) {
  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  lifecycle_->MarkImageReady();
  render_->MarkImageAcquired();

  EditorRenderCommand command;
  command.reason           = EditorRenderReason::InitialFrame;
  const auto         ff_id = render_->RouteInitialRender(command, lifecycle_->identity());

  // Construct a render result referencing the stale identity.
  EditorRenderResult stale_result;
  stale_result.request_id                = ff_id;
  stale_result.kind                      = EditorRenderResultKind::RenderCompleted;
  stale_result.intent.session_generation = 99;
  stale_result.intent.image_id           = lifecycle_->identity().image_id;
  stale_result.intent.element_id         = lifecycle_->identity().element_id;

  const auto accepted_before             = coordinator_->diagnostics().accepted_count;
  render_->NotifyRenderResult(stale_result, lifecycle_->identity(), EditorSessionState::Loading);
  stale_result.kind = EditorRenderResultKind::FrameSubmitted;
  render_->NotifyRenderResult(stale_result, lifecycle_->identity(), EditorSessionState::Loading);
  stale_result.kind = EditorRenderResultKind::FramePresented;
  render_->NotifyRenderResult(stale_result, lifecycle_->identity(), EditorSessionState::Loading);

  EXPECT_TRUE(std::none_of(events_.begin(), events_.end(), [](const EditorRenderEvent& event) {
    return event.kind == EditorRenderEventKind::FirstFramePresented;
  }));
  EXPECT_EQ(coordinator_->diagnostics().accepted_count, accepted_before);
}

TEST_F(EditorSessionRenderControllerTest, SecondRouteInitialRenderReturnsRequestId) {
  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  lifecycle_->MarkImageReady();
  render_->MarkImageAcquired();

  // Route a first frame.
  EditorRenderCommand cmd1;
  cmd1.reason                 = EditorRenderReason::InitialFrame;
  cmd1.adjustment.params_json = R"({"v1":1})";
  const auto id1              = render_->RouteInitialRender(cmd1, lifecycle_->identity());
  EXPECT_NE(id1, 0u);

  // Route a second render (e.g. undo/redo).
  EditorRenderCommand cmd2;
  cmd2.reason                 = EditorRenderReason::UndoRedo;
  cmd2.adjustment.params_json = R"({"v2":2})";
  const auto id2              = render_->RouteInitialRender(cmd2, lifecycle_->identity());
  // Undo/redo should still submit a render, just not as a first-frame.
  EXPECT_NE(id2, 0u);
}

}  // namespace
}  // namespace alcedo
