//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include "decoders/processor/nn/metal_demosaicnet_cache.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

/**
 * @brief Encode SensorDevelop into `develop.sensor_linear` on the current Metal command buffer.
 *
 * Must be called between BeginRender and EndRender. Failures throw; there is no CPU or
 * RawProcessor product-path substitute. Output is camera scene-linear RGBA32F.
 */
void ExecuteMetalDevelop(MetalRenderDevice& device, const ExecutionPlan& plan,
                         const PreparedRawInput& input, PipelineDocument& document);

void ExecuteMetalGeometryResample(MetalRenderDevice& device, const ExecutionPlan& plan);

void ExecuteMetalCameraColor(MetalRenderDevice& device, const ExecutionPlan& plan,
                             const PipelineDocument& document);

void WarmUpMetalDagPlan(MetalBackend& backend, const ExecutionPlan& plan);

void SetMetalDevelopNeuralModelCacheForTesting(MetalDemosaicNetModelCache* cache);

}  // namespace alcedo

#endif
