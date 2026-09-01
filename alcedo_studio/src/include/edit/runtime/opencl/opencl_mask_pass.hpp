//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>
#include <span>

#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_store.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

struct OpenClMaskResult {
  GraphValueId  output;
  std::uint64_t persistent_texture_resource_id = 0;
  std::uint64_t signed_distance_resource_id    = 0;
  std::uint32_t mip_level_count                = 0;
  std::uint32_t transient_bytes                = 0;
};

/**
 * @brief Evaluate @p compiled_grade's analytic or raster mask into a RenderSpace R8 image.
 *
 * Raster source levels are owned by the workspace mask cache. Feathering uses an exact
 * signed Euclidean distance field whose node-buffer metadata omits the feather radius, so
 * changing only that radius reuses the distance result. The function only enqueues OpenCL
 * work; failures throw and no CPU or alternate-backend substitute is used.
 */
[[nodiscard]] auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                     const PipelineDocument& document,
                                     const CompiledGradeNode& compiled_grade,
                                     MaskStore* store = nullptr,
                                     std::span<const RectI> dirty_rectangles = {})
    -> OpenClMaskResult;

/**
 * @brief Evaluate every compiled Color Grade mask in backbone order.
 *
 * @return The last mask result. @throws std::runtime_error when no compiled Grade has a mask.
 */
[[nodiscard]] auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                                     const PipelineDocument& document, MaskStore* store = nullptr,
                                     std::span<const RectI> dirty_rectangles = {})
    -> OpenClMaskResult;

}  // namespace alcedo

#endif  // HAVE_OPENCL
