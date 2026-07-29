//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

#include "app/editor_render_intent.hpp"
#include "app/editor_session_ports.hpp"
#include "app/editor_session_request_ids.hpp"
#include "app/editor_session_types.hpp"
#include "type/type.hpp"

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
  EditorRenderEventKind kind         = EditorRenderEventKind::RenderRouted;
  std::uint64_t         operation_id = 0;
  std::uint64_t         request_id   = 0;
  EditorSessionState    state        = EditorSessionState::NoImage;
  EditorSessionIdentity identity{};
  EditorRenderReason    reason = EditorRenderReason::ZoomPan;
  std::string           message;
  /// When FirstFramePresented, the identity at the time of presentation.
  /// The facade uses this to transition lifecycle to Interactive.
  EditorSessionIdentity presented_identity{};
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
    EventCallback                            on_event;
  };

  explicit EditorSessionRenderController(Dependencies dependencies);
  ~EditorSessionRenderController();

  EditorSessionRenderController(const EditorSessionRenderController&)            = delete;
  EditorSessionRenderController& operator=(const EditorSessionRenderController&) = delete;

  void                           SetPresentationSinkId(PresentationSinkId sink_id);
  void                           SetPresentationSize(int width, int height);
  /// Select source-frame rendering while the geometry panel owns the overlay.
  void                           SetGeometryOverlayActive(bool active);

  /// Route the initial render for a new image. Accepts the session identity,
  /// image-load request, and state as immutable inputs from the facade.
  auto RouteInitialRender(const EditorRenderCommand& command, const EditorSessionIdentity& identity,
                          ImageLoadRequestId image_load_request) -> std::uint64_t;

  /// Route a view change. The facade provides identity, image-load request, and
  /// state after advancing view stamps.
  auto RouteViewChange(const EditorRenderCommand& command, const EditorSessionIdentity& identity,
                       ImageLoadRequestId image_load_request, EditorSessionState state)
      -> EditorRenderEvent;

  /// Feed a render result from the coordinator. The facade provides the session
  /// identity and image-load request for filtering and first-frame gate updates.
  void NotifyRenderResult(const EditorRenderResult&    render_result,
                          const EditorSessionIdentity& identity, ImageLoadRequestId image_load_request,
                          EditorSessionState state);

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
  void               ResetForNewImage();

  /// Mark the image as acquired after guards succeed. Stays in Loading until
  /// the first frame is presented.
  void               MarkImageAcquired();

  /// Cancel the active render session for one image-load request.
  void               CancelSessionAndWait(ImageLoadRequestId image_load_request);

  /// Advance the content stamp used for coordinator supersession after edits.
  void               AdvanceContentGeneration();
  /// Advance the view stamp used to obsolete DetailPatch work.
  void               AdvanceViewGeneration();
  [[nodiscard]] auto content_generation() const -> std::uint64_t;
  [[nodiscard]] auto view_generation() const -> std::uint64_t;

 private:
  /// Build a fully-stamped render intent from the command and identity.
  /// Returns nullopt when the identity does not represent an active image.
  [[nodiscard]] auto MakeRenderIntent(const EditorRenderCommand&   command,
                                      const EditorSessionIdentity& identity,
                                      ImageLoadRequestId           image_load_request) const
      -> std::optional<EditorRenderIntent>;
  /// Emit a FirstFramePresented event when all first-frame conditions are met.
  /// The facade applies the Interactive lifecycle transition in its handler.
  void                TryEnterInteractiveFromFirstFrame(const EditorSessionIdentity& identity);
  /// Check if a render result matches the active first-frame request and the
  /// provided identity.
  [[nodiscard]] auto  MatchesActiveFirstFrame(const EditorRenderResult&    render_result,
                                              const EditorSessionIdentity& identity,
                                              ImageLoadRequestId image_load_request) const -> bool;
  void                RoutePendingInitialRender(const EditorRenderCommand&   command,
                                                const EditorSessionIdentity& identity,
                                                ImageLoadRequestId           image_load_request);
  /// Aggregate coordinator in-flight/pending state.
  [[nodiscard]] auto  CoordinatorBusy() const -> bool;
  /// Emit a render event to the facade.
  void                EmitEvent(EditorRenderEvent event);

  struct Dependencies deps_;
  mutable std::recursive_mutex      mutex_;
  PresentationSinkId                presentation_sink_id_    = 0;
  int                               presentation_width_      = 0;
  int                               presentation_height_     = 0;
  std::uint64_t                     first_frame_request_id_  = 0;
  std::uint64_t                     quality_base_request_id_ = 0;
  bool                              image_acquired_          = false;
  bool                              first_frame_completed_   = false;
  bool                              first_frame_submitted_   = false;
  bool                              first_frame_presented_   = false;
  bool                              quality_base_routed_     = false;
  std::optional<EditorRenderReason> pending_initial_reason_;
  std::uint64_t                     pending_operation_id_ = 0;
  EditorRenderAdjustmentSnapshot    pending_initial_adjustment_;
  EditorRenderSupersessionPolicy    pending_initial_policy_ =
      EditorRenderSupersessionPolicy::CancelObsolete;
  /// Identity at the time the first frame was routed. Used to correlate the
  /// first-frame complete→submit→present gate and to emit the
  /// FirstFramePresented event with the correct identity snapshot.
  EditorSessionIdentity                                pending_session_identity_{};
  ImageLoadRequestId                                   pending_image_load_request_{};
  std::uint64_t                                        content_generation_ = 0;
  std::uint64_t                                        view_generation_    = 1;
  bool                                                 last_notified_render_busy_ = false;
  std::optional<std::chrono::steady_clock::time_point> first_frame_route_time_{};
  double                                               first_frame_time_ms_ = -1.0;
  std::atomic<bool>                                    geometry_overlay_active_{false};
};

}  // namespace alcedo
