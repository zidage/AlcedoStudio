//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <json.hpp>
#include <optional>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "edit/history/commit_types.hpp"
#include "edit/operators/op_base.hpp"

namespace alcedo {

class CommitGraph;
class CPUPipelineExecutor;
class EditCommit;
class PipelineDocument;

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

/**
 * @brief Map a field write payload onto PipelineDocument Model JSON keys.
 *
 * Panel writes use field keys (`exposure`) or a scalar `value`. Document Models
 * use `exposure_ev` / `cube_path`. Unknown keys are left unchanged.
 */
auto EditorAdjustmentDocumentParamsFromWrite(const std::string& field_key, nlohmann::json params)
    -> nlohmann::json;

/**
 * @brief Map a field write or document Model JSON onto CPU operator keys.
 *
 * CPU operators use `exposure` / `ocio_lmt`. Document `exposure_ev` / `cube_path`
 * and scalar `value` writes are rewritten. Unknown keys are left unchanged.
 */
auto EditorAdjustmentExecutorParamsFromWrite(const std::string& field_key, nlohmann::json params)
    -> nlohmann::json;

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

/**
 * @brief Install default editable operator params while preserving image-local keys.
 *
 * Image-local keys (RAW inherent context, as-shot white balance cache, lens EXIF
 * identity) stay on the live pipeline. User-editable keys reset from
 * `default_pipeline_params.hpp`. Caller must hold the executor render lock.
 *
 * @return false when a SetOperator fails; @p error receives the reason.
 */
auto ResetEditableOperatorsToDefaultsPreservingImageLocal(CPUPipelineExecutor& executor,
                                                          std::string*         error) -> bool;

/**
 * @brief Rebuild live pipeline operator state for a history head.
 *
 * Algorithm (plan §4.7):
 * 1. Capture prior ExportPipelineParams for failure rollback.
 * 2. Reset editable operators to defaults (preserve image-local keys).
 * 3. Apply first-parent chain commit after-values via SetOperator.
 * 4. Remirror current-panel CPU operators from the bound PipelineDocument when
 *    present, so Paste InsertNode values reach panel snapshots.
 * 5. SetExecutionStages once.
 * 6. On any failure, ImportPipelineParams(prior) and return false.
 *
 * Caller must hold the executor render lock. Does not touch Version refs, WAL,
 * or DuckDB.
 *
 * @param executor Live pipeline to mutate.
 * @param graph    Commit graph that owns @p head.
 * @param head     Target first-parent head (nullopt = defaults only).
 * @param error    Optional failure message.
 */
auto ApplyVersionHeadToLivePipeline(CPUPipelineExecutor&      executor, const CommitGraph& graph,
                                    const head_commit_hash_t& head, std::string* error) -> bool;

/**
 * @brief Copy current-panel Model JSON from @p document onto CPU stage operators.
 *
 * Grade fields use the Default Grade (`grade.primary`, else the first backbone
 * Grade). DRT/Post, Develop, and geometry fields use their document owners.
 * CPU aliases (`exposure` / `ocio_lmt`) are applied so panel snapshots match
 * the live document after Paste InsertNode or document replay.
 *
 * Missing owners are skipped. Caller holds the executor render lock.
 */
auto RemirrorCurrentPanelFromDocument(CPUPipelineExecutor& executor,
                                      const PipelineDocument& document, std::string* error)
    -> bool;

/**
 * @brief Copy one Model field onto the matching CPU stage operator.
 *
 * Used after a typed live write so the executor tracks the document owner.
 * Caller holds the executor render lock.
 */
auto RemirrorEditorParameterToExecutor(CPUPipelineExecutor& executor,
                                       const PipelineDocument& document,
                                       const EditorParameterTarget& target, std::string* error)
    -> bool;

/**
 * @brief Remirror CPU stages from one typed-batch commit's after (or before) values.
 *
 * Used by undo (before) and redo (after). Caller holds the render lock.
 */
auto ApplyHistoryCommitToLivePipeline(CPUPipelineExecutor& executor, const CommitGraph& graph,
                                      const EditCommit& commit, bool use_after_value,
                                      std::string* error) -> bool;

}  // namespace alcedo
