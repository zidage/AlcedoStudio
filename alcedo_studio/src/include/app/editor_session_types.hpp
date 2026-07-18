//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "type/type.hpp"

namespace alcedo {

/// Explicit editor-session lifecycle states (Phase 5A).
/// UI shell observes these; it never drives pipeline scheduling directly.
enum class EditorSessionState : std::uint8_t {
  NoImage = 0,
  Acquiring,
  Loading,
  Interactive,
  Saving,
  Switching,
  Failed,
  ShuttingDown,
};

/// Typed session intents submitted by the QML facade or internal recovery paths.
enum class EditorSessionIntentKind : std::uint8_t {
  Open = 0,
  Switch,
  Patch,
  GestureCommit,
  Undo,
  Redo,
  Discard,
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
  sl_element_id_t element_id = 0;
  image_id_t      image_id   = 0;
  std::uint64_t   session_generation = 0;
  std::uint64_t   render_generation  = 0;
  std::uint64_t   view_generation    = 0;
};

struct EditorSessionIntent {
  EditorSessionIntentKind kind = EditorSessionIntentKind::Open;
  sl_element_id_t         element_id = 0;
  image_id_t              image_id   = 0;
  /// Opaque adjustment patch fingerprint or field key for Patch intents.
  std::string             patch_key;
  /// Optional human-readable failure/context payload for tests and diagnostics.
  std::string             note;
};

struct EditorSessionResult {
  EditorSessionResultKind kind  = EditorSessionResultKind::Accepted;
  EditorSessionState      state = EditorSessionState::NoImage;
  EditorSessionIdentity   identity{};
  std::uint64_t           render_request_id = 0;
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
      return true;
    case EditorSessionState::NoImage:
    case EditorSessionState::Failed:
    case EditorSessionState::ShuttingDown:
      return false;
  }
  return false;
}

}  // namespace alcedo
