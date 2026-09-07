//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_parameter_write.hpp"
#include "edit/graph/graph_ids.hpp"

/// Reusable adjustment patch/snapshot data for editor session and render intents.
/// Lives in the application layer so session/render ports never depend on QWidget
/// control panels (Phase 5A-Fix).

namespace alcedo {

/// Owner of one editor parameter write. Unspecified is filled at history from the
/// current-panel field_key. ApplyEditorParameterPatch still requires a complete target.
enum class EditorParameterOwnerKind : std::uint8_t {
  Unspecified = 0,
  Document,
  Develop,
  ColorGrade,
  ColorGradeMask,
  DrtPost,
};

/**
 * @brief Identity of one parameter write on PipelineDocument.
 *
 * Every production patch must fill this completely. The app does not invent
 * owner_kind or node_id from field_key or UI selection.
 *
 * @pre ColorGrade writes set node_id and adjustment_instance_id.
 * @pre Document writes leave node_id empty.
 * @pre DrtPost writes set node_id. Clarity/Sharpen/Halation/Film Grain also set
 *      adjustment_instance_id. `odt` may leave adjustment_instance_id empty.
 * @pre mask_id stays empty until NM3.
 */
struct EditorParameterTarget {
  EditorParameterOwnerKind owner_kind = EditorParameterOwnerKind::Unspecified;
  NodeId                   node_id;
  AdjustmentInstanceId     adjustment_instance_id;
  std::string              mask_id;
  std::string              field_key;

  auto operator==(const EditorParameterTarget& other) const -> bool = default;
};

/**
 * @brief Return a rejection message when @p target is not a complete production target.
 *
 * @param target Patch target.
 * @param patch_field_key Field key on the enclosing patch; must equal target.field_key.
 * @return Empty when the target is complete and is not a Mask write.
 */
[[nodiscard]] inline auto DescribeEditorParameterTargetError(const EditorParameterTarget& target,
                                                             const std::string& patch_field_key)
    -> std::string {
  if (target.owner_kind == EditorParameterOwnerKind::Unspecified) {
    return "Editor parameter target requires owner_kind";
  }
  if (target.owner_kind == EditorParameterOwnerKind::ColorGradeMask || !target.mask_id.empty()) {
    return "Mask parameter targets are rejected until NM3";
  }
  if (target.field_key.empty()) {
    return "Editor parameter target requires field_key";
  }
  if (target.field_key != patch_field_key) {
    return "Editor parameter target field_key must match the patch field_key";
  }
  switch (target.owner_kind) {
    case EditorParameterOwnerKind::Document:
      if (!target.node_id.Empty() || !target.adjustment_instance_id.Empty()) {
        return "Document parameter target must not set node_id or adjustment_instance_id";
      }
      return {};
    case EditorParameterOwnerKind::Develop:
      if (target.node_id.Empty()) {
        return "Editor parameter target requires node_id";
      }
      return {};
    case EditorParameterOwnerKind::DrtPost:
      if (target.node_id.Empty()) {
        return "Editor parameter target requires node_id";
      }
      if ((target.field_key == "clarity" || target.field_key == "sharpen" ||
           target.field_key == "halation" || target.field_key == "film_grain") &&
          target.adjustment_instance_id.Empty()) {
        return "DRT/Post parameter target requires adjustment_instance_id";
      }
      return {};
    case EditorParameterOwnerKind::ColorGrade:
      if (target.node_id.Empty()) {
        return "Editor parameter target requires node_id";
      }
      if (target.adjustment_instance_id.Empty()) {
        return "Color Grade parameter target requires adjustment_instance_id";
      }
      return {};
    default:
      return "Editor parameter target owner_kind is not supported";
  }
}

/// One atomic adjustment change (field key + typed write, or snapshot JSON).
struct EditorAdjustmentPatch {
  /// Stable field id, e.g. "exposure", "contrast", "lut".
  std::string field_key;
  /// Live field operation. Required for queue and owner apply.
  std::optional<EditorParameterWrite> write;
  /// True when the input sequence has settled (quality ladder); false while dragging.
  bool settled = false;
  /// Enabled state captured with the immutable adjustment value.
  bool enabled = true;
  /// Production write identity. Unspecified is filled at history; Apply still requires a complete target.
  EditorParameterTarget target{};
  /// CPU operator JSON for executor remirror, committed snapshots, and history restore.
  /// Live queue entries must not use this as the write payload.
  std::string params_json;
};

/// Full adjustment snapshot stamped onto a render intent.
struct EditorRenderAdjustmentSnapshot {
  std::uint64_t snapshot_generation = 0;
  /// Compact digest for cheap equality (optional; may equal params hash).
  std::string   fingerprint;
  /// Unused on the live write path. Checkpoint helpers may still store a stage document here.
  std::string   params_json;
  /// Ordered patches applied since the previous committed snapshot (may be empty).
  std::vector<EditorAdjustmentPatch> patches;

};

}  // namespace alcedo
