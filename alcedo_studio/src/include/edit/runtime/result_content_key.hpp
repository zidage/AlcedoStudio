//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Content keys and extents for one product frame's GPU result cache lookups.
 *
 * Viewport is in @ref geometry_scene_source only. CCT/tint is in @ref develop_image only.
 */
struct FrameResultContentKeys {
  ContentKey  sensor_linear{};
  ContentKey  geometry_scene_source{};
  ContentKey  develop_image{};
  ContentKey  primary_grade{};
  ContentKey  drt_display{};
  ContentKey  mask{};
  ImageExtent sensor_extent{};
  ImageExtent geometry_extent{};
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
 * @brief Identity of the canonical LLF reference. Viewport ROI is omitted.
 *
 * Includes prepared source, user crop/rotation, CameraColor params, and Primary Grade
 * adjustments. Viewport, render extent, and dynamic scale are not mixed.
 */
[[nodiscard]] auto HashLlfReferenceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                                       const PipelineDocument& document) -> ContentKey;

/**
 * @brief Identity of the canonical LLF source pyramid. Viewport ROI and
 * Shadows/Highlights slider values are omitted so a slider edit can reuse it.
 */
[[nodiscard]] auto HashLlfSourceKey(const ExecutionPlan& plan, const PreparedRawInput& input,
                                    const PipelineDocument& document) -> ContentKey;

/**
 * @brief Build layer keys for the current document, prepared source, and bound geometry.
 *
 * @pre @p plan.geometry is the per-frame ResolvedRenderGeometry.
 */
[[nodiscard]] auto BuildFrameResultContentKeys(const ExecutionPlan&    plan,
                                               const PreparedRawInput& input,
                                               const PipelineDocument& document)
    -> FrameResultContentKeys;

}  // namespace alcedo
