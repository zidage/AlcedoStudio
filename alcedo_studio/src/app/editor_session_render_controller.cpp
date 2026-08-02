//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_render_controller.hpp"

#include <algorithm>
#include <iostream>
#include <utility>

namespace alcedo {

EditorSessionRenderController::EditorSessionRenderController(Dependencies dependencies)
    : deps_(std::move(dependencies)) {}

EditorSessionRenderController::~EditorSessionRenderController() {
  // The facade owns lifecycle and cancels sessions in its own destructor.
}

void EditorSessionRenderController::SetPresentationSinkId(PresentationSinkId sink_id) {
  EditorRenderCommand   pending_command;
  EditorSessionIdentity pending_identity;
  ImageLoadRequestId    pending_load_request;
  bool                  has_pending = false;
  {
    std::scoped_lock lock(mutex_);
    presentation_sink_id_ = sink_id;
    if (sink_id == 0) {
      presentation_width_  = 0;
      presentation_height_ = 0;
      return;
    }
    if (pending_initial_reason_.has_value()) {
      pending_command.reason       = *pending_initial_reason_;
    pending_command.operation_id = pending_operation_id_;
    pending_command.adjustment   = pending_initial_adjustment_;
      pending_identity             = pending_session_identity_;
      pending_load_request         = pending_image_load_request_;
      has_pending                  = true;
    }
  }
  if (has_pending) {
    RoutePendingInitialRender(pending_command, pending_identity, pending_load_request);
  }
}

void EditorSessionRenderController::SetPresentationSize(int width, int height) {
  EditorRenderCommand   pending_command;
  EditorSessionIdentity pending_identity;
  ImageLoadRequestId    pending_load_request;
  bool                  has_pending = false;
  {
    std::scoped_lock lock(mutex_);
    presentation_width_  = std::max(0, width);
    presentation_height_ = std::max(0, height);
    if (pending_initial_reason_.has_value()) {
      pending_command.reason       = *pending_initial_reason_;
    pending_command.operation_id = pending_operation_id_;
    pending_command.adjustment   = pending_initial_adjustment_;
      pending_identity             = pending_session_identity_;
      pending_load_request         = pending_image_load_request_;
      has_pending                  = true;
    }
  }
  if (has_pending) {
    RoutePendingInitialRender(pending_command, pending_identity, pending_load_request);
  }
}

auto EditorSessionRenderController::MakeRenderIntent(const EditorRenderCommand&   command,
                                                     const EditorSessionIdentity& identity,
                                                     ImageLoadRequestId image_load_request) const
    -> std::optional<EditorRenderIntent> {
  if (identity.element_id == 0 || identity.image_id == 0 || !image_load_request.valid()) {
    return std::nullopt;
  }
  EditorRenderIntent intent;
  intent.element_id            = identity.element_id;
  intent.image_id              = identity.image_id;
  intent.operation_id          = command.operation_id;
  intent.image_load_request_id = image_load_request;
  intent.reason                = command.reason;
  intent.quality               = DefaultQualityForReason(command.reason);
  intent.priority              = DefaultPriorityForReason(command.reason);
  intent.frame_role            = FrameRoleForQuality(intent.quality);
  intent.replacement_key       = DefaultReplacementKey(intent.quality);
  intent.adjustment            = command.adjustment;
  intent.geometry_overlay_only = geometry_overlay_active_.load(std::memory_order_acquire);
  intent.requested_width       = presentation_width_;
  intent.requested_height      = presentation_height_;
  intent.presentation_sink_id  = presentation_sink_id_;
  intent.cancellation          = std::make_shared<EditorRenderCancellationToken>();
  FillRenderIntentDefaults(intent);
  return intent;
}

void EditorSessionRenderController::SetGeometryOverlayActive(bool active) {
  geometry_overlay_active_.store(active, std::memory_order_release);
}

auto EditorSessionRenderController::RouteInitialRender(const EditorRenderCommand&   command,
                                                       const EditorSessionIdentity& identity,
                                                       ImageLoadRequestId           image_load_request)
    -> std::uint64_t {
  if (!PresentationTargetReady()) {
    std::scoped_lock lock(mutex_);
    pending_initial_reason_     = command.reason;
    pending_operation_id_       = command.operation_id;
    pending_initial_adjustment_ = command.adjustment;
    pending_session_identity_   = identity;
    pending_image_load_request_ = image_load_request;
    return 0;
  }
  if (!deps_.render) {
    return 0;
  }
  auto intent = MakeRenderIntent(command, identity, image_load_request);
  if (!intent) {
    return 0;
  }
  deps_.render->SetActiveImageLoadRequest(image_load_request.value);
  const EditorRenderResult routed = deps_.render->Submit(*intent);
  if (routed.kind == EditorRenderResultKind::RequestAccepted) {
    if (intent->frame_role == FrameRole::QualityBase) {
      std::scoped_lock lock(mutex_);
      quality_base_routed_     = true;
      quality_base_ready_      = false;
      quality_base_request_id_ = routed.request_id;
    }
    if (command.reason == EditorRenderReason::InitialFrame ||
        command.reason == EditorRenderReason::ImageSwitch ||
        command.reason == EditorRenderReason::Retry) {
      std::scoped_lock lock(mutex_);
      first_frame_request_id_     = routed.request_id;
      first_frame_ready_          = false;
      quality_base_routed_        = false;
      quality_base_ready_         = false;
      quality_base_request_id_    = 0;
      pending_detail_render_.reset();
      first_frame_route_time_     = std::chrono::steady_clock::now();
      first_frame_time_ms_        = -1.0;
      pending_session_identity_   = identity;
      pending_image_load_request_ = image_load_request;
      pending_initial_adjustment_ = command.adjustment;
    }
    std::scoped_lock lock(mutex_);
    pending_initial_reason_.reset();
    EditorRenderEvent event;
    event.kind         = EditorRenderEventKind::RenderRouted;
    event.operation_id = command.operation_id;
    event.request_id   = routed.request_id;
    event.state        = EditorSessionState::Loading;
    event.identity     = identity;
    event.reason       = command.reason;
    EmitEvent(std::move(event));
    return routed.request_id;
  }
  return 0;
}

auto EditorSessionRenderController::RouteViewChange(const EditorRenderCommand&   command,
                                                    const EditorSessionIdentity& identity,
                                                    ImageLoadRequestId           image_load_request,
                                                    EditorSessionState           state)
    -> EditorRenderEvent {
  EditorRenderEvent event;
  const bool        detail_from_visible_loading_frame =
      command.reason == EditorRenderReason::DetailRefresh && state == EditorSessionState::Loading &&
      image_acquired_ && first_frame_ready_;
  if (state != EditorSessionState::Interactive && !detail_from_visible_loading_frame) {
    event.kind    = EditorRenderEventKind::RenderRejected;
    event.message = "View change requires an interactive session";
    return event;
  }
  if (identity.element_id == 0 || identity.image_id == 0 || !image_load_request.valid()) {
    event.kind    = EditorRenderEventKind::RenderRejected;
    event.message = "View change requires an open image";
    return event;
  }

  // A detail patch is a refinement of the current QualityBase. Never enqueue
  // it ahead of that base: doing so both wastes pipeline work and can produce a
  // patch whose content generation has no matching full-frame layer. Keep only
  // the newest stable viewport request and submit it when QualityBase is ready.
  if (command.reason == EditorRenderReason::DetailRefresh) {
    std::scoped_lock lock(mutex_);
    if (!quality_base_ready_) {
      pending_detail_render_ = PendingDetailRender{command, identity, image_load_request};
      std::cout << "[ROI_TRACE][detail-deferred] image=" << identity.image_id
                << " image_load_request=" << image_load_request.value
                << " quality_base_request=" << quality_base_request_id_;
      if (command.view_region.has_value()) {
        const auto& region = *command.view_region;
        std::cout << " region_px=" << region.x_ << ',' << region.y_
                  << " scale=" << region.scale_x_ << ',' << region.scale_y_;
      }
      std::cout << '\n';
      event.kind              = EditorRenderEventKind::RenderReused;
      event.state             = state;
      event.identity          = identity;
      event.operation_id      = command.operation_id;
      event.reason            = command.reason;
      event.message           = "Detail refresh deferred until QualityBase is ready";
      return event;
    }
  }

  if (deps_.render) {
    deps_.render->SetActiveImageLoadRequest(image_load_request.value);
  }

  auto intent_opt = MakeRenderIntent(command, identity, image_load_request);
  if (!intent_opt) {
    event.kind    = EditorRenderEventKind::RenderRejected;
    event.message = "View change render intent could not be built";
    return event;
  }
  intent_opt->view_region = command.view_region;

  EditorRenderResult routed;
  if (deps_.render) {
    routed = deps_.render->Submit(*intent_opt);
  } else {
    routed.kind    = EditorRenderResultKind::Failed;
    routed.message = "Render submit port unavailable";
  }

  event.state        = state;
  event.identity     = identity;
  event.operation_id = command.operation_id;
  event.reason       = command.reason;
  switch (routed.kind) {
    case EditorRenderResultKind::RequestAccepted:
      event.kind       = EditorRenderEventKind::RenderRouted;
      event.request_id = routed.request_id;
      event.message    = "View change render routed";
      break;
    case EditorRenderResultKind::Reused:
      event.kind    = EditorRenderEventKind::RenderReused;
      event.message = "View change reused current frame";
      break;
    default:
      event.kind    = EditorRenderEventKind::RenderRejected;
      event.message = routed.message.empty() ? "View change render rejected" : routed.message;
      break;
  }
  return event;
}

void EditorSessionRenderController::NotifyRenderResult(
    const EditorRenderResult& render_result, const EditorSessionIdentity& identity,
    ImageLoadRequestId image_load_request, EditorSessionState state) {
  if (render_result.intent.image_load_request_id.valid() &&
      render_result.intent.image_load_request_id != image_load_request) {
    return;
  }
  if (render_result.intent.image_id != 0 && render_result.intent.image_id != identity.image_id) {
    return;
  }
  if (render_result.intent.element_id != 0 &&
      render_result.intent.element_id != identity.element_id) {
    return;
  }

  if (render_result.kind == EditorRenderResultKind::Failed) {
    if (state == EditorSessionState::Loading &&
        MatchesActiveFirstFrame(render_result, identity, image_load_request)) {
      EditorRenderEvent event;
      event.kind         = EditorRenderEventKind::RenderFailed;
      event.operation_id = render_result.intent.operation_id;
      event.state        = state;
      event.identity     = identity;
      event.message      = render_result.message.empty() ? "Render failed" : render_result.message;
      EmitEvent(std::move(event));
    }
    return;
  }

  if (MatchesActiveFirstFrame(render_result, identity, image_load_request)) {
    if (render_result.kind == EditorRenderResultKind::FrameReady) {
      bool                  should_try_interactive = false;
      EditorSessionIdentity first_frame_identity;
      {
        std::scoped_lock lock(mutex_);
        if (first_frame_ready_) {
          return;
        }
        first_frame_ready_ = true;
        if (first_frame_route_time_.has_value()) {
          const auto elapsed   = std::chrono::steady_clock::now() - *first_frame_route_time_;
          first_frame_time_ms_ = std::chrono::duration<double, std::milli>(elapsed).count();
        }
        first_frame_identity   = pending_session_identity_;
        should_try_interactive = true;
      }
      if (should_try_interactive) {
        TryEnterInteractiveFromFirstFrame(first_frame_identity);
      }
    }
    return;
  }

  std::optional<PendingDetailRender> pending_detail;
  {
    std::scoped_lock lock(mutex_);
    const bool matches_quality_base =
        quality_base_request_id_ != 0 &&
        render_result.request_id == quality_base_request_id_ &&
        render_result.intent.frame_role == FrameRole::QualityBase &&
        identity.element_id == pending_session_identity_.element_id &&
        identity.image_id == pending_session_identity_.image_id &&
        image_load_request == pending_image_load_request_;
    if (matches_quality_base && render_result.kind == EditorRenderResultKind::FrameReady) {
      quality_base_ready_ = true;
      pending_detail      = std::move(pending_detail_render_);
      pending_detail_render_.reset();
      std::cout << "[ROI_TRACE][quality-base-ready] request=" << render_result.request_id
                << " image=" << identity.image_id
                << " pending_detail=" << (pending_detail.has_value() ? 1 : 0) << '\n';
    }
  }
  if (pending_detail.has_value()) {
    const auto routed = RouteViewChange(pending_detail->command, pending_detail->identity,
                                        pending_detail->image_load_request, state);
    std::cout << "[ROI_TRACE][detail-after-base] quality_base_request="
              << render_result.request_id << " detail_request=" << routed.request_id
              << " outcome=" << static_cast<int>(routed.kind) << '\n';
  }

  const bool busy          = CoordinatorBusy();
  bool       should_notify = false;
  {
    std::scoped_lock lock(mutex_);
    if (busy != last_notified_render_busy_) {
      last_notified_render_busy_ = busy;
      should_notify              = true;
    }
  }
  if (should_notify) {
    EditorRenderEvent event;
    event.kind         = EditorRenderEventKind::BusyChanged;
    event.operation_id = render_result.intent.operation_id;
    event.state        = state;
    event.identity     = identity;
    EmitEvent(std::move(event));
  }
}

auto EditorSessionRenderController::PresentationTargetReady() const -> bool {
  std::scoped_lock lock(mutex_);
  return presentation_sink_id_ != 0 && presentation_width_ > 0 && presentation_height_ > 0;
}

auto EditorSessionRenderController::first_frame_request_id() const -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  return first_frame_request_id_;
}

auto EditorSessionRenderController::first_frame_time_ms() const -> double {
  std::scoped_lock lock(mutex_);
  return first_frame_time_ms_;
}

auto EditorSessionRenderController::render_busy() const -> bool { return CoordinatorBusy(); }

auto EditorSessionRenderController::render_diagnostics() const
    -> EditorRenderCoordinatorDiagnostics {
  if (!deps_.render) {
    return {};
  }
  return deps_.render->diagnostics();
}

auto EditorSessionRenderController::presentation_sink_id() const -> PresentationSinkId {
  std::scoped_lock lock(mutex_);
  return presentation_sink_id_;
}

void EditorSessionRenderController::ResetForNewImage() {
  std::scoped_lock lock(mutex_);
  first_frame_request_id_  = 0;
  quality_base_request_id_ = 0;
  image_acquired_          = false;
  first_frame_ready_       = false;
  quality_base_routed_     = false;
  quality_base_ready_      = false;
  pending_detail_render_.reset();
  pending_initial_reason_.reset();
  pending_operation_id_       = 0;
  pending_initial_adjustment_ = {};
  pending_session_identity_   = {};
  pending_image_load_request_ = {};
  first_frame_route_time_.reset();
  first_frame_time_ms_ = -1.0;
}

void EditorSessionRenderController::MarkImageAcquired() {
  std::scoped_lock lock(mutex_);
  image_acquired_ = true;
}

void EditorSessionRenderController::CancelSessionAndWait(ImageLoadRequestId image_load_request) {
  if (deps_.render && image_load_request.valid()) {
    deps_.render->CancelSessionAndWait(image_load_request.value);
  }
}

void EditorSessionRenderController::TryEnterInteractiveFromFirstFrame(
    const EditorSessionIdentity& identity) {
  bool ready = false;
  ImageLoadRequestId load_request;
  {
    std::scoped_lock lock(mutex_);
    if (!image_acquired_ || !first_frame_ready_) {
      return;
    }
    ready         = true;
    load_request  = pending_image_load_request_;
  }
  if (!ready) {
    return;
  }
  EditorRenderEvent event;
  event.kind               = EditorRenderEventKind::FirstFrameReady;
  event.operation_id       = pending_operation_id_;
  event.state              = EditorSessionState::Interactive;
  event.identity           = identity;
  event.message            = "First frame ready";
  EmitEvent(std::move(event));

  {
    std::scoped_lock lock(mutex_);
    if (quality_base_routed_) {
      return;
    }
  }
  EditorRenderCommand qb_command;
  qb_command.operation_id = pending_operation_id_;
  qb_command.reason       = EditorRenderReason::SettledAdjustment;
  qb_command.adjustment   = pending_initial_adjustment_;
  auto intent             = MakeRenderIntent(qb_command, identity, load_request);
  if (intent && deps_.render && PresentationTargetReady()) {
    intent->quality         = EditorRenderQuality::Quality;
    intent->frame_role      = FrameRole::QualityBase;
    intent->priority        = EditorRenderPriority::Normal;
    intent->replacement_key = DefaultReplacementKey(EditorRenderQuality::Quality);
    FillRenderIntentDefaults(*intent);
    deps_.render->SetActiveImageLoadRequest(load_request.value);
    const auto qb_routed = deps_.render->Submit(*intent);
    if (qb_routed.kind == EditorRenderResultKind::RequestAccepted) {
      std::scoped_lock lock(mutex_);
      quality_base_routed_     = true;
      quality_base_ready_      = false;
      quality_base_request_id_ = qb_routed.request_id;
    }
  }
}

auto EditorSessionRenderController::MatchesActiveFirstFrame(
    const EditorRenderResult& render_result, const EditorSessionIdentity& identity,
    ImageLoadRequestId image_load_request) const -> bool {
  std::scoped_lock lock(mutex_);
  if (first_frame_request_id_ == 0 || render_result.request_id != first_frame_request_id_ ||
      identity.element_id != pending_session_identity_.element_id ||
      identity.image_id != pending_session_identity_.image_id ||
      image_load_request != pending_image_load_request_) {
    return false;
  }
  const auto& intent = render_result.intent;
  return intent.element_id == pending_session_identity_.element_id &&
         intent.image_id == pending_session_identity_.image_id &&
         intent.image_load_request_id == pending_image_load_request_;
}

void EditorSessionRenderController::RoutePendingInitialRender(
    const EditorRenderCommand& command, const EditorSessionIdentity& identity,
    ImageLoadRequestId image_load_request) {
  if (!PresentationTargetReady()) {
    return;
  }
  if (identity.element_id == 0 || identity.image_id == 0 || !image_load_request.valid()) {
    return;
  }
  if (RouteInitialRender(command, identity, image_load_request) == 0) {
    EditorRenderEvent event;
    event.kind         = EditorRenderEventKind::RenderFailed;
    event.operation_id = command.operation_id;
    event.state        = EditorSessionState::Loading;
    event.identity     = identity;
    event.message      = "First frame could not be scheduled";
    EmitEvent(std::move(event));
  }
}

auto EditorSessionRenderController::CoordinatorBusy() const -> bool {
  if (!deps_.render) {
    return false;
  }
  const auto diag = deps_.render->diagnostics();
  return diag.has_inflight || diag.pending_count > 0;
}

void EditorSessionRenderController::EmitEvent(EditorRenderEvent event) {
  if (deps_.on_event) {
    deps_.on_event(event);
  }
}

}  // namespace alcedo
