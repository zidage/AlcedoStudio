//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "edit/frame_presentation_types.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Why the editor requested a pipeline render. Maps to coordinator policy
/// (priority, quality ladder, replacement key), not to QML control identity.
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
  RenderCompleted,
  FrameSubmitted,
  FramePresented,
  Replaced,
  Cancelled,
  Failed,
  // The coordinator accepted the view change but did not schedule a pipeline
  // task: the existing full frame is reused (renderer re-samples via the
  // item-to-renderer synchronize() path). Phase 5D reuse-vs-render decision.
  Reused,
};

/// Opaque presentation-sink identity. The coordinator never owns the sink; the
/// session service resolves it and stamps the identity on each intent.
using PresentationSinkId = std::uint64_t;

/// Cancellation token shared by intent producers and the coordinator. Setting
/// `cancelled` to true rejects or aborts the matching request generation.
struct EditorRenderCancellationToken {
  std::atomic<bool> cancelled{false};

  void              Cancel() {
    if (cancelled.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    std::function<void()> callback;
    {
      std::scoped_lock lock(callback_mutex);
      if (!callback_delivered && on_cancel) {
        callback_delivered = true;
        callback           = on_cancel;
      }
    }
    if (callback) {
      callback();
    }
  }
  [[nodiscard]] auto IsCancelled() const -> bool {
    return cancelled.load(std::memory_order_acquire);
  }

  void SetCancelCallback(std::function<void()> callback) {
    std::function<void()> callback_to_run;
    {
      std::scoped_lock lock(callback_mutex);
      on_cancel = std::move(callback);
      if (cancelled.load(std::memory_order_acquire) && !callback_delivered && on_cancel) {
        callback_delivered = true;
        callback_to_run    = on_cancel;
      }
    }
    if (callback_to_run) {
      callback_to_run();
    }
  }

 private:
  mutable std::mutex    callback_mutex;
  std::function<void()> on_cancel;
  bool                  callback_delivered = false;
};

/// Render intent accepted only by EditorRenderCoordinator.
///
/// Producers fill defaults before Submit. After Submit accepts the request the
/// coordinator does not mutate the stored intent (Phase 5A-Fix immutability).
struct EditorRenderIntent {
  sl_element_id_t                                element_id         = 0;
  image_id_t                                     image_id           = 0;
  std::uint64_t                                  session_generation = 0;
  std::uint64_t                                  render_generation  = 0;
  std::uint64_t                                  view_generation    = 0;
  EditorRenderReason                             reason = EditorRenderReason::InitialFrame;
  EditorRenderAdjustmentSnapshot                 adjustment{};
  std::optional<ViewportRenderRegion>            view_region;
  int                                            requested_width  = 0;
  int                                            requested_height = 0;
  FrameRole                                      frame_role       = FrameRole::InteractivePrimary;
  EditorRenderQuality                            quality  = EditorRenderQuality::Interactive;
  EditorRenderPriority                           priority = EditorRenderPriority::Normal;
  /// Same key replaces prior pending work (e.g. "interactive", "quality", "detail").
  std::string                                    replacement_key;
  std::shared_ptr<EditorRenderCancellationToken> cancellation;
  PresentationSinkId                             presentation_sink_id  = 0;
  // Geometry-panel previews keep the full source frame visible while the
  // crop/rotation overlay is being edited. The adjustment state is still
  // carried by the intent, but the scheduler disables CROP_ROTATE for this
  // preview frame so its aspect matches the overlay's source-image UV space.
  bool                                           geometry_overlay_only = false;
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

[[nodiscard]] inline auto DefaultReplacementKey(EditorRenderQuality quality) -> const char* {
  switch (quality) {
    case EditorRenderQuality::Interactive:
      return "interactive";
    case EditorRenderQuality::Quality:
      return "quality";
    case EditorRenderQuality::Detail:
      return "detail";
  }
  return "interactive";
}

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

/// Phase 5D: a pure view-transform change (zoom/pan/resize) reuses the current
/// full frame — the renderer re-samples it through synchronize(). The
/// coordinator drops these intents without scheduling a pipeline task. Only
/// content-changing or detail-refresh reasons produce a render.
[[nodiscard]] inline auto ReasonReusesCurrentFrame(EditorRenderReason reason) -> bool {
  return reason == EditorRenderReason::ZoomPan || reason == EditorRenderReason::Resize;
}

/// Scope reads image content, so view-only re-sampling and view-dependent ROI
/// refreshes retain the last content frame instead of replacing the scope input.
[[nodiscard]] inline auto ScopeUpdateAllowedForReason(EditorRenderReason reason) -> bool {
  return reason != EditorRenderReason::ZoomPan && reason != EditorRenderReason::Resize &&
         reason != EditorRenderReason::DetailRefresh;
}

/// Fill role/replacement defaults derived from quality before Submit stores the
/// intent. Producers should set reason/quality/priority (or accept reason defaults).
inline void FillRenderIntentDefaults(EditorRenderIntent& intent) {
  // Align InteractivePrimary with non-interactive quality (legacy coordinator policy).
  if (intent.frame_role == FrameRole::InteractivePrimary &&
      intent.quality != EditorRenderQuality::Interactive) {
    intent.frame_role = FrameRoleForQuality(intent.quality);
  } else if (intent.frame_role == FrameRole::InteractivePrimary &&
             intent.quality == EditorRenderQuality::Interactive) {
    intent.frame_role = FrameRoleForQuality(intent.quality);
  }
  if (intent.replacement_key.empty()) {
    intent.replacement_key = DefaultReplacementKey(intent.quality);
  }
}

}  // namespace alcedo
