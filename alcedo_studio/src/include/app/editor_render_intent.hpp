//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_session_request_ids.hpp"
#include "edit/frame_presentation_types.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Why the editor requested a pipeline render. Maps to coordinator policy
/// (priority, quality ladder / fixed slots), not to QML control identity.
enum class EditorRenderReason : std::uint8_t {
  InitialFrame = 0,
  InteractiveAdjustment,
  SettledAdjustment,
  ZoomPan,
  Resize,
  DetailRefresh,
  UndoRedo,
  ImageSwitch,
  Retry,
  // Geometry (crop rect / rotation) changes the rendered content, not just the
  // view transform. Distinct from ZoomPan so the coordinator can decide a new
  // InteractivePrimary render instead of reusing the current full frame.
  CropRotate,
  // A newly selected scope needs a fresh final-display frame, using the
  // current viewport region just like the deprecated QWidget scope switch.
  ScopeRefresh,
  GraphTopologyChanged,
  SettledMaskEdit,
  // Same-image Version checkout / root / branch: one Quality rebuild of the
  // live DAG. Distinct from InitialFrame so the session stays Interactive.
  VersionDocumentChanged,
  // Typed Paste replaced transferable Grades, Masks, and DRT/Post on a new
  // root-relative Version. Quality rebuild of the live DAG.
  PastedPipelineDocument,
};

enum class EditorRenderQuality : std::uint8_t {
  Interactive = 0,
  Quality,
  Detail,
};

/// Higher value is scheduled sooner among pending work.
enum class EditorRenderPriority : std::uint8_t {
  Low = 0,
  Normal,
  High,
};

enum class EditorRenderResultKind : std::uint8_t {
  RequestAccepted = 0,
  RenderStarted,
  // The blocking pipeline call finished and the sink published a Ready frame.
  // Qt Quick composition is deliberately outside the request scheduler.
  FrameReady,
  Replaced,
  Cancelled,
  Failed,
  // The coordinator accepted the view change but did not schedule a pipeline
  // task: the existing full frame is reused (renderer re-samples via the
  // item-to-renderer synchronize() path).
  Reused,
};

/// Opaque presentation-sink identity. The coordinator never owns the sink; the
/// session service resolves it and stamps the identity on each intent.
using PresentationSinkId = std::uint64_t;

/// Cancellation token shared by intent producers and the coordinator. Setting
/// `cancelled` to true rejects or aborts the matching request generation.
struct EditorRenderCancellationToken {
  std::atomic<bool> cancelled{false};

  void Cancel() { cancelled.store(true, std::memory_order_release); }
  [[nodiscard]] auto IsCancelled() const -> bool {
    return cancelled.load(std::memory_order_acquire);
  }
};

/// Render intent accepted only by EditorRenderCoordinator.
///
/// Producers fill defaults before Submit. After Submit accepts the request the
/// coordinator does not mutate the stored intent.
struct EditorRenderIntent {
  sl_element_id_t                                element_id         = 0;
  image_id_t                                     image_id           = 0;
  std::uint64_t                                  operation_id       = 0;
  ImageLoadRequestId                             image_load_request_id{};
  EditorRenderReason                             reason = EditorRenderReason::InitialFrame;
  EditorRenderAdjustmentSnapshot                 adjustment{};
  std::optional<ViewportRenderRegion>            view_region;
  int                                            requested_width  = 0;
  int                                            requested_height = 0;
  FrameRole                                      frame_role       = FrameRole::InteractivePrimary;
  EditorRenderQuality                            quality  = EditorRenderQuality::Interactive;
  EditorRenderPriority                           priority = EditorRenderPriority::Normal;
  std::shared_ptr<EditorRenderCancellationToken> cancellation;
  PresentationSinkId                             presentation_sink_id  = 0;
  // Geometry-panel previews keep the full source frame visible while the
  // crop/rotation overlay is being edited. The adjustment state is still
  // carried by the intent, but the scheduler disables CROP_ROTATE for this
  // preview frame so its aspect matches the overlay's source-image UV space.
  bool                                           geometry_overlay_only = false;
  /// True when consume already wrote live document and CPU operators under the
  /// render lock. Configure must not apply `adjustment` again.
  bool                                           live_parameters_applied = false;
};

struct EditorRenderRequest {
  std::uint64_t      request_id = 0;
  EditorRenderIntent intent{};
};

struct EditorRenderResult {
  EditorRenderResultKind kind       = EditorRenderResultKind::RequestAccepted;
  std::uint64_t          request_id = 0;
  EditorRenderIntent     intent{};
  std::string            message;
};

[[nodiscard]] inline auto FrameRoleForQuality(EditorRenderQuality quality) -> FrameRole {
  switch (quality) {
    case EditorRenderQuality::Interactive:
      return FrameRole::InteractivePrimary;
    case EditorRenderQuality::Quality:
      return FrameRole::QualityBase;
    case EditorRenderQuality::Detail:
      return FrameRole::DetailPatch;
  }
  return FrameRole::InteractivePrimary;
}

[[nodiscard]] inline auto DefaultPriorityForReason(EditorRenderReason reason)
    -> EditorRenderPriority {
  switch (reason) {
    case EditorRenderReason::InitialFrame:
    case EditorRenderReason::ImageSwitch:
    case EditorRenderReason::Retry:
      return EditorRenderPriority::High;
    case EditorRenderReason::InteractiveAdjustment:
    case EditorRenderReason::ZoomPan:
    case EditorRenderReason::Resize:
      return EditorRenderPriority::Normal;
    case EditorRenderReason::SettledAdjustment:
    case EditorRenderReason::DetailRefresh:
    case EditorRenderReason::UndoRedo:
    case EditorRenderReason::CropRotate:
    case EditorRenderReason::ScopeRefresh:
    case EditorRenderReason::GraphTopologyChanged:
    case EditorRenderReason::SettledMaskEdit:
    case EditorRenderReason::VersionDocumentChanged:
    case EditorRenderReason::PastedPipelineDocument:
      return EditorRenderPriority::Normal;
  }
  return EditorRenderPriority::Normal;
}

[[nodiscard]] inline auto DefaultQualityForReason(EditorRenderReason reason)
    -> EditorRenderQuality {
  switch (reason) {
    case EditorRenderReason::DetailRefresh:
      return EditorRenderQuality::Detail;
    case EditorRenderReason::SettledAdjustment:
    case EditorRenderReason::UndoRedo:
    case EditorRenderReason::GraphTopologyChanged:
    case EditorRenderReason::SettledMaskEdit:
    case EditorRenderReason::VersionDocumentChanged:
    case EditorRenderReason::PastedPipelineDocument:
      return EditorRenderQuality::Quality;
    case EditorRenderReason::InitialFrame:
    case EditorRenderReason::InteractiveAdjustment:
    case EditorRenderReason::ZoomPan:
    case EditorRenderReason::Resize:
    case EditorRenderReason::ImageSwitch:
    case EditorRenderReason::Retry:
    case EditorRenderReason::CropRotate:
    case EditorRenderReason::ScopeRefresh:
      return EditorRenderQuality::Interactive;
  }
  return EditorRenderQuality::Interactive;
}

/// A pure view-transform change (zoom/pan/resize) reuses the current
/// full frame — the renderer re-samples it through synchronize(). The
/// coordinator drops these intents without scheduling a pipeline task. Only
/// content-changing or detail-refresh reasons produce a render.
[[nodiscard]] inline auto ReasonReusesCurrentFrame(EditorRenderReason reason) -> bool {
  return reason == EditorRenderReason::ZoomPan || reason == EditorRenderReason::Resize;
}

/// Whether frame configuration applies the full adjustment snapshot.
///
/// Pipeline operators are updated incrementally when a field changes
/// (SetOperator + SetGlobalParams). Replaying a full adjustment snapshot every
/// frame is only correct for content-bearing renders (open, edit, undo, crop
/// commit). View-dependent work (Detail ROI, scope ROI, pure zoom/pan/resize)
/// must only retarget Geometry render params (RESIZE ROI / user crop) so
/// Image Loading caches such as RAW_DECODE stay warm.
[[nodiscard]] inline auto ReasonAppliesAdjustmentSnapshot(EditorRenderReason reason) -> bool {
  switch (reason) {
    case EditorRenderReason::ZoomPan:
    case EditorRenderReason::Resize:
    case EditorRenderReason::DetailRefresh:
    case EditorRenderReason::ScopeRefresh:
      return false;
    case EditorRenderReason::InitialFrame:
    case EditorRenderReason::InteractiveAdjustment:
    case EditorRenderReason::SettledAdjustment:
    case EditorRenderReason::UndoRedo:
    case EditorRenderReason::ImageSwitch:
    case EditorRenderReason::Retry:
    case EditorRenderReason::CropRotate:
    case EditorRenderReason::GraphTopologyChanged:
    case EditorRenderReason::SettledMaskEdit:
    case EditorRenderReason::VersionDocumentChanged:
    case EditorRenderReason::PastedPipelineDocument:
      return true;
  }
  return true;
}

/// Scope reads image content, so view-only re-sampling and view-dependent ROI
/// refreshes retain the last content frame instead of replacing the scope input.
[[nodiscard]] inline auto ScopeUpdateAllowedForReason(EditorRenderReason reason) -> bool {
  return reason != EditorRenderReason::ZoomPan && reason != EditorRenderReason::Resize &&
         reason != EditorRenderReason::DetailRefresh;
}

/// Fill frame-role defaults derived from quality before Submit stores the
/// intent. Producers should set reason/quality/priority (or accept reason defaults).
/// Coalesce uses fixed quality slots — no string replacement key.
inline void FillRenderIntentDefaults(EditorRenderIntent& intent) {
  // Align InteractivePrimary with quality ladder role.
  if (intent.frame_role == FrameRole::InteractivePrimary) {
    intent.frame_role = FrameRoleForQuality(intent.quality);
  }
}

}  // namespace alcedo
