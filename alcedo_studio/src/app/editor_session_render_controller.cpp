//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_render_controller.hpp"

#include <algorithm>
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
      pending_command.policy       = pending_initial_policy_;
      pending_identity             = pending_session_identity_;
      has_pending                  = true;
    }
  }
  if (has_pending) {
    RoutePendingInitialRender(pending_command, pending_identity);
  }
}

void EditorSessionRenderController::SetPresentationSize(int width, int height) {
  EditorRenderCommand   pending_command;
  EditorSessionIdentity pending_identity;
  bool                  has_pending = false;
  {
    std::scoped_lock lock(mutex_);
    presentation_width_  = std::max(0, width);
    presentation_height_ = std::max(0, height);
    if (pending_initial_reason_.has_value()) {
      pending_command.reason       = *pending_initial_reason_;
      pending_command.operation_id = pending_operation_id_;
      pending_command.adjustment   = pending_initial_adjustment_;
      pending_command.policy       = pending_initial_policy_;
      pending_identity             = pending_session_identity_;
      has_pending                  = true;
    }
  }
  if (has_pending) {
    RoutePendingInitialRender(pending_command, pending_identity);
  }
}

auto EditorSessionRenderController::MakeRenderIntent(const EditorRenderCommand&   command,
                                                     const EditorSessionIdentity& identity) const
    -> std::optional<EditorRenderIntent> {
  if (identity.element_id == 0 || identity.image_id == 0) {
    return std::nullopt;
  }
  EditorRenderIntent intent;
  intent.element_id            = identity.element_id;
  intent.image_id              = identity.image_id;
  intent.operation_id          = command.operation_id;
  intent.session_generation    = identity.session_generation;
  intent.render_generation     = identity.render_generation;
  intent.view_generation       = identity.view_generation;
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
                                                       const EditorSessionIdentity& identity)
    -> std::uint64_t {
  if (!PresentationTargetReady()) {
    std::scoped_lock lock(mutex_);
    pending_initial_reason_     = command.reason;
    pending_operation_id_       = command.operation_id;
    pending_initial_adjustment_ = command.adjustment;
    pending_initial_policy_     = command.policy;
    pending_session_identity_   = identity;
    return 0;
  }
  if (!deps_.render) {
    return 0;
  }
  auto intent = MakeRenderIntent(command, identity);
  if (!intent) {
    return 0;
  }
  deps_.render->SetActiveGenerations(identity.session_generation, identity.render_generation,
                                     identity.view_generation, command.policy);
  const EditorRenderResult routed = deps_.render->Submit(*intent);
  if (routed.kind == EditorRenderResultKind::RequestAccepted) {
    if (command.reason == EditorRenderReason::InitialFrame ||
        command.reason == EditorRenderReason::ImageSwitch ||
        command.reason == EditorRenderReason::Retry) {
      std::scoped_lock lock(mutex_);
      first_frame_request_id_     = routed.request_id;
      first_frame_completed_      = false;
      first_frame_submitted_      = false;
      first_frame_presented_      = false;
      quality_base_routed_        = false;
      quality_base_request_id_    = 0;
      first_frame_route_time_     = std::chrono::steady_clock::now();
      first_frame_time_ms_        = -1.0;
      pending_session_identity_   = identity;
      pending_initial_adjustment_ = command.adjustment;
    }
    std::scoped_lock lock(mutex_);
    pending_initial_reason_.reset();
    // Keep pending_initial_adjustment_ so TryEnterInteractiveFromFirstFrame
    // can use it for the QualityBase follow-up. Only reset the reason.
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
                                                    EditorSessionState state) -> EditorRenderEvent {
  EditorRenderEvent event;
  const bool        detail_from_visible_loading_frame =
      command.reason == EditorRenderReason::DetailRefresh && state == EditorSessionState::Loading &&
      image_acquired_ && first_frame_completed_ && first_frame_submitted_;
  if (state != EditorSessionState::Interactive && !detail_from_visible_loading_frame) {
    event.kind    = EditorRenderEventKind::RenderRejected;
    event.message = "View change requires an interactive session";
    return event;
  }
  if (identity.element_id == 0 || identity.image_id == 0) {
    event.kind    = EditorRenderEventKind::RenderRejected;
    event.message = "View change requires an open image";
    return event;
  }

  if (deps_.render) {
    deps_.render->SetActiveGenerations(identity.session_generation, identity.render_generation,
                                       identity.view_generation);
  }

  auto intent_opt = MakeRenderIntent(command, identity);
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

void EditorSessionRenderController::NotifyRenderResult(const EditorRenderResult&    render_result,
                                                       const EditorSessionIdentity& identity,
                                                       EditorSessionState           state) {
  // Ignore results for a different image/session.
  if (render_result.intent.session_generation != 0 &&
      render_result.intent.session_generation != identity.session_generation) {
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
    if (state == EditorSessionState::Loading && MatchesActiveFirstFrame(render_result, identity)) {
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

  // First-frame gate: require matching generations and ordered complete→submit→present.
  if (MatchesActiveFirstFrame(render_result, identity)) {
    if (render_result.kind == EditorRenderResultKind::RenderCompleted) {
      std::scoped_lock lock(mutex_);
      first_frame_completed_ = true;
    } else if (render_result.kind == EditorRenderResultKind::FrameSubmitted) {
      std::scoped_lock lock(mutex_);
      if (!first_frame_completed_) {
        return;
      }
      first_frame_submitted_ = true;
    } else if (render_result.kind == EditorRenderResultKind::FramePresented) {
      bool                  should_try_interactive = false;
      EditorSessionIdentity first_frame_identity;
      {
        std::scoped_lock lock(mutex_);
        if (!first_frame_completed_ || !first_frame_submitted_ || first_frame_presented_) {
          return;
        }
        first_frame_presented_ = true;
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

  // Non-first-frame results: still require generation match for presentation side-effects.
  if (render_result.kind == EditorRenderResultKind::FramePresented) {
    if (render_result.intent.render_generation != identity.render_generation ||
        render_result.intent.view_generation != identity.view_generation) {
      return;
    }
  }

  // Announce render-busy transitions.
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
  first_frame_completed_   = false;
  first_frame_submitted_   = false;
  first_frame_presented_   = false;
  quality_base_routed_     = false;
  pending_initial_reason_.reset();
  pending_operation_id_       = 0;
  pending_initial_adjustment_ = {};
  pending_session_identity_   = {};
  first_frame_route_time_.reset();
  first_frame_time_ms_ = -1.0;
}

void EditorSessionRenderController::MarkImageAcquired() {
  std::scoped_lock lock(mutex_);
  image_acquired_ = true;
}

void EditorSessionRenderController::CancelSessionAndWait(std::uint64_t session_generation) {
  if (deps_.render && session_generation != 0) {
    deps_.render->CancelSessionAndWait(session_generation);
  }
}

void EditorSessionRenderController::TryEnterInteractiveFromFirstFrame(
    const EditorSessionIdentity& identity) {
  bool ready = false;
  {
    std::scoped_lock lock(mutex_);
    if (!image_acquired_ || !first_frame_completed_ || !first_frame_submitted_ ||
        !first_frame_presented_) {
      return;
    }
    ready = true;
  }
  if (!ready) {
    return;
  }
  // Emit the FirstFramePresented event. The facade's handler applies the
  // Interactive lifecycle transition and schedules the QualityBase follow-up.
  EditorRenderEvent event;
  event.kind               = EditorRenderEventKind::FirstFramePresented;
  event.operation_id       = pending_operation_id_;
  event.state              = EditorSessionState::Interactive;
  event.identity           = identity;
  event.presented_identity = identity;
  event.message            = "First frame presented";
  EmitEvent(std::move(event));

  // Queue the QualityBase follow-up using the captured adjustment.
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
  auto intent             = MakeRenderIntent(qb_command, identity);
  if (intent && deps_.render && PresentationTargetReady()) {
    intent->quality         = EditorRenderQuality::Quality;
    intent->frame_role      = FrameRole::QualityBase;
    intent->priority        = EditorRenderPriority::Normal;
    intent->replacement_key = DefaultReplacementKey(EditorRenderQuality::Quality);
    FillRenderIntentDefaults(*intent);
    deps_.render->SetActiveGenerations(identity.session_generation, identity.render_generation,
                                       identity.view_generation);
    const auto qb_routed = deps_.render->Submit(*intent);
    if (qb_routed.kind == EditorRenderResultKind::RequestAccepted) {
      std::scoped_lock lock(mutex_);
      quality_base_routed_     = true;
      quality_base_request_id_ = qb_routed.request_id;
    }
  }
}

auto EditorSessionRenderController::MatchesActiveFirstFrame(
    const EditorRenderResult& render_result, const EditorSessionIdentity& identity) const -> bool {
  std::scoped_lock lock(mutex_);
  if (first_frame_request_id_ == 0 || render_result.request_id != first_frame_request_id_ ||
      identity.element_id != pending_session_identity_.element_id ||
      identity.image_id != pending_session_identity_.image_id ||
      identity.session_generation != pending_session_identity_.session_generation ||
      identity.render_generation != pending_session_identity_.render_generation ||
      identity.view_generation != pending_session_identity_.view_generation) {
    return false;
  }
  const auto& intent = render_result.intent;
  return intent.element_id == pending_session_identity_.element_id &&
         intent.image_id == pending_session_identity_.image_id &&
         intent.session_generation == pending_session_identity_.session_generation &&
         intent.render_generation == pending_session_identity_.render_generation &&
         intent.view_generation == pending_session_identity_.view_generation;
}

void EditorSessionRenderController::RoutePendingInitialRender(
    const EditorRenderCommand& command, const EditorSessionIdentity& identity) {
  if (!PresentationTargetReady()) {
    return;
  }
  if (identity.element_id == 0 || identity.image_id == 0) {
    return;
  }
  if (RouteInitialRender(command, identity) == 0) {
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
