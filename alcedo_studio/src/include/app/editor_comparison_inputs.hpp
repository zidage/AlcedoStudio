//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <memory>
#include <optional>
#include <string>

#include "app/editor_comparison_types.hpp"
#include "app/pipeline_root_state.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief Build the inputs of the editor comparison from the held history of the open image.
 *
 * A comparison input is a read-only document. Building one never moves history, writes the
 * working document, or records a commit.
 */
namespace alcedo {

/**
 * @brief Build both comparison inputs of one image from its held history.
 *
 * Root replays the immutable root with no commit. Version replays the first-parent chain of that
 * Version's head, except the active Version, which uses @p current like Current. Each replay gets
 * the sensor settings of @p current (PipelineDocument::UseSensorSettingsFrom), is checked as a
 * product graph with a bound camera profile, and is frozen as a preview with the lineage and
 * image of @p current.
 *
 * Reads @p graph and @p root only; the caller holds the history owner's access for the call and
 * keeps no reference to them in the result. Does not lock, write, or publish anything.
 *
 * @param graph CommitGraph of the image that the editor holds.
 * @param root Decoded immutable root of the same image.
 * @param current Preview of the working values captured when the comparison opened.
 * @param element_id Image the editor holds; @p current and each Version must belong to it.
 * @return Both inputs, or nullopt with @p error set when a Version is missing or belongs to
 *         another image, a replay fails, a document is invalid, a referenced DNG profile is not
 *         loaded, or @p current or a selected document encodes HDR (ST 2084 or HLG). Nothing of a
 *         failed call is returned.
 */
[[nodiscard]] auto BuildEditorComparisonInputs(
    const CommitGraph& graph, const LoadedRootState& root,
    const std::shared_ptr<const PipelineGraphSnapshot>& current, sl_element_id_t element_id,
    const EditorComparisonSource& a, const EditorComparisonSource& b, std::string* error)
    -> std::optional<EditorComparisonInputPair>;

}  // namespace alcedo
