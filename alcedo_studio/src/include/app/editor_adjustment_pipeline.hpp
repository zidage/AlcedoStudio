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
class PipelineDocument;

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

}  // namespace alcedo
