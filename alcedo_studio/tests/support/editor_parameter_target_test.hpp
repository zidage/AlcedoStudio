//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "app/editor_adjustment_types.hpp"

namespace alcedo::test {

/// Complete Color Grade target for tests. Production callers must fill the same fields.
inline auto ColorGradeFieldTarget(std::string field, std::string node_id = "grade.primary")
    -> EditorParameterTarget {
  EditorParameterTarget target;
  target.owner_kind             = EditorParameterOwnerKind::ColorGrade;
  target.node_id                = NodeId{node_id};
  target.adjustment_instance_id = AdjustmentInstanceId{node_id + "." + field};
  target.field_key              = std::move(field);
  return target;
}

inline auto DrtPostFieldTarget(std::string field, std::string node_id = "drt")
    -> EditorParameterTarget {
  EditorParameterTarget target;
  target.owner_kind = EditorParameterOwnerKind::DrtPost;
  target.node_id    = NodeId{node_id};
  if (field != "odt") {
    target.adjustment_instance_id = AdjustmentInstanceId{node_id + "." + field};
  }
  target.field_key = std::move(field);
  return target;
}

inline auto WithColorGradeTarget(EditorAdjustmentPatch patch, std::string node_id = "grade.primary")
    -> EditorAdjustmentPatch {
  patch.target = ColorGradeFieldTarget(patch.field_key, std::move(node_id));
  return patch;
}

inline auto WithDrtPostTarget(EditorAdjustmentPatch patch, std::string node_id = "drt")
    -> EditorAdjustmentPatch {
  patch.target = DrtPostFieldTarget(patch.field_key, std::move(node_id));
  return patch;
}

}  // namespace alcedo::test
