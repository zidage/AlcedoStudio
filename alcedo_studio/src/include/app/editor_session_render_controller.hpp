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
  FirstFrameReady = 0,
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
};

/// Owns the render and first-frame state for the focused editor session.
/// Manages the presentation sink/dimensions, first-frame and quality-base
/// request IDs, the single FrameReady check, pending initial render
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
  /**
   * @brief Source of the Mask coverage probe that every new render intent carries (Mask ranges
   *        D9). Called when an intent is built, on the thread that builds it; it must be
   *        thread-safe. Empty source: no probe.
   */
  void SetMaskCoverageProbeSource(std::function<std::optional<MaskCoverageProbe>()> source);
  /// The viewport was minimized/hidden (true) or shown again (false). On resume,
  /// frames the sink refused while suspended are routed again for the current
  /// state: the first frame of a loading image, else a QualityBase refresh, and
  /// the newest refused detail patch after it.
  void SetPresentationSuspended(bool suspended, const EditorSessionIdentity& identity,
                                ImageLoadRequestId image_load_request, EditorSessionState state);
  [[nodiscard]] auto presentation_suspended() const -> bool;
  /// An export released the editor GPU resources (true) or ended (false). The render port refuses
  /// frames like a suspended viewport while it runs; after it ends, the refused frames are routed
  /// again as on a viewport resume, unless the viewport is still suspended.
  void SetSuspendedForExport(bool suspended, const EditorSessionIdentity& identity,
                             ImageLoadRequestId image_load_request, EditorSessionState state);
  [[nodiscard]] auto suspended_for_export() const -> bool;

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
  /// identity and image-load request for filtering and first-frame state updates.
  void NotifyRenderResult(const EditorRenderResult&    render_result,
                          const EditorSessionIdentity& identity, ImageLoadRequestId image_load_request,
                          EditorSessionState state);

  /// True when the presentation target (sink + dimensions) is ready.
  [[nodiscard]] auto PresentationTargetReady() const -> bool;

  /// The current first-frame request id. Zero when no first frame is pending.
  [[nodiscard]] auto first_frame_request_id() const -> std::uint64_t;

  /// Wall time from open/switch first-frame route until the sink publishes the
  /// first Ready frame. Negative while no frame is ready.
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
  /// the first frame is ready.
  void               MarkImageAcquired();

  /// Cancel pending/in-flight work for one image-load request without joining.
  void               CancelSession(ImageLoadRequestId image_load_request);
  /// Cancel without blocking and report actual scheduler idleness. The callback
  /// can originate on a render worker; callers must marshal state changes to
  /// their owner thread.
  void               CancelSession(ImageLoadRequestId image_load_request,
                                   std::function<void(ImageLoadRequestId)> on_idle);

 private:
  /// Build a fully-stamped render intent from the command and identity.
  /// Returns nullopt when the identity does not represent an active image.
  [[nodiscard]] auto MakeRenderIntent(const EditorRenderCommand&   command,
                                      const EditorSessionIdentity& identity,
                                      ImageLoadRequestId           image_load_request) const
      -> std::optional<EditorRenderIntent>;
  /// Emit a FirstFrameReady event when image acquisition and rendering finish.
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
  /// Emit BusyChanged when the coordinator busy state moved.
  void                NotifyBusyIfChanged(std::uint64_t                operation_id,
                                          const EditorSessionIdentity& identity,
                                          EditorSessionState           state);
  /// Record a frame the sink refused while the viewport was suspended.
  void                NoteSuspendedPresentation(const EditorRenderResult&    render_result,
                                                const EditorSessionIdentity& identity,
                                                ImageLoadRequestId           image_load_request,
                                                EditorSessionState           state);
  /// Route the frames recorded by NoteSuspendedPresentation for the current state.
  void                RouteSuspendedRefresh(const EditorSessionIdentity& identity,
                                            ImageLoadRequestId           image_load_request,
                                            EditorSessionState           state);

  struct PendingDetailRender {
    EditorRenderCommand   command{};
    EditorSessionIdentity identity{};
    ImageLoadRequestId    image_load_request{};
  };

  struct Dependencies deps_;
  mutable std::recursive_mutex      mutex_;
  PresentationSinkId                presentation_sink_id_    = 0;
  int                               presentation_width_      = 0;
  int                               presentation_height_     = 0;
  std::uint64_t                     first_frame_request_id_  = 0;
  std::uint64_t                     quality_base_request_id_ = 0;
  bool                              image_acquired_          = false;
  bool                              first_frame_ready_       = false;
  bool                              quality_base_routed_     = false;
  bool                              quality_base_ready_      = false;
  std::optional<PendingDetailRender> pending_detail_render_{};
  std::optional<EditorRenderReason> pending_initial_reason_;
  std::uint64_t                     pending_operation_id_ = 0;
  /// Identity at the time the first frame was routed. Used to correlate the
  /// first-frame Ready event with the correct identity snapshot.
  EditorSessionIdentity                                pending_session_identity_{};
  ImageLoadRequestId                                   pending_image_load_request_{};
  bool                                                 last_notified_render_busy_ = false;
  std::optional<std::chrono::steady_clock::time_point> first_frame_route_time_{};
  double                                               first_frame_time_ms_ = -1.0;
  std::atomic<bool>                                    geometry_overlay_active_{false};
  mutable std::mutex                                   probe_source_mutex_;
  std::function<std::optional<MaskCoverageProbe>()>    probe_source_;
  /// Set one suspension reason and route the refused frames when no reason is left.
  void SetSuspension(bool EditorSessionRenderController::* reason, bool suspended,
                     const EditorSessionIdentity& identity, ImageLoadRequestId image_load_request,
                     EditorSessionState state);
  /// Last suspension reported by the viewport owner.
  bool                                                 presentation_suspended_ = false;
  /// An export holds the GPU resources the editor released.
  bool                                                 suspended_for_export_ = false;
  /// One refresh per resume: a refusal that lands after the resume routes the
  /// refresh itself, and a second refusal without a new resume does not loop.
  bool                                                 suspended_refresh_routed_ = false;
  /// Frames refused while suspended, for @ref suspended_image_load_request_.
  ImageLoadRequestId                                   suspended_image_load_request_{};
  bool                                                 suspended_first_frame_ = false;
  bool                                                 suspended_full_frame_  = false;
  std::optional<PendingDetailRender>                   suspended_detail_render_{};
};

}  // namespace alcedo
