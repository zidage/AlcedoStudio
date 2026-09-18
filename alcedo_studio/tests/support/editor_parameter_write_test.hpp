//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "app/editor_parameter_write.hpp"
#include "json.hpp"

namespace alcedo::test {

/// Brace-init helper for tests that still describe a field JSON object.
struct EditorPatchJsonInit {
  std::string field_key;
  std::string params_json;
  bool        settled = false;
};

/// Build a live write from field JSON. Used by tests that still describe the
/// QML collection object; the queue stores the parsed operation, not the string.
inline auto PatchFromJson(std::string field, std::string json, bool settled = false)
    -> EditorAdjustmentPatch {
  EditorAdjustmentPatch patch;
  patch.field_key   = std::move(field);
  patch.params_json = json;
  patch.settled     = settled;
  if (patch.field_key.empty()) {
    return patch;
  }
  nlohmann::json parsed = nlohmann::json::object();
  if (!json.empty()) {
    parsed = nlohmann::json::parse(json, nullptr, false);
    if (parsed.is_discarded()) {
      return patch;
    }
  }
  std::string error;
  auto        write = ParseEditorParameterWrite(patch.field_key, parsed, &error);
  if (write.has_value()) {
    patch.write = std::move(*write);
  }
  return patch;
}

inline auto PatchFromJson(EditorPatchJsonInit init) -> EditorAdjustmentPatch {
  return PatchFromJson(std::move(init.field_key), std::move(init.params_json), init.settled);
}

inline auto ScalarPatch(std::string field, float value, bool settled = false)
    -> EditorAdjustmentPatch {
  EditorAdjustmentPatch patch;
  patch.field_key = std::move(field);
  patch.write     = EditorScalarWrite{value};
  patch.settled   = settled;
  return patch;
}

/// Snapshot / panel-projection payload only. Live queue and owner apply require
/// @ref PatchFromJson or @ref ScalarPatch so @c write is populated.
inline auto SnapshotPatch(std::string field, std::string json, bool settled = false)
    -> EditorAdjustmentPatch {
  EditorAdjustmentPatch patch;
  patch.field_key   = std::move(field);
  patch.params_json = std::move(json);
  patch.settled     = settled;
  return patch;
}

inline auto SnapshotPatch(EditorPatchJsonInit init) -> EditorAdjustmentPatch {
  return SnapshotPatch(std::move(init.field_key), std::move(init.params_json), init.settled);
}

[[nodiscard]] inline auto ScalarValue(const EditorParameterWrite& write) -> float {
  const auto* scalar = std::get_if<EditorScalarWrite>(&write);
  return scalar == nullptr ? 0.0f : scalar->value;
}

/// Snapshot projection equality. Live queue writes are not part of this
/// comparison; those use typed @c write payloads.
[[nodiscard]] inline auto SameSnapshotProjection(const EditorRenderAdjustmentSnapshot& lhs,
                                                 const EditorRenderAdjustmentSnapshot& rhs)
    -> bool {
  if (lhs.snapshot_generation != rhs.snapshot_generation || lhs.fingerprint != rhs.fingerprint ||
      lhs.params_json != rhs.params_json || lhs.patches.size() != rhs.patches.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.patches.size(); ++i) {
    const auto& a = lhs.patches[i];
    const auto& b = rhs.patches[i];
    if (a.field_key != b.field_key || a.params_json != b.params_json || a.settled != b.settled ||
        a.enabled != b.enabled || a.target != b.target) {
      return false;
    }
  }
  return true;
}

}  // namespace alcedo::test
