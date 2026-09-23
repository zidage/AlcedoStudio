//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <json.hpp>
#include <optional>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "edit/operators/op_base.hpp"

namespace alcedo {

struct EditorAdjustmentFieldSpec {
  PipelineStageName stage_name    = PipelineStageName::Stage_Count;
  OperatorType      operator_type = OperatorType::UNKNOWN;
};

/// Resolve the stable QML field key to the pipeline operator it controls.
auto ResolveEditorAdjustmentField(const std::string& field_key)
    -> std::optional<EditorAdjustmentFieldSpec>;

/**
 * @brief Map a field write payload onto PipelineDocument Model JSON keys.
 *
 * Panel writes use field keys (`exposure`) or a scalar `value`. Document Models
 * use `exposure_ev` / `cube_path`. Unknown keys are left unchanged.
 */
auto EditorAdjustmentDocumentParamsFromWrite(const std::string& field_key, nlohmann::json params)
    -> nlohmann::json;

/// Return the canonical QML field key for a committed operator payload.
auto EditorAdjustmentFieldKey(PipelineStageName stage_name, OperatorType operator_type)
    -> std::optional<std::string>;

}  // namespace alcedo
