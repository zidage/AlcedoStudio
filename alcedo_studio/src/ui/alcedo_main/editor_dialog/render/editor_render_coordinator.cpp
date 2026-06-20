//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/editor_dialog/render/editor_render_coordinator.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "image/image_buffer.hpp"
#include "ui/alcedo_main/editor_dialog/widgets/spinner.hpp"
#include "ui/edit_viewer/edit_viewer.hpp"

namespace alcedo::ui {
namespace {

using namespace std::chrono_literals;

constexpr float kDetailPreviewZoomEpsilon = 1.0e-4f;

}  // namespace

EditorRenderCoordinator::EditorRenderCoordinator(Dependencies dependencies, Callbacks callbacks)
    : dependencies_(std::move(dependencies)), callbacks_(std::move(callbacks)) {}

void EditorRenderCoordinator::AdvancePreviewGeneration() {
  ++preview_generation_;
  detail_serial_                        = 0;
  latest_quality_base_generation_ready_ = 0;
  pending_fast_preview_request_.reset();
  pending_quality_base_render_request_.reset();
  detail_preview_waiting_for_quality_base_ = false;
  if (quality_preview_timer_ && quality_preview_timer_->isActive()) {
    quality_preview_timer_->stop();
  }
  if (fast_preview_submit_timer_ && fast_preview_submit_timer_->isActive()) {
    fast_preview_submit_timer_->stop();
  }
  InvalidateDetailPreviewState();
}

void EditorRenderCoordinator::InvalidateDetailPreviewState() {
  pending_detail_render_request_.reset();
  detail_preview_waiting_for_quality_base_ = false;
  if (detail_preview_timer_ && detail_preview_timer_->isActive()) {
    detail_preview_timer_->stop();
  }
  if (auto* viewer = CurrentViewer()) {
    viewer->ClearExpectedDetailToken();
  }
}

auto EditorRenderCoordinator::BuildPreviewMetadata(RenderType render_type) const
    -> FramePreviewMetadata {
  FramePreviewMetadata metadata{};
  metadata.preview_generation = preview_generation_;
  switch (render_type) {
    case RenderType::FAST_PREVIEW:
      metadata.frame_role = FrameRole::InteractivePrimary;
      break;
    case RenderType::QUALITY_BASE_PREVIEW:
      metadata.frame_role = FrameRole::QualityBase;
      break;
    case RenderType::DETAIL_ROI_PREVIEW:
      metadata.frame_role    = FrameRole::DetailPatch;
      metadata.detail_serial = detail_serial_;
      break;
    default:
      metadata.frame_role = FrameRole::QualityBase;
      break;
  }
  return metadata;
}

auto EditorRenderCoordinator::IsDetailPreviewGeometryFallbackActive() const -> bool {
  const auto rotation =
      dependencies_.state != nullptr ? dependencies_.state->rotate_degrees_ : 0.0f;
  const bool needs_full_frame = callbacks_.needs_full_frame_preview_after_geometry_commit
                                    ? callbacks_.needs_full_frame_preview_after_geometry_commit()
                                    : false;
  return std::abs(rotation) > 1.0e-4f || needs_full_frame;
}

auto EditorRenderCoordinator::WantsDetailPreviewFromViewport() const -> bool {
  auto* viewer = CurrentViewer();
  if (!viewer) {
    return false;
  }
  if (viewer->GetViewZoom() <= (1.0f + kDetailPreviewZoomEpsilon)) {
    return false;
  }
  if (IsDetailPreviewGeometryFallbackActive()) {
    return false;
  }
  const auto viewport_region = viewer->GetViewportRenderRegion();
  return viewport_region.has_value();
}

auto EditorRenderCoordinator::CanScheduleDetailPreview() const -> bool {
  if (!WantsDetailPreviewFromViewport()) {
    return false;
  }
  return preview_generation_ != 0 && latest_quality_base_generation_ready_ == preview_generation_;
}

void EditorRenderCoordinator::MaybeScheduleDetailPreviewRenderFromViewport() {
  if (!WantsDetailPreviewFromViewport()) {
    InvalidateDetailPreviewState();
    return;
  }
  if (!CanScheduleDetailPreview()) {
    detail_preview_waiting_for_quality_base_ = true;
    return;
  }
  detail_preview_waiting_for_quality_base_ = false;
  ScheduleDetailPreviewRenderFromViewport();
}

void EditorRenderCoordinator::RequestQualityPreviewRenderFromPipeline(bool apply_state) {
  if (!dependencies_.state) {
    return;
  }
  if (quality_preview_timer_ && quality_preview_timer_->isActive()) {
    quality_preview_timer_->stop();
  }

  AdjustmentState snapshot = *dependencies_.state;
  snapshot.type_           = RenderType::QUALITY_BASE_PREVIEW;
  EnqueueRenderRequest(snapshot, BuildPreviewMetadata(snapshot.type_),
                       apply_state,
                       /*use_viewport_region=*/false);
}

void EditorRenderCoordinator::TriggerQualityPreviewRenderFromPipeline() {
  RequestQualityPreviewRenderFromPipeline(/*apply_state=*/false);
}

void EditorRenderCoordinator::ScheduleQualityPreviewRenderFromPipeline() {
  EnsureQualityPreviewTimer();
  quality_preview_timer_->start(static_cast<int>(kQualityPreviewDebounceInterval.count()));
}

void EditorRenderCoordinator::ScheduleDetailPreviewRenderFromViewport() {
  EnsureDetailPreviewTimer();
  detail_preview_timer_->start(static_cast<int>(kViewportDetailDebounceInterval.count()));
}

void EditorRenderCoordinator::TriggerDetailPreviewRenderFromViewport() {
  auto* viewer = CurrentViewer();
  if (!CanScheduleDetailPreview() || !viewer || !dependencies_.state) {
    InvalidateDetailPreviewState();
    return;
  }

  ++detail_serial_;

  AdjustmentState snapshot = *dependencies_.state;
  snapshot.type_           = RenderType::DETAIL_ROI_PREVIEW;
  EnqueueRenderRequest(snapshot, BuildPreviewMetadata(snapshot.type_),
                       /*apply_state=*/false,
                       /*use_viewport_region=*/true);
}

auto EditorRenderCoordinator::CanSubmitFastPreviewNow() const -> bool {
  return controllers::render::CanSubmitFastPreviewNow(last_fast_preview_submit_time_,
                                                      std::chrono::steady_clock::now());
}

void EditorRenderCoordinator::EnqueueRenderRequest(const AdjustmentState&      snapshot,
                                                   const FramePreviewMetadata& frame_metadata,
                                                   bool apply_state, bool use_viewport_region) {
  PendingRenderRequest request{snapshot, frame_metadata, apply_state, use_viewport_region};

  switch (snapshot.type_) {
    case RenderType::FAST_PREVIEW:
      pending_fast_preview_request_ = std::move(request);
      break;
    case RenderType::QUALITY_BASE_PREVIEW:
      pending_quality_base_render_request_ = std::move(request);
      break;
    case RenderType::DETAIL_ROI_PREVIEW:
      pending_detail_render_request_ = std::move(request);
      break;
    default:
      pending_quality_base_render_request_ = std::move(request);
      break;
  }

  if (!inflight_) {
    StartNext();
  }
}

void EditorRenderCoordinator::RequestRender(bool use_viewport_region,
                                            bool bump_preview_generation) {
  if (!dependencies_.state) {
    return;
  }
  if (bump_preview_generation) {
    AdvancePreviewGeneration();
  }

  AdjustmentState snapshot = *dependencies_.state;
  snapshot.type_           = RenderType::FAST_PREVIEW;
  EnqueueRenderRequest(snapshot, BuildPreviewMetadata(snapshot.type_),
                       /*apply_state=*/true, use_viewport_region);
}

void EditorRenderCoordinator::RequestRenderWithoutApplyingState(bool use_viewport_region,
                                                                bool bump_preview_generation) {
  if (!dependencies_.state) {
    return;
  }
  if (bump_preview_generation) {
    AdvancePreviewGeneration();
  }

  AdjustmentState snapshot = *dependencies_.state;
  snapshot.type_           = RenderType::FAST_PREVIEW;
  EnqueueRenderRequest(snapshot, BuildPreviewMetadata(snapshot.type_),
                       /*apply_state=*/false, use_viewport_region);
}

void EditorRenderCoordinator::PollInflight() {
  if (!inflight_future_.has_value()) {
    if (poll_timer_ && poll_timer_->isActive() && !inflight_) {
      poll_timer_->stop();
    }
    return;
  }

  if (inflight_future_->wait_for(0ms) != std::future_status::ready) {
    return;
  }

  bool render_succeeded = false;
  try {
    render_succeeded = static_cast<bool>(inflight_future_->get());
  } catch (...) {
  }
  inflight_future_.reset();
  OnRenderFinished(render_succeeded);
}

void EditorRenderCoordinator::StartNext() {
  if (inflight_) {
    return;
  }

  std::optional<PendingRenderRequest> request;
  if (pending_quality_base_render_request_.has_value()) {
    request = pending_quality_base_render_request_;
    pending_quality_base_render_request_.reset();
  } else if (pending_detail_render_request_.has_value()) {
    request = pending_detail_render_request_;
    pending_detail_render_request_.reset();
  } else if (pending_fast_preview_request_.has_value()) {
    if (!CanSubmitFastPreviewNow()) {
      ArmFastPreviewSubmitTimer();
      return;
    }
    request = pending_fast_preview_request_;
    pending_fast_preview_request_.reset();
    last_fast_preview_submit_time_ = std::chrono::steady_clock::now();
    if (fast_preview_submit_timer_ && fast_preview_submit_timer_->isActive()) {
      fast_preview_submit_timer_->stop();
    }
  }

  if (!request.has_value() || !dependencies_.base_task || !dependencies_.scheduler) {
    return;
  }
  const PendingRenderRequest next_request = *request;

  if (auto* spinner = CurrentSpinner()) {
    spinner->Start();
  }

  if (next_request.apply_state_) {
    if (callbacks_.apply_state_to_pipeline) {
      callbacks_.apply_state_to_pipeline(next_request.state_);
    }
    if (dependencies_.pipeline_guard) {
      dependencies_.pipeline_guard->dirty_ = true;
    }
  }

  PipelineTask task                               = *dependencies_.base_task;
  task.options_.render_desc_.render_type_         = next_request.state_.type_;
  task.options_.render_desc_.use_viewport_region_ = next_request.use_viewport_region_;
  task.options_.render_desc_.frame_metadata_      = next_request.frame_metadata_;
  task.options_.is_callback_                      = false;
  task.options_.is_seq_callback_                  = false;
  task.options_.is_blocking_                      = true;

  auto promise      = std::make_shared<std::promise<std::shared_ptr<ImageBuffer>>>();
  auto fut          = promise->get_future();
  task.result_      = promise;

  inflight_         = true;
  inflight_request_ = next_request;
  dependencies_.scheduler->ScheduleTask(std::move(task));

  inflight_future_ = std::move(fut);
  EnsurePollTimer();
  if (poll_timer_ && !poll_timer_->isActive()) {
    poll_timer_->start();
  }
}

void EditorRenderCoordinator::OnRenderFinished(bool render_succeeded) {
  inflight_ = false;

  if (auto* spinner = CurrentSpinner()) {
    spinner->Stop();
  }

  const std::optional<PendingRenderRequest> finished_request = inflight_request_;
  inflight_request_.reset();

  if (callbacks_.refresh_color_temp_runtime_state &&
      callbacks_.refresh_color_temp_runtime_state()) {
    if (callbacks_.sync_color_temp_controls) {
      callbacks_.sync_color_temp_controls();
    }
  }

  if (render_succeeded) {
    if (auto* viewer = CurrentViewer()) {
      viewer->SyncPendingFrameStateForScheduling();
    }
  }

  if (render_succeeded && finished_request.has_value() &&
      finished_request->state_.type_ == RenderType::QUALITY_BASE_PREVIEW &&
      finished_request->frame_metadata_.preview_generation == preview_generation_) {
    latest_quality_base_generation_ready_ = preview_generation_;
    if (callbacks_.clear_full_frame_preview_after_geometry_commit) {
      callbacks_.clear_full_frame_preview_after_geometry_commit();
    }
    if (detail_preview_waiting_for_quality_base_ || WantsDetailPreviewFromViewport()) {
      MaybeScheduleDetailPreviewRenderFromViewport();
    }
  }

  if (render_succeeded && finished_request.has_value() &&
      finished_request->state_.type_ == RenderType::DETAIL_ROI_PREVIEW &&
      finished_request->frame_metadata_.preview_generation == preview_generation_) {
    if (auto* viewer = CurrentViewer()) {
      viewer->SetExpectedDetailToken(finished_request->frame_metadata_.preview_generation,
                                     finished_request->frame_metadata_.detail_serial);
    }
  }

  if (pending_quality_base_render_request_.has_value() ||
      pending_detail_render_request_.has_value() || pending_fast_preview_request_.has_value()) {
    StartNext();
  } else if (poll_timer_ && poll_timer_->isActive()) {
    poll_timer_->stop();
  }
}

auto EditorRenderCoordinator::CurrentViewer() const -> QtEditViewer* {
  return callbacks_.viewer ? callbacks_.viewer() : nullptr;
}

auto EditorRenderCoordinator::CurrentSpinner() const -> SpinnerWidget* {
  return callbacks_.spinner ? callbacks_.spinner() : nullptr;
}

auto EditorRenderCoordinator::CurrentActivePanel() const -> ControlPanelKind {
  return callbacks_.active_panel ? callbacks_.active_panel() : ControlPanelKind::Tone;
}

void EditorRenderCoordinator::EnsureQualityPreviewTimer() {
  if (quality_preview_timer_) {
    return;
  }
  quality_preview_timer_ = new QTimer(dependencies_.timer_parent);
  quality_preview_timer_->setSingleShot(true);
  QObject::connect(quality_preview_timer_, &QTimer::timeout, dependencies_.timer_parent,
                   [this]() { TriggerQualityPreviewRenderFromPipeline(); });
}

void EditorRenderCoordinator::EnsureDetailPreviewTimer() {
  if (detail_preview_timer_) {
    return;
  }
  detail_preview_timer_ = new QTimer(dependencies_.timer_parent);
  detail_preview_timer_->setSingleShot(true);
  QObject::connect(detail_preview_timer_, &QTimer::timeout, dependencies_.timer_parent,
                   [this]() { TriggerDetailPreviewRenderFromViewport(); });
}

void EditorRenderCoordinator::EnsureFastPreviewSubmitTimer() {
  if (fast_preview_submit_timer_) {
    return;
  }
  fast_preview_submit_timer_ = new QTimer(dependencies_.timer_parent);
  fast_preview_submit_timer_->setSingleShot(true);
  QObject::connect(fast_preview_submit_timer_, &QTimer::timeout, dependencies_.timer_parent,
                   [this]() {
                     if (!inflight_) {
                       StartNext();
                     }
                   });
}

void EditorRenderCoordinator::ArmFastPreviewSubmitTimer() {
  EnsureFastPreviewSubmitTimer();
  const int delay_ms = controllers::render::ComputeFastPreviewDelayMs(
      last_fast_preview_submit_time_, std::chrono::steady_clock::now());

  if (!fast_preview_submit_timer_->isActive()) {
    fast_preview_submit_timer_->start(delay_ms);
    return;
  }

  const int current_remaining = fast_preview_submit_timer_->remainingTime();
  if (current_remaining < 0 || delay_ms < current_remaining) {
    fast_preview_submit_timer_->start(delay_ms);
  }
}

void EditorRenderCoordinator::EnsurePollTimer() {
  if (poll_timer_) {
    return;
  }
  poll_timer_ = new QTimer(dependencies_.timer_parent);
  poll_timer_->setInterval(4);
  QObject::connect(poll_timer_, &QTimer::timeout, dependencies_.timer_parent,
                   [this]() { PollInflight(); });
}

}  // namespace alcedo::ui
