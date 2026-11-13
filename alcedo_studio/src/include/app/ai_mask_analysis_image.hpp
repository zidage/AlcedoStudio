//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "app/pipeline_root_state.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/runtime/rendered_pipeline_image.hpp"
#include "sidecar_client/dto/segmentation.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief The analysis image of the AI Mask tool (SM4): the image that the segmentation sidecar
 * reads.
 *
 * The analysis image is the whole source in ReferenceSpace, without the document crop and
 * rotation (master plan D7). Its document is the import root of the image with the current
 * Develop settings (SM4 plan S1), so user grading never changes it. Normalized analysis image
 * coordinates equal normalized ReferenceSpace coordinates.
 */
namespace alcedo {

/// Long edge limit of the analysis image; the segmentation start request accepts at most 4096.
inline constexpr std::uint32_t kAiMaskAnalysisMaxLongEdge    = 4096;

/// Prefix of every analysis image key. A change of the key content changes the prefix.
inline constexpr const char*   kAiMaskAnalysisImageKeyPrefix = "am1-";

/**
 * @brief Build the frozen analysis document of one image from its held history.
 *
 * Replays the immutable root with no commit, checks it as a product graph, rejects an HDR root
 * DRT, gives it every Develop setting of @p current (PipelineDocument::UseDevelopSettingsFrom),
 * checks the bound camera profile, and freezes it as a preview with the lineage and image of
 * @p current, so the editor executor keeps its binding.
 *
 * Reads @p graph and @p root only; the caller holds the history owner's access for the call.
 *
 * @param current Preview of the working values of the same image.
 * @throws std::runtime_error or std::invalid_argument with the real reason.
 */
[[nodiscard]] auto             BuildAiMaskAnalysisDocument(
                const CommitGraph& graph, const LoadedRootState& root,
                const std::shared_ptr<const PipelineGraphSnapshot>& current, sl_element_id_t element_id)
    -> std::shared_ptr<const PipelineGraphSnapshot>;

/**
 * @brief Image key of the analysis image (SM4 plan S5), at most 256 bytes.
 *
 * `am1-` and the 32 hexadecimal digits of the XXH3 128-bit hash of the key version, @p element_id,
 * @p image_id, the geometry use, the long edge limit, and the canonical JSON of @p document. The
 * key has no model set identity: the sidecar clears its cache on a model change.
 */
[[nodiscard]] auto MakeAiMaskAnalysisImageKey(sl_element_id_t element_id, image_id_t image_id,
                                              const PipelineDocument& document) -> std::string;

/**
 * @brief Geometry fingerprint of an AI Mask: the Lens and RAW decode settings that move image
 *        content in the analysis image.
 *
 * 32 lowercase hexadecimal digits of the XXH3 128-bit hash of the canonical JSON of the Develop
 * fields `demosaic_method`, `highlights_reconstruct`, `lens_enabled`, `apply_distortion`,
 * `apply_tca`, `apply_crop`, `auto_scale`, `use_user_scale`, `user_scale`, `projection_enabled`,
 * `target_projection`, `lens_maker`, and `lens_model`. White balance and vignetting do not
 * change it. Each field is read through the Model lock.
 */
[[nodiscard]] auto MakeAiMaskGeometryFingerprint(const DevelopParamsModel& develop) -> std::string;

/// Render request of the analysis image: the whole ReferenceSource frame, Preview resampling, long
/// edge at most kAiMaskAnalysisMaxLongEdge.
[[nodiscard]] auto MakeAiMaskAnalysisRenderRequest() -> RenderRequest;

/**
 * @brief Convert the rendered analysis image to the RGB8 input of the segmentation session.
 *
 * Rounds each display-encoded value to 0 to 255 with saturation and drops alpha (the SDR rule of
 * the editor comparison). The result is row-major RGB8 at the render size.
 *
 * @throws std::invalid_argument when the image has no RGBA32F host pixels, the pixel size differs
 *         from the render size, the long edge is above kAiMaskAnalysisMaxLongEdge, the output is
 *         ST 2084 or HLG, or a value is not finite.
 */
[[nodiscard]] auto ConvertAiMaskAnalysisImage(const RenderedPipelineImage& rendered)
    -> SegmentationRgb8Image;

}  // namespace alcedo
