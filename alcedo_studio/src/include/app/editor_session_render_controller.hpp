//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

#include <expected>

namespace alcedo {

/// Typed render events returned by the render controller to the facade. The
/// facade maps these to EditorSessionResult values and publishes them.
enum class EditorRenderEventKind : std::uint8_t {
  FirstFramePresented = 0,
  RenderFailed,
  RenderRouted,
  RenderReused,
  RenderRejected,
  BusyChanged,
};

struct EditorRenderEvent {
  EditorRenderEventKind         kind       = EditorRenderEventKind::RenderRouted;
  std::uint64_t                 request_id = 0;
  EditorSessionState            state      = EditorSessionState::NoImage;
  EditorSessionIdentity         identity{};
  EditorRenderReason            reason     = EditorRenderReason::ZoomPan;
  std::string                   message;
  /// When FirstFramePresented, the identity at the time of presentation.
  /// The facade uses this to transition lifecycle to Interactive.
  EditorSessionIdentity         presented_identity{};
};

/// Owns the render and first-frame state for the focused editor session.
/// Manages the presentation sink/dimensions, first-frame and quality-base
/// request IDs, the complete→submit→present gate, pending initial render
/// routing, render-busy notification, and first-frame timing. Receives
/// immutable EditorRenderCommand values; never reads another component's
/// mutable state.
class EditorSessionRenderController final {
 public:
  using EventCallback = std::function<void(const EditorRenderEvent&)>;

  struct Dependencies {
    std::shared_ptr<IEditorRenderSubmitPort> render;
    EditorSessionLifecycle&                  lifecycle;
    EventCallback                            on_event;
  };

  explicit EditorSessionRenderController(Dependencies dependencies);
  ~EditorSessionRenderController();

  EditorSessionRenderController(const EditorSessionRenderController&)            = delete;
  EditorSessionRenderController& operator=(const EditorSessionRenderController&) = delete;

  void SetPresentationSinkId(PresentationSinkId sink_id);
  void SetPresentationSize(int width, int height);

  /// Route the initial render for a new image. Takes the adjustment snapshot
  /// as an immutable input so this controller does not read edit state.
  auto RouteInitialRender(const EditorRenderCommand& command) -> std::uint64_t;

  /// Route a view change. Takes the adjustment snapshot as an immutable input.
  auto RouteViewChange(const EditorRenderCommand& command) -> EditorRenderEvent;

  /// Feed a render result from the coordinator. Updates the first-frame gate,
  /// transitions to Interactive when the first frame is presented, and
  /// announces render-busy transitions.
  void NotifyRenderResult(const EditorRenderResult& render_result);

  /// True when the presentation target (sink + dimensions) is ready.
  [[nodiscard]] auto PresentationTargetReady() const -> bool;

  /// The current first-frame request id. Zero when no first frame is pending.
  [[nodiscard]] auto first_frame_request_id() const -> std::uint64_t;

  /// Wall time from open/switch first-frame route to first presentation, in
  /// milliseconds. Negative when no first frame has been presented.
  [[nodiscard]] auto first_frame_time_ms() const -> double;

  /// Aggregate coordinator busy state.
  [[nodiscard]] auto render_busy() const -> bool;

  /// Coordinator diagnostics pass-through.
  [[nodiscard]] auto render_diagnostics() const -> EditorRenderCoordinatorDiagnostics;

  /// The presentation sink id (for diagnostics).
  [[nodiscard]] auto presentation_sink_id() const -> PresentationSinkId;

  /// Reset the first-frame and render state for a new image. Called by the
  /// facade when opening or switching images.
  void ResetForNewImage();

  /// Mark the image as acquired after guards succeed. Stays in Loading until
  /// the first frame is presented.
  void MarkImageAcquired();

  /// Cancel the active render session. Called by the facade during seal/close.
  void CancelSessionAndWait(std::uint64_t session_generation);

 private:
  /// Build a fully-stamped render intent from the command. Returns nullopt
  /// when no image is active.
  [[nodiscard]] auto MakeRenderIntent(const EditorRenderCommand& command) const
      -> std::optional<EditorRenderIntent>;
  /// Attempt to enter Interactive when all first-frame conditions are met.
  void TryEnterInteractiveFromFirstFrame();
  /// Check if a render result matches the active first-frame request.
  [[nodiscard]] auto MatchesActiveFirstFrame(const EditorRenderResult& render_result) const
      -> bool;
  /// Route a pending initial render if the presentation target becomes ready.
  void RoutePendingInitialRender(const EditorRenderCommand& command);
  /// Aggregate coordinator in-flight/pending state.
  [[nodiscard]] auto CoordinatorBusy() const -> bool;
  /// Emit a render event to the facade.
  void EmitEvent(EditorRenderEvent event);

  struct Dependencies                deps_;
  mutable std::recursive_mutex      mutex_;
  PresentationSinkId                presentation_sink_id_     = 0;
  int                               presentation_width_       = 0;
  int                               presentation_height_      = 0;
  std::uint64_t                     first_frame_request_id_  = 0;
  std::uint64_t                     quality_base_request_id_ = 0;
  bool                              image_acquired_          = false;
  bool                              first_frame_completed_   = false;
  bool                              first_frame_submitted_   = false;
  bool                              first_frame_presented_   = false;
  bool                              quality_base_routed_     = false;
  std::optional<EditorRenderReason> pending_initial_reason_;
  EditorRenderAdjustmentSnapshot    pending_initial_adjustment_;
  EditorRenderSupersessionPolicy    pending_initial_policy_ =
      EditorRenderSupersessionPolicy::CancelObsolete;
  bool last_notified_render_busy_ = false;
  std::optional<std::chrono::steady_clock::time_point> first_frame_route_time_{};
  double                                               first_frame_time_ms_ = -1.0;
};

}  // namespace alcedo