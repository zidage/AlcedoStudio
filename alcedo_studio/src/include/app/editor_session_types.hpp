//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_render_intent.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Explicit editor-session lifecycle states (Phase 5A).
/// UI shell observes these; it never drives pipeline scheduling directly.
///
/// `RetainedImageFailure` is a non-fatal failure state introduced by Phase 7A
/// repair: a save or Version-checkpoint failed, but the prior image identity,
/// guards, and last presented frame are still valid. `hasImage` stays true so
/// the viewport keeps showing the retained frame instead of the empty-editor
/// placeholder. Recovery actions (Retry Save, Discard and Continue, Cancel)
/// resolve back to Interactive, Saving, or the pending target.
enum class EditorSessionState : std::uint8_t {
  NoImage = 0,
  Acquiring,
  Loading,
  Interactive,
  Saving,
  Switching,
  RetainedImageFailure,
  Failed,
  ShuttingDown,
};

/// Typed session intents submitted by the QML facade or internal recovery paths.
enum class EditorSessionIntentKind : std::uint8_t {
  Open = 0,
  Switch,
  Close,
  Patch,
  CommitAdjustment,
  Undo,
  Redo,
  Discard,
  // Phase 5D: pure viewport/geometry view change (zoom, pan, resize, crop/
  // rotation, ROI). Carries the render reason in EditorSessionIntent::view_reason;
  // the coordinator decides reuse vs. InteractivePrimary vs. DetailPatch.
  ViewChange,
  Shutdown,
};

/// Outcomes published by EditorSessionService for UI and tests.
enum class EditorSessionResultKind : std::uint8_t {
  Accepted = 0,
  StateChanged,
  ImageReady,
  RenderRouted,
  SaveStarted,
  SaveFinished,
  Failed,
  Rejected,
};

struct EditorSessionIdentity {
  sl_element_id_t element_id         = 0;
  image_id_t      image_id           = 0;
  std::uint64_t   session_generation = 0;
  std::uint64_t   render_generation  = 0;
  std::uint64_t   view_generation    = 0;
};

struct EditorSessionIntent {
  EditorSessionIntentKind        kind       = EditorSessionIntentKind::Open;
  sl_element_id_t                element_id = 0;
  image_id_t                     image_id   = 0;
  /// Full adjustment patch for Patch / CommitAdjustment (field + params).
  EditorAdjustmentPatch          patch{};
  /// Optional full snapshot when the producer already has one.
  EditorRenderAdjustmentSnapshot adjustment{};
  /// ViewChange reason (zoom/pan/resize/crop-rotation/ROI). Ignored for other
  /// intent kinds. Phase 5D.
  EditorRenderReason             view_reason = EditorRenderReason::ZoomPan;
  /// ViewChange requested region (the visible viewport ROI). Attached to the
  /// render intent for DetailRefresh so the request carries its requested
  /// region (Phase 5D D5). Nullopt for full-frame view changes.
  std::optional<ViewportRenderRegion> view_region{std::nullopt};
  /// Optional human-readable failure/context payload for tests and diagnostics.
  std::string                    note{};
  /// Close persists by default. False means discard the current unflushed edit.
  bool                           persist_changes = true;

  /// Convenience: field key only (maps into patch.field_key).
  [[nodiscard]] auto             patch_key() const -> const std::string& { return patch.field_key; }
};

struct EditorSessionResult {
  EditorSessionResultKind kind  = EditorSessionResultKind::Accepted;
  EditorSessionState      state = EditorSessionState::NoImage;
  EditorSessionIdentity   identity{};
  std::uint64_t           render_request_id = 0;
  /// Background task id for SaveStarted / SaveFinished pairing.
  std::uint64_t           task_id           = 0;
  std::string             message;
};

[[nodiscard]] inline auto EditorSessionStateName(EditorSessionState state) -> const char* {
  switch (state) {
    case EditorSessionState::NoImage:
      return "NoImage";
    case EditorSessionState::Acquiring:
      return "Acquiring";
    case EditorSessionState::Loading:
      return "Loading";
    case EditorSessionState::Interactive:
      return "Interactive";
    case EditorSessionState::Saving:
      return "Saving";
    case EditorSessionState::Switching:
      return "Switching";
    case EditorSessionState::RetainedImageFailure:
      return "RetainedImageFailure";
    case EditorSessionState::Failed:
      return "Failed";
    case EditorSessionState::ShuttingDown:
      return "ShuttingDown";
  }
  return "Unknown";
}

[[nodiscard]] inline auto EditorSessionHasImage(EditorSessionState state) -> bool {
  switch (state) {
    case EditorSessionState::Acquiring:
    case EditorSessionState::Loading:
    case EditorSessionState::Interactive:
    case EditorSessionState::Saving:
    case EditorSessionState::Switching:
    case EditorSessionState::RetainedImageFailure:
      return true;
    case EditorSessionState::NoImage:
    case EditorSessionState::Failed:
    case EditorSessionState::ShuttingDown:
      return false;
  }
  return false;
}

}  // namespace alcedo
