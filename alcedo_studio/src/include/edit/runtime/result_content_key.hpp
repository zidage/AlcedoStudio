//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <map>
#include <span>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Content keys and extents for one product frame's GPU result cache lookups.
 *
 * Viewport is in @ref geometry_scene_source only. CCT/tint is in @ref develop_image only.
 * Each compiled output also has an entry in @ref values keyed by @ref GraphValueId.
 * @ref primary_grade is the first compiled Color Grade scene key, or a no-Grade
 * identity derived from Develop when the backbone has none.
 */
struct FrameResultContentKeys {
  ContentKey                           sensor_linear{};
  ContentKey                           geometry_scene_source{};
  ContentKey                           develop_image{};
  ContentKey                           primary_grade{};
  ContentKey                           drt_display{};
  ContentKey                           mask{};
  ImageExtent                          sensor_extent{};
  ImageExtent                          geometry_extent{};
  std::map<GraphValueId, ContentKey>   values;

  /**
   * @brief Content key for @p id, or empty when that value was not hashed.
   *
   * Does not throw. Callers that require a compiled output must check Empty().
   */
  [[nodiscard]] auto Value(const GraphValueId& id) const -> ContentKey {
    const auto it = values.find(id);
    return it == values.end() ? ContentKey{} : it->second;
  }

  /**
   * @brief Scene-image key for Color Grade @p id, or empty when it was not compiled.
   */
  [[nodiscard]] auto GradeScene(const NodeId& id) const -> ContentKey {
    return Value(GraphValueId{id, PortId{"image"}});
  }
};

/**
 * @brief Hash PreparedSourceKey fields. Viewport is omitted.
 */
[[nodiscard]] auto HashPreparedSourceKey(const PreparedSourceKey& key) -> ContentKey;

/**
 * @brief Hash the full ResolvedRenderGeometry, including crop, rotation, ROI, and sampling.
 */
[[nodiscard]] auto HashResolvedRenderGeometry(const ResolvedRenderGeometry& geometry) -> ContentKey;

/**
 * @brief Identity of the canonical LLF reference for @p grade_id. Viewport ROI is omitted.
 *
 * Includes prepared source, user crop/rotation, CameraColor params, every preceding
 * Color Grade, this Grade's mask, and this Grade's adjustments including local-tone
 * sliders. Viewport, render extent, and dynamic scale are not mixed.
 *
 * @param grade_id Compiled Color Grade that owns the LLF stage.
 * @throws std::runtime_error when @p grade_id is not a compiled Color Grade.
 */
[[nodiscard]] auto HashLlfReferenceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                                       const PipelineDocument& document, const NodeId& grade_id)
    -> ContentKey;

/**
 * @brief @ref HashLlfReferenceKey for the first compiled Color Grade, or shared
 * identity alone when the backbone has none.
 */
[[nodiscard]] auto HashLlfReferenceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                                       const PipelineDocument& document) -> ContentKey;

/**
 * @brief Identity of the canonical LLF source pyramid for @p grade_id.
 *
 * Viewport ROI and this Grade's Shadows/Highlights slider values are omitted so a
 * slider edit can reuse the source. Preceding Grades are mixed in full, including
 * their local-tone sliders.
 *
 * @param grade_id Compiled Color Grade that owns the LLF stage.
 * @throws std::runtime_error when @p grade_id is not a compiled Color Grade.
 */
[[nodiscard]] auto HashLlfSourceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                                    const PipelineDocument& document, const NodeId& grade_id)
    -> ContentKey;

/**
 * @brief @ref HashLlfSourceKey for the first compiled Color Grade, or shared
 * identity alone when the backbone has none.
 */
[[nodiscard]] auto HashLlfSourceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                                    const PipelineDocument& document) -> ContentKey;

/**
 * @brief Hash pass inputs in PortId order. Does not sort by content hash.
 *
 * @param inputs Consumer ports and producer values. Duplicate ports are hashed twice.
 * @param produced Content key of each producer. Missing producers throw.
 * @throws std::runtime_error when an input has no producer key.
 */
[[nodiscard]] auto HashBoundInputs(std::span<const CompiledPassInput>     inputs,
                                   const std::map<GraphValueId, ContentKey>& produced)
    -> ContentKey;

/**
 * @brief Content identity of an all-disabled nonempty Mask list (zero coverage).
 *
 * Distinct from an empty list, which mixes no Mask key into the Grade.
 */
[[nodiscard]] auto AllDisabledMaskUnionKey() -> ContentKey;

/**
 * @brief Build layer keys for the current document, prepared source, and bound geometry.
 *
 * @pre @p plan.geometry is the per-frame ResolvedRenderGeometry.
 * @param active_raster_masks Task-owned Brush overrides mixed into matching source keys.
 *        Dirty rectangles are omitted.
 */
[[nodiscard]] auto BuildFrameResultContentKeys(
    const ExecutionPlan& plan, const PreparedRawInput& input, const PipelineDocument& document,
    std::span<const ActiveRasterMaskInput> active_raster_masks = {}) -> FrameResultContentKeys;

}  // namespace alcedo
