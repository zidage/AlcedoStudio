//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

struct CudaMaskResult {
  GraphValueId output;
};

/**
 * @brief Evaluate @p compiled_source into its effective GraphValueId (RenderSpace R8).
 *
 * Analytic sources evaluate in the native kernel. Invert and opacity apply in
 * that order. No CPU image processing fallback is used.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document,
                                   const CompiledGradeNode& compiled_grade,
                                   const CompiledMaskSource& compiled_source) -> CudaMaskResult;

/**
 * @brief Maximum-Union enabled Mask sources into the Grade Union output.
 *
 * Zero enabled sources fill zeros. One enabled source aliases the source texture.
 * Two or more fold a native R8 maximum over the full render extent so a lowered
 * source can decrease coverage. Failures throw; there is no CPU substitute.
 */
[[nodiscard]] auto ExecuteCudaMaskUnion(CudaRenderDevice& device, const ExecutionPlan& plan,
                                        const PipelineDocument& document,
                                        const CompiledGradeNode& compiled_grade) -> CudaMaskResult;

/**
 * @brief Evaluate every enabled source on @p compiled_grade and Union into mask_output.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document,
                                   const CompiledGradeNode& compiled_grade) -> CudaMaskResult;

/**
 * @brief Evaluate every compiled Color Grade mask in backbone order.
 *
 * @return The last mask result. @throws std::runtime_error when no compiled Grade has a mask.
 */
[[nodiscard]] auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                                   const PipelineDocument& document) -> CudaMaskResult;

}  // namespace alcedo
