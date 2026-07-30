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

class CPUPipelineExecutor;

struct EditorAdjustmentFieldSpec {
  PipelineStageName stage_name    = PipelineStageName::Stage_Count;
  OperatorType      operator_type = OperatorType::UNKNOWN;
};

struct EditorAdjustmentOperatorState {
  nlohmann::json params  = nullptr;
  bool           enabled = false;
};

/// Resolve the stable QML field key to the pipeline operator it controls.
auto ResolveEditorAdjustmentField(const std::string& field_key)
    -> std::optional<EditorAdjustmentFieldSpec>;

/// Return the canonical QML field key for a committed operator payload.
auto EditorAdjustmentFieldKey(PipelineStageName stage_name, OperatorType operator_type)
    -> std::optional<std::string>;

/// Read or apply one complete operator state. The caller owns the executor
/// render lock when this runs against a live production pipeline.
auto ReadEditorAdjustmentOperatorState(CPUPipelineExecutor& executor, const std::string& field_key,
                                       EditorAdjustmentOperatorState* state, std::string* error)
    -> bool;
auto ApplyEditorAdjustmentOperatorState(CPUPipelineExecutor&                 executor,
                                        const EditorAdjustmentFieldSpec&     spec,
                                        const EditorAdjustmentOperatorState& state,
                                        std::string*                         error) -> bool;

/// Applies one render request's adjustment state to an executor. The caller
/// must hold executor.GetRenderLock() so the state and resulting frame belong
/// to the same render generation.
auto ApplyEditorAdjustmentSnapshot(CPUPipelineExecutor&                  executor,
                                   const EditorRenderAdjustmentSnapshot& snapshot,
                                   std::string*                          error) -> bool;

/// Disable the geometry operator for an overlay editing preview while keeping
/// its parameters installed on the executor for the next full render.
void DisableEditorGeometryOperatorForOverlay(CPUPipelineExecutor& executor);

}  // namespace alcedo
