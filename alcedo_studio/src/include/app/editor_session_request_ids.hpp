//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

namespace alcedo {

/// Opaque id for one image-load / switch acquire. Completions match by equality
/// only; values are not ordered and are discarded when the load finishes or is
/// cancelled. Internal to C++ worker boundaries — not exposed to QML.
struct ImageLoadRequestId {
  std::uint64_t value = 0;

  [[nodiscard]] auto valid() const -> bool { return value != 0; }
  [[nodiscard]] auto operator==(const ImageLoadRequestId&) const -> bool = default;
};

/// Opaque id for one editor render request. First-frame and stale-frame gates
/// match this id; they never compare session-wide generations.
struct EditorRenderRequestId {
  std::uint64_t value = 0;

  [[nodiscard]] auto valid() const -> bool { return value != 0; }
  [[nodiscard]] auto operator==(const EditorRenderRequestId&) const -> bool = default;
};

/// Opaque id for one save / materialization task. Paired with the initiating
/// EditorSessionOperationId.command_id for stale-completion rejection.
struct EditorSaveTaskId {
  std::uint64_t value = 0;

  [[nodiscard]] auto valid() const -> bool { return value != 0; }
  [[nodiscard]] auto operator==(const EditorSaveTaskId&) const -> bool = default;
};

/// Opaque id for one Merge preview. Complete/Cancel Merge are valid only while
/// this id remains active and the package/head fingerprint still matches.
struct MergePreviewId {
  std::uint64_t value = 0;

  [[nodiscard]] auto valid() const -> bool { return value != 0; }
  [[nodiscard]] auto operator==(const MergePreviewId&) const -> bool = default;
};

}  // namespace alcedo
