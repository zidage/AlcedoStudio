//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include "decoders/processor/nn/opencl_demosaicnet_cache.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

/**
 * @brief Encode SensorDevelop into `develop.sensor_linear` on the product queue.
 *
 * Must be called between BeginRender and EndRender. Failures throw; there is no CPU,
 * CUDA, Metal, or RawProcessor product-path substitute. Output is camera scene-linear
 * RGBA32F.
 */
void ExecuteOpenClDevelop(OpenClRenderDevice& device, const ExecutionPlan& plan,
                          const PreparedRawInput& input, PipelineDocument& document);

void ExecuteOpenClGeometryResample(OpenClRenderDevice& device, const ExecutionPlan& plan);

void ExecuteOpenClCameraColor(OpenClRenderDevice& device, const ExecutionPlan& plan,
                              PipelineDocument& document);

/**
 * @brief Test hook: inject a model cache so Neural load failure can be asserted.
 *
 * Null restores the process-wide cache. Not thread-safe.
 */
void SetOpenClDevelopNeuralModelCacheForTesting(OpenClDemosaicNetModelCache* cache);

}  // namespace alcedo

#endif
