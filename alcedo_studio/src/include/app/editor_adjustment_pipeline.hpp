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
///
/// Call only for content-bearing renders (see ReasonAppliesAdjustmentSnapshot).
/// Do not call on Detail ROI / scope ROI / pure view transforms — those must
/// only retarget Geometry render params so Image Loading (RAW_DECODE) stays
/// cached across pan/zoom frames.
///
/// Slider / field edits should stamp only the changed field patch(es) onto the
/// intent (not a full history snapshot). Replaying raw_decode/lens_calib on
/// every exposure drag thrash-invalidates the Image Loading stage cache.
auto ApplyEditorAdjustmentSnapshot(CPUPipelineExecutor&                  executor,
                                   const EditorRenderAdjustmentSnapshot& snapshot,
                                   std::string*                          error) -> bool;

/// True when applying this snapshot may touch Image Loading (RAW_DECODE /
/// LENS_CALIBRATION) or rebuild the full pipeline. Used to gate loading-stage
/// default ensure and similar work off the slider hot path.
[[nodiscard]] auto SnapshotTouchesImageLoading(const EditorRenderAdjustmentSnapshot& snapshot)
    -> bool;

/// Disable the geometry operator for an overlay editing preview while keeping
/// its parameters installed on the executor for the next full render.
void DisableEditorGeometryOperatorForOverlay(CPUPipelineExecutor& executor);

}  // namespace alcedo
