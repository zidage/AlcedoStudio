//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "app/editor_parameter_write.hpp"
#include "app/pipeline_root_state.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief The LUT browser preview of one LUT (issue #598): the import root of the open photo with
 * that LUT on its primary Color Grade.
 *
 * A preview shows the direction of a LUT, not the exact result on the current edit. So the
 * document is the immutable import root, not the working document: edits of the photo do not
 * change a preview. Only the LUT and its input and output encodings do. The preview renders
 * through the editor image render port, so the DRT and Post steps of the root are in it.
 */
namespace alcedo {

/// Long edge of a LUT preview, in pixels. A tile is at least 128 logical pixels wide.
inline constexpr std::uint32_t kLutPreviewLongEdge = 256;

/**
 * @brief Build the frozen LUT preview document of one image from its held history.
 *
 * Replays the immutable root with no commit, checks it as a product graph, gives it the sensor
 * settings of @p current (PipelineDocument::UseSensorSettingsFrom, so the editor executor reuses
 * the sensor result of the image), and applies @p write at full strength to the LMT of the root
 * primary Color Grade. The preview is frozen with the lineage and image of @p current, so the
 * editor executor keeps its binding.
 *
 * Reads @p graph and @p root only; the caller holds the history owner's access for the call.
 *
 * @param current Preview of the working values of the same image.
 * @param write Reference, display name, and encodings of the LUT. Its strength is ignored.
 * @throws std::runtime_error or std::invalid_argument with the real reason, for example when the
 *         root has no primary Color Grade with a LUT adjustment.
 */
[[nodiscard]] auto             BuildLutPreviewDocument(
                const CommitGraph& graph, const LoadedRootState& root,
                const std::shared_ptr<const PipelineGraphSnapshot>& current, sl_element_id_t element_id,
                const EditorLutWrite& write) -> std::shared_ptr<const PipelineGraphSnapshot>;

/// Render request of a LUT preview: the whole root frame (the root has no crop), Preview
/// resampling, long edge kLutPreviewLongEdge.
[[nodiscard]] auto MakeLutPreviewRenderRequest() -> RenderRequest;

/**
 * @brief Output encoding of a LUT preview of @p document.
 *
 * Empty for an SDR DRT: the preview uses the encoding of the document. For an ST 2084 or HLG
 * DRT, Rec.709 gamma 2.2 at 100 nits (ExportColorProfileConfig defaults), because a tile is an
 * 8-bit SDR image.
 */
[[nodiscard]] auto LutPreviewOutputColor(const PipelineDocument& document)
    -> std::optional<ExportColorProfileConfig>;

}  // namespace alcedo
