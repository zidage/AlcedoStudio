//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <string>

#include "app/adjustment_transfer_types.hpp"
#include "app/pipeline_root_state.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief The preset browser preview of one preset (edit presets plan 2.4, PS3): the import root of
 * the open photo with the preset applied by the Paste planner.
 *
 * A preview is the result of an apply of the preset, except for the sensor stage: the demosaic
 * method, AI Denoise, highlight reconstruction, lens correction, and the camera profile come from
 * the current Version, so the editor executor reuses the sensor result that it holds (plan D6).
 * Edits of the current Version outside these settings do not change a preview.
 */
namespace alcedo {

/**
 * @brief Build the frozen preset preview document of one image from its held history.
 *
 * Replays the immutable root with no commit, checks it as a product graph, plans the Paste of
 * @p package against the root document (DocumentTransferPlanner::Plan, the planner of each Paste
 * path) and applies its batch, then gives the document the sensor settings of @p current
 * (PipelineDocument::UseSensorSettingsFrom). The white balance of the preset stays. The preview is
 * frozen with the lineage and image of @p current, so the editor executor keeps its binding.
 *
 * Reads @p graph and @p root only; the caller holds the history owner's access for the call.
 *
 * @param current Preview of the working values of the same image.
 * @param package A checked preset package (ReadPresetFile or PresetLibraryService::PackageFor).
 * @throws std::runtime_error or std::invalid_argument with the real reason, for example when the
 *         package does not apply to the root.
 */
[[nodiscard]] auto BuildPresetPreviewDocument(
    const CommitGraph& graph, const LoadedRootState& root,
    const std::shared_ptr<const PipelineGraphSnapshot>& current, sl_element_id_t element_id,
    const AdjustmentTransferPackage& package) -> std::shared_ptr<const PipelineGraphSnapshot>;

/**
 * @brief Text key of the sensor settings that a preset preview takes from @p current.
 *
 * The XXH3 hash, in hex, of the Develop parameters of @p current without the white-balance fields
 * (the fields that PipelineDocument::UseSensorSettingsFrom does not take). Equal settings give an
 * equal key in one process; a change of the demosaic method, AI Denoise, highlight
 * reconstruction, lens correction, or the camera profile gives another key. Empty when
 * @p current has no Develop node.
 */
[[nodiscard]] auto PresetPreviewSensorKey(const PipelineDocument& current) -> std::string;

}  // namespace alcedo
