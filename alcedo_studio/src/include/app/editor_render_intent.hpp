//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

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
};

/// Opaque presentation-sink identity. The coordinator never owns the sink; the
/// session service resolves it and stamps the identity on each intent.
using PresentationSinkId = std::uint64_t;

/// Immutable adjustment snapshot carried with a render intent. Phase 5A stores a
/// fingerprint only; Phase 5B/6 fill pipeline params from reusable snapshot types
/// that already live outside QWidget ownership (`EditorAdjustmentSnapshot`,
/// `AdjustmentPreview` / `AdjustmentCommit`).
struct EditorRenderAdjustmentSnapshot {
  std::uint64_t snapshot_generation = 0;
  std::string   fingerprint;
};

/// Cancellation token shared by intent producers and the coordinator. Setting
/// `cancelled` to true rejects or aborts the matching request generation.
struct EditorRenderCancellationToken {
  std::atomic<bool> cancelled{false};

  void Cancel() { cancelled.store(true, std::memory_order_release); }
  [[nodiscard]] auto IsCancelled() const -> bool {
    return cancelled.load(std::memory_order_acquire);
  }
};

/// Immutable render intent accepted only by EditorRenderCoordinator.
struct EditorRenderIntent {
  sl_element_id_t element_id = 0;
  image_id_t      image_id   = 0;
  std::uint64_t   session_generation = 0;
  std::uint64_t   render_generation  = 0;
  std::uint64_t   view_generation    = 0;
  EditorRenderReason   reason   = EditorRenderReason::InitialFrame;
  EditorRenderAdjustmentSnapshot adjustment{};
  std::optional<ViewportRenderRegion> view_region;
  int requested_width  = 0;
  int requested_height = 0;
  FrameRole            frame_role = FrameRole::InteractivePrimary;
  EditorRenderQuality  quality    = EditorRenderQuality::Interactive;
  EditorRenderPriority priority   = EditorRenderPriority::Normal;
  /// Same key replaces prior pending work (e.g. "interactive", "quality", "detail").
  std::string replacement_key;
  std::shared_ptr<EditorRenderCancellationToken> cancellation;
  PresentationSinkId presentation_sink_id = 0;
};

struct EditorRenderRequest {
  std::uint64_t     request_id = 0;
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
      return EditorRenderQuality::Interactive;
  }
  return EditorRenderQuality::Interactive;
}

}  // namespace alcedo
