//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Encode SensorDevelop into `develop.sensor_linear` for the current submission.
 *
 * Must be called between BeginRender and EndRender. Failures throw; there is
 * no CPU Apply fallback. Output is camera scene-linear RGBA32F before geometry.
 * Source host bytes are uploaded only from this pass.
 *
 * CUDA order: Linearize → (optional CFA Clamp01) → Demosaic → HighlightRecover
 * on RGB when enabled. Geometry and CameraColor are separate passes.
 */
void ExecuteCudaDevelop(CudaRenderDevice& device, const ExecutionPlan& plan,
                        const PreparedRawInput& input, PipelineDocument& document);

/**
 * @brief Write `geometry.scene_source` from `develop.sensor_linear`.
 *
 * Identity geometry copies the sensor texture. Non-identity runs GeometryResamplePass.
 */
void ExecuteCudaGeometryResample(CudaRenderDevice& device, const ExecutionPlan& plan);

/**
 * @brief Write `develop.image` from `geometry.scene_source` using the current camera matrix.
 *
 * G7R.3 replaces the matrix construction. This pass is independently skippable.
 */
void ExecuteCudaCameraColor(CudaRenderDevice& device, const ExecutionPlan& plan,
                            const RawRuntimeColorContext& color_context,
                            const PipelineDocument&       document);

}  // namespace alcedo
