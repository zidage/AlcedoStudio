//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_render_controller.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_session_bootstrap.hpp"
#include "app/editor_session_lifecycle.hpp"

namespace alcedo {
namespace {

class EditorSessionRenderControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    history_     = std::make_shared<EditorSessionBootstrapHistoryPort>();
    scheduler_   = std::make_shared<EditorSessionBootstrapSchedulerPort>();
    coordinator_ = std::make_shared<EditorRenderCoordinator>(scheduler_);

    EditorSessionLifecycle::Dependencies life_deps;
    life_deps.history = history_;
    lifecycle_        = std::make_unique<EditorSessionLifecycle>(std::move(life_deps));

    EditorSessionRenderController::Dependencies render_deps{
        coordinator_,
        [this](const EditorRenderEvent& event) { events_.push_back(event); },
    };
    render_ = std::make_unique<EditorSessionRenderController>(std::move(render_deps));

    // Wire coordinator results into the render controller.
    EditorSessionRenderController* render_ptr = render_.get();
    coordinator_->SetResultObserver([render_ptr, this](const EditorRenderResult& result) {
      if (render_ptr) {
        render_ptr->NotifyRenderResult(result, lifecycle_->identity(),
                                       lifecycle_->active_image_load_request(), lifecycle_->state());
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
    render_->RouteInitialRender(command, lifecycle_->identity(),
                                lifecycle_->active_image_load_request());
  }

  void PresentFirstFrame() {
    auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
    ASSERT_NE(sched, nullptr);
    ASSERT_FALSE(sched->scheduled().empty());
    const auto request_id = sched->scheduled().front().request_id;
    coordinator_->NotifySchedulerCompleted(request_id, true);
    // The render controller auto-routes QualityBase after first frame.
    // Drive it to completion for subsequent tests.
    if (sched->scheduled().size() > 1u) {
      const auto quality_request_id = sched->scheduled().back().request_id;
      coordinator_->NotifySchedulerCompleted(quality_request_id, true);
    }
  }

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

TEST_F(EditorSessionRenderControllerTest, RouteInitialRenderBindsSessionContextAtOpen) {
  OpenImage();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);
  ASSERT_EQ(sched->binds().size(), 1u);
  EXPECT_EQ(sched->binds().front().element_id, lifecycle_->identity().element_id);
  EXPECT_EQ(sched->binds().front().image_id, lifecycle_->identity().image_id);
  EXPECT_EQ(sched->binds().front().epoch, lifecycle_->active_image_load_request().value);
  EXPECT_EQ(sched->binds().front().presentation_sink_id, 1u);
}

TEST_F(EditorSessionRenderControllerTest, ResetForNewImageClearsSessionRenderContext) {
  OpenImage();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);
  const int clears_after_open = sched->clear_count();

  render_->ResetForNewImage();
  EXPECT_EQ(sched->clear_count(), clears_after_open + 1);
}

TEST_F(EditorSessionRenderControllerTest, GeometryOverlayFlagIsStampedOnRenderIntent) {
  render_->SetGeometryOverlayActive(true);
  OpenImage();

  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_FALSE(sched->scheduled().empty());
  EXPECT_TRUE(sched->scheduled().front().intent.geometry_overlay_only);
}

TEST_F(EditorSessionRenderControllerTest, FirstReadyFrameEntersInteractiveState) {
  OpenImage();
  EXPECT_LT(render_->first_frame_time_ms(), 0.0);
  PresentFirstFrame();
  EXPECT_GE(render_->first_frame_time_ms(), 0.0);
  bool saw_first_frame = false;
  for (const auto& event : events_) {
    if (event.kind == EditorRenderEventKind::FirstFrameReady) {
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
  lifecycle_->MarkFirstFrameReady();
  auto*               sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  const auto          scheduled_before = sched->scheduled().size();

  EditorRenderCommand command;
  command.reason = EditorRenderReason::ZoomPan;
  const auto event =
      render_->RouteViewChange(command, lifecycle_->identity(),
                               lifecycle_->active_image_load_request(),
                               EditorSessionState::Interactive);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderReused);
  EXPECT_EQ(sched->scheduled().size(), scheduled_before);
}

TEST_F(EditorSessionRenderControllerTest, ViewChangeDetailRefreshSchedulesDetailPatch) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->MarkFirstFrameReady();
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
      render_->RouteViewChange(command, lifecycle_->identity(),
                               lifecycle_->active_image_load_request(),
                               EditorSessionState::Interactive);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderRouted);
  ASSERT_FALSE(sched->scheduled().empty());
  const auto& scheduled = sched->scheduled().back().intent;
  EXPECT_EQ(scheduled.frame_role, FrameRole::DetailPatch);
  EXPECT_EQ(scheduled.reason, EditorRenderReason::DetailRefresh);
  ASSERT_TRUE(scheduled.view_region.has_value());
  EXPECT_EQ(scheduled.view_region->x_, 7);
}

TEST_F(EditorSessionRenderControllerTest, DetailRefreshWaitsForQualityBaseAndKeepsNewestRegion) {
  OpenImage();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);
  ASSERT_EQ(sched->scheduled().size(), 1u);

  const auto first_request_id = sched->scheduled().front().request_id;
  coordinator_->NotifySchedulerCompleted(first_request_id, true);
  lifecycle_->MarkFirstFrameReady();
  ASSERT_EQ(sched->scheduled().size(), 2u);
  ASSERT_EQ(sched->scheduled().back().intent.frame_role, FrameRole::QualityBase);
  const auto quality_request_id = sched->scheduled().back().request_id;

  EditorRenderCommand first_detail;
  first_detail.reason = EditorRenderReason::DetailRefresh;
  ViewportRenderRegion first_region{};
  first_region.x_          = 10;
  first_detail.view_region = first_region;
  const auto first_event =
      render_->RouteViewChange(first_detail, lifecycle_->identity(),
                               lifecycle_->active_image_load_request(),
                               EditorSessionState::Interactive);
  EXPECT_EQ(first_event.kind, EditorRenderEventKind::RenderReused);
  EXPECT_EQ(sched->scheduled().size(), 2u);

  EditorRenderCommand final_detail;
  final_detail.reason = EditorRenderReason::DetailRefresh;
  ViewportRenderRegion final_region{};
  final_region.x_          = 20;
  final_detail.view_region = final_region;
  const auto final_event =
      render_->RouteViewChange(final_detail, lifecycle_->identity(),
                               lifecycle_->active_image_load_request(),
                               EditorSessionState::Interactive);
  EXPECT_EQ(final_event.kind, EditorRenderEventKind::RenderReused);
  EXPECT_EQ(sched->scheduled().size(), 2u);

  coordinator_->NotifySchedulerCompleted(quality_request_id, true);
  ASSERT_EQ(sched->scheduled().size(), 3u);
  const auto& detail_intent = sched->scheduled().back().intent;
  EXPECT_EQ(detail_intent.frame_role, FrameRole::DetailPatch);
  ASSERT_TRUE(detail_intent.view_region.has_value());
  EXPECT_EQ(detail_intent.view_region->x_, 20);
}

TEST_F(EditorSessionRenderControllerTest, ViewChangeRejectedWhenNotInteractive) {
  EditorRenderCommand command;
  command.reason = EditorRenderReason::ZoomPan;
  const auto event =
      render_->RouteViewChange(command, lifecycle_->identity(),
                               lifecycle_->active_image_load_request(),
                               EditorSessionState::NoImage);
  EXPECT_EQ(event.kind, EditorRenderEventKind::RenderRejected);
}

TEST_F(EditorSessionRenderControllerTest, RenderBusyTransitionsAroundViewChange) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->MarkFirstFrameReady();
  EXPECT_FALSE(render_->render_busy());

  EditorRenderCommand command;
  command.reason = EditorRenderReason::DetailRefresh;
  ViewportRenderRegion region{};
  region.x_           = 3;
  command.view_region = region;
  const auto event =
      render_->RouteViewChange(command, lifecycle_->identity(),
                               lifecycle_->active_image_load_request(),
                               EditorSessionState::Interactive);
  ASSERT_EQ(event.kind, EditorRenderEventKind::RenderRouted);
  EXPECT_TRUE(render_->render_busy());

  coordinator_->NotifySchedulerCompleted(event.request_id, true);
  EXPECT_FALSE(render_->render_busy());
}

// ── Deferred initial render and stale results ───────────────────────────────

// The render intent carries no parameter values. The render port binds the element's live
// document, so a deferred first frame renders whatever that document holds when it runs.
TEST_F(EditorSessionRenderControllerTest, DeferredInitialRenderPresentsBoundDocument) {
  render_->SetPresentationSinkId(0);
  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  lifecycle_->MarkImageReady();
  render_->MarkImageAcquired();

  EditorRenderCommand command;
  command.reason       = EditorRenderReason::InitialFrame;
  command.operation_id = 9;
  EXPECT_EQ(render_->RouteInitialRender(command, lifecycle_->identity(),
                                        lifecycle_->active_image_load_request()),
            0u);
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);
  EXPECT_TRUE(sched->scheduled().empty());

  // Clearing the sink also cleared the size, so the deferred render waits for both.
  render_->SetPresentationSinkId(1);
  EXPECT_TRUE(sched->scheduled().empty());
  render_->SetPresentationSize(640, 480);
  ASSERT_EQ(sched->scheduled().size(), 1u);
  const auto& intent = sched->scheduled().front().intent;
  EXPECT_EQ(intent.reason, EditorRenderReason::InitialFrame);
  EXPECT_EQ(intent.operation_id, 9u);
  EXPECT_EQ(intent.element_id, lifecycle_->identity().element_id);
  EXPECT_EQ(intent.image_id, lifecycle_->identity().image_id);
  EXPECT_EQ(intent.image_load_request_id, lifecycle_->active_image_load_request());
  EXPECT_EQ(intent.presentation_sink_id, 1u);
}

TEST_F(EditorSessionRenderControllerTest, QualityBaseAfterFirstFrameTargetsSameImage) {
  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  lifecycle_->MarkImageReady();
  render_->MarkImageAcquired();

  EditorRenderCommand command;
  command.reason = EditorRenderReason::InitialFrame;
  render_->RouteInitialRender(command, lifecycle_->identity(),
                              lifecycle_->active_image_load_request());

  // Drive first-frame to completion then presentation.
  auto*      sched         = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  const auto ff_request_id = sched->scheduled().front().request_id;
  coordinator_->NotifySchedulerCompleted(ff_request_id, true);

  // The QualityBase follow-up renders the same image and load request.
  ASSERT_GE(sched->scheduled().size(), 2u);
  const auto& first_intent = sched->scheduled().front().intent;
  const auto& qb_intent    = sched->scheduled().back().intent;
  EXPECT_EQ(qb_intent.frame_role, FrameRole::QualityBase);
  EXPECT_EQ(qb_intent.element_id, first_intent.element_id);
  EXPECT_EQ(qb_intent.image_id, first_intent.image_id);
  EXPECT_EQ(qb_intent.image_load_request_id, first_intent.image_load_request_id);
}

TEST_F(EditorSessionRenderControllerTest, StaleSessionGenerationResultDoesNotAdvanceFirstFrame) {
  ASSERT_TRUE(lifecycle_->BeginAcquire(1, 2, false, nullptr, nullptr));
  ASSERT_TRUE(lifecycle_->AcquireGuards(nullptr));
  lifecycle_->MarkImageReady();
  render_->MarkImageAcquired();

  EditorRenderCommand command;
  command.reason           = EditorRenderReason::InitialFrame;
  const auto         ff_id = render_->RouteInitialRender(command, lifecycle_->identity(),
                                                         lifecycle_->active_image_load_request());

  // Construct a render result referencing the stale identity.
  EditorRenderResult stale_result;
  stale_result.request_id                = ff_id;
  stale_result.kind                      = EditorRenderResultKind::FrameReady;
  stale_result.intent.image_load_request_id = ImageLoadRequestId{99};
  stale_result.intent.image_id           = lifecycle_->identity().image_id;
  stale_result.intent.element_id         = lifecycle_->identity().element_id;

  const auto accepted_before             = coordinator_->diagnostics().accepted_count;
  render_->NotifyRenderResult(stale_result, lifecycle_->identity(),
                              lifecycle_->active_image_load_request(),
                              EditorSessionState::Loading);
  EXPECT_TRUE(std::none_of(events_.begin(), events_.end(), [](const EditorRenderEvent& event) {
    return event.kind == EditorRenderEventKind::FirstFrameReady;
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
  const auto id1              = render_->RouteInitialRender(cmd1, lifecycle_->identity(),
                                                            lifecycle_->active_image_load_request());
  EXPECT_NE(id1, 0u);

  // Route a second render (e.g. undo/redo).
  EditorRenderCommand cmd2;
  cmd2.reason                 = EditorRenderReason::UndoRedo;
  const auto id2              = render_->RouteInitialRender(cmd2, lifecycle_->identity(),
                                                            lifecycle_->active_image_load_request());
  // Undo/redo should still submit a render, just not as a first-frame.
  EXPECT_NE(id2, 0u);
}

// The window is minimized while frames are rendering: the viewport sink refuses
// each frame with the presentation-suspended message. Restoring the window must
// render the current state again instead of leaving the refusals as failures.
auto SuspendedSinkMessage() -> std::string {
  return std::string(kPresentationSuspendedMessage) + "test viewport is minimized";
}

TEST_F(EditorSessionRenderControllerTest, FirstFrameRefusedWhileMinimizedRendersAgainOnRestore) {
  OpenImage();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);
  ASSERT_EQ(sched->scheduled().size(), 1u);

  render_->SetPresentationSuspended(true, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  coordinator_->NotifySchedulerCompleted(sched->scheduled().front().request_id, false,
                                         SuspendedSinkMessage());

  // A refusal is neither a render failure nor a reason to render while minimized.
  EXPECT_FALSE(std::any_of(events_.begin(), events_.end(), [](const EditorRenderEvent& event) {
    return event.kind == EditorRenderEventKind::RenderFailed;
  }));
  EXPECT_EQ(coordinator_->diagnostics().failed_count, 0u);
  EXPECT_TRUE(coordinator_->diagnostics().last_error.empty());
  EXPECT_EQ(sched->scheduled().size(), 1u);
  EXPECT_EQ(lifecycle_->state(), EditorSessionState::Loading);

  render_->SetPresentationSuspended(false, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  ASSERT_EQ(sched->scheduled().size(), 2u);
  const auto& retry = sched->scheduled().back();
  EXPECT_EQ(retry.intent.reason, EditorRenderReason::Retry);
  EXPECT_EQ(retry.intent.frame_role, FrameRole::InteractivePrimary);
  EXPECT_EQ(render_->first_frame_request_id(), retry.request_id);

  coordinator_->NotifySchedulerCompleted(retry.request_id, true);
  EXPECT_TRUE(std::any_of(events_.begin(), events_.end(), [](const EditorRenderEvent& event) {
    return event.kind == EditorRenderEventKind::FirstFrameReady;
  }));
  // The first frame then routes the QualityBase as on a normal open.
  ASSERT_EQ(sched->scheduled().size(), 3u);
  EXPECT_EQ(sched->scheduled().back().intent.frame_role, FrameRole::QualityBase);
}

TEST_F(EditorSessionRenderControllerTest,
       QualityBaseRefusedWhileMinimizedRendersAgainAndReleasesDeferredDetail) {
  OpenImage();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);
  coordinator_->NotifySchedulerCompleted(sched->scheduled().front().request_id, true);
  lifecycle_->MarkFirstFrameReady();
  ASSERT_EQ(sched->scheduled().size(), 2u);
  const auto quality_request_id = sched->scheduled().back().request_id;

  render_->SetPresentationSuspended(true, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  coordinator_->NotifySchedulerCompleted(quality_request_id, false, SuspendedSinkMessage());

  // A zoom while minimized waits for the QualityBase that was refused.
  EditorRenderCommand detail;
  detail.reason = EditorRenderReason::DetailRefresh;
  ViewportRenderRegion region{};
  region.x_          = 20;
  detail.view_region = region;
  EXPECT_EQ(render_
                ->RouteViewChange(detail, lifecycle_->identity(),
                                  lifecycle_->active_image_load_request(),
                                  EditorSessionState::Interactive)
                .kind,
            EditorRenderEventKind::RenderReused);
  EXPECT_EQ(sched->scheduled().size(), 2u);

  render_->SetPresentationSuspended(false, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  ASSERT_EQ(sched->scheduled().size(), 3u);
  const auto& refresh = sched->scheduled().back();
  EXPECT_EQ(refresh.intent.frame_role, FrameRole::QualityBase);
  EXPECT_EQ(refresh.intent.operation_id, 0u);

  // The new QualityBase unblocks the zoomed detail patch.
  coordinator_->NotifySchedulerCompleted(refresh.request_id, true);
  ASSERT_EQ(sched->scheduled().size(), 4u);
  const auto& detail_intent = sched->scheduled().back().intent;
  EXPECT_EQ(detail_intent.frame_role, FrameRole::DetailPatch);
  ASSERT_TRUE(detail_intent.view_region.has_value());
  EXPECT_EQ(detail_intent.view_region->x_, 20);
}

TEST_F(EditorSessionRenderControllerTest, DetailRefusedWhileMinimizedRoutesOnlyTheDetailOnRestore) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->MarkFirstFrameReady();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);

  EditorRenderCommand detail;
  detail.reason = EditorRenderReason::DetailRefresh;
  ViewportRenderRegion region{};
  region.x_          = 33;
  detail.view_region = region;
  ASSERT_EQ(render_
                ->RouteViewChange(detail, lifecycle_->identity(),
                                  lifecycle_->active_image_load_request(),
                                  EditorSessionState::Interactive)
                .kind,
            EditorRenderEventKind::RenderRouted);
  const auto detail_request_id = sched->scheduled().back().request_id;

  render_->SetPresentationSuspended(true, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  coordinator_->NotifySchedulerCompleted(detail_request_id, false, SuspendedSinkMessage());
  const auto scheduled_before_restore = sched->scheduled().size();

  render_->SetPresentationSuspended(false, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  ASSERT_EQ(sched->scheduled().size(), scheduled_before_restore + 1u);
  const auto& routed = sched->scheduled().back().intent;
  EXPECT_EQ(routed.frame_role, FrameRole::DetailPatch);
  ASSERT_TRUE(routed.view_region.has_value());
  EXPECT_EQ(routed.view_region->x_, 33);
}

TEST_F(EditorSessionRenderControllerTest, RefusalThatArrivesAfterRestoreRoutesOneRefresh) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->MarkFirstFrameReady();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);

  // The window is restored before the refusal of a frame reaches the owner.
  render_->SetPresentationSuspended(true, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  render_->SetPresentationSuspended(false, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  EditorRenderCommand settled;
  settled.reason = EditorRenderReason::SettledAdjustment;
  const auto edit_request_id =
      render_->RouteInitialRender(settled, lifecycle_->identity(),
                                  lifecycle_->active_image_load_request());
  ASSERT_NE(edit_request_id, 0u);
  const auto scheduled_before = sched->scheduled().size();

  coordinator_->NotifySchedulerCompleted(edit_request_id, false, SuspendedSinkMessage());
  ASSERT_EQ(sched->scheduled().size(), scheduled_before + 1u);
  const auto refresh_request_id = sched->scheduled().back().request_id;

  // A viewport that keeps refusing without a new resume must not loop.
  coordinator_->NotifySchedulerCompleted(refresh_request_id, false, SuspendedSinkMessage());
  EXPECT_EQ(sched->scheduled().size(), scheduled_before + 1u);
}

auto ExportRefusalMessage() -> std::string {
  return std::string(kPresentationSuspendedMessage) + "an export is running";
}

TEST_F(EditorSessionRenderControllerTest, EditRefusedDuringExportRendersOnceWhenTheExportEnds) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->MarkFirstFrameReady();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);

  render_->SetSuspendedForExport(true, lifecycle_->identity(),
                                 lifecycle_->active_image_load_request(), lifecycle_->state());
  EXPECT_TRUE(render_->suspended_for_export());
  EditorRenderCommand settled;
  settled.reason = EditorRenderReason::SettledAdjustment;
  const auto edit_request_id =
      render_->RouteInitialRender(settled, lifecycle_->identity(),
                                  lifecycle_->active_image_load_request());
  ASSERT_NE(edit_request_id, 0u);
  const auto scheduled_before = sched->scheduled().size();

  // The port refused the frame because the export holds the GPU: no failure, no render until
  // the export ends.
  coordinator_->NotifySchedulerCompleted(edit_request_id, false, ExportRefusalMessage());
  EXPECT_EQ(sched->scheduled().size(), scheduled_before);
  EXPECT_EQ(coordinator_->diagnostics().failed_count, 0u);
  EXPECT_FALSE(std::any_of(events_.begin(), events_.end(), [](const EditorRenderEvent& event) {
    return event.kind == EditorRenderEventKind::RenderFailed;
  }));

  render_->SetSuspendedForExport(false, lifecycle_->identity(),
                                 lifecycle_->active_image_load_request(), lifecycle_->state());
  EXPECT_FALSE(render_->suspended_for_export());
  ASSERT_EQ(sched->scheduled().size(), scheduled_before + 1u);
  EXPECT_EQ(sched->scheduled().back().intent.frame_role, FrameRole::QualityBase);
}

TEST_F(EditorSessionRenderControllerTest, ExportEndWhileMinimizedWaitsForTheRestoreToRender) {
  OpenImage();
  PresentFirstFrame();
  lifecycle_->MarkFirstFrameReady();
  auto* sched = dynamic_cast<EditorSessionBootstrapSchedulerPort*>(scheduler_.get());
  ASSERT_NE(sched, nullptr);

  render_->SetPresentationSuspended(true, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  render_->SetSuspendedForExport(true, lifecycle_->identity(),
                                 lifecycle_->active_image_load_request(), lifecycle_->state());
  EditorRenderCommand settled;
  settled.reason = EditorRenderReason::SettledAdjustment;
  const auto edit_request_id =
      render_->RouteInitialRender(settled, lifecycle_->identity(),
                                  lifecycle_->active_image_load_request());
  ASSERT_NE(edit_request_id, 0u);
  coordinator_->NotifySchedulerCompleted(edit_request_id, false, ExportRefusalMessage());
  const auto scheduled_before = sched->scheduled().size();

  // The export ended, but the window is still minimized: the viewport would refuse the frame.
  render_->SetSuspendedForExport(false, lifecycle_->identity(),
                                 lifecycle_->active_image_load_request(), lifecycle_->state());
  EXPECT_EQ(sched->scheduled().size(), scheduled_before);

  render_->SetPresentationSuspended(false, lifecycle_->identity(),
                                    lifecycle_->active_image_load_request(), lifecycle_->state());
  ASSERT_EQ(sched->scheduled().size(), scheduled_before + 1u);
  EXPECT_EQ(sched->scheduled().back().intent.frame_role, FrameRole::QualityBase);
}

}  // namespace
}  // namespace alcedo
