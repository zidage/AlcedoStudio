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
                        const PreparedRawInput& input, const PipelineDocument& document);

/**
 * @brief Write `geometry.scene_source` from `develop.sensor_linear`.
 *
 * Identity geometry aliases the sensor texture. Non-identity runs GeometryResamplePass.
 */
void ExecuteCudaGeometryResample(CudaRenderDevice& device, const ExecutionPlan& plan);

/**
 * @brief Write AP1/ACEScc `develop.image` from camera-linear `geometry.scene_source`.
 *
 * Interpolates the Develop camera-profile matrices on the CPU, applies camera→AP1 in scene-linear,
 * then encodes AP1 as the graph working space. Missing or singular matrices throw; identity is
 * never substituted. Independently skippable from SensorDevelop and Geometry.
 */
void ExecuteCudaCameraColor(CudaRenderDevice& device, const ExecutionPlan& plan,
                            const PipelineDocument& document);

/**
 * @brief Raster input: write ACEScc AP1 `develop.image` from `geometry.scene_source`.
 *
 * Display-referred input goes through the cached OpenColorIO ACES 2.0 inverse for the effective
 * source description; scene-linear input through the source-to-AP1 matrix. The parameter block
 * is uploaded only when it changes.
 * @throws std::runtime_error when the document has no raster input object or the pixels do not
 *         match its description.
 */
void ExecuteCudaDisplayToAp1(CudaRenderDevice& device, const ExecutionPlan& plan,
                             const PreparedRawInput& input, const PipelineDocument& document);

}  // namespace alcedo
