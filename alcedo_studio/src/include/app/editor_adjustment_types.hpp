//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

/// Reusable adjustment patch/snapshot data for editor session and render intents.
/// Lives in the application layer so session/render ports never depend on QWidget
/// control panels (Phase 5A-Fix).

namespace alcedo {

/// One atomic adjustment change (field key + serialized params).
struct EditorAdjustmentPatch {
  /// Stable field id, e.g. "exposure", "contrast", "lut".
  std::string field_key;
  /// Serialized operator/panel parameters (JSON text).
  std::string params_json;
  /// True when the input sequence has settled (quality ladder); false while dragging.
  bool settled = false;
  /// Enabled state captured with the immutable adjustment value. Interactive callers that do not
  /// provide a separate enabled flag keep the default and may encode it inside params_json.
  bool enabled = true;

  auto operator==(const EditorAdjustmentPatch& other) const -> bool = default;
};

/// Full adjustment snapshot stamped onto a render intent.
struct EditorRenderAdjustmentSnapshot {
  std::uint64_t snapshot_generation = 0;
  /// Compact digest for cheap equality (optional; may equal params hash).
  std::string   fingerprint;
  /// Full pipeline or panel parameter set for this render (JSON text).
  std::string   params_json;
  /// Ordered patches applied since the previous committed snapshot (may be empty).
  std::vector<EditorAdjustmentPatch> patches;

  auto operator==(const EditorRenderAdjustmentSnapshot& other) const -> bool = default;
};

}  // namespace alcedo
