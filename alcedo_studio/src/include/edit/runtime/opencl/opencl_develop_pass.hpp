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
 * CUDA, or Metal product-path substitute. Output is camera scene-linear RGBA32F.
 */
void ExecuteOpenClDevelop(OpenClRenderDevice& device, const ExecutionPlan& plan,
                          const PreparedRawInput& input, const PipelineDocument& document);

void ExecuteOpenClGeometryResample(OpenClRenderDevice& device, const ExecutionPlan& plan);

void ExecuteOpenClCameraColor(OpenClRenderDevice& device, const ExecutionPlan& plan,
                              const PipelineDocument& document);

/**
 * @brief Raster input: write ACEScc AP1 `develop.image` from `geometry.scene_source` with the
 * DisplayToAp1 kernel. The parameter block is bound through the workspace parameter arena.
 * @throws std::runtime_error when the document has no raster input object or the pixels do not
 *         match its description.
 */
void ExecuteOpenClDisplayToAp1(OpenClRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& input, const PipelineDocument& document);

/**
 * @brief Test hook: inject a model cache so Neural load failure can be asserted.
 *
 * Null restores the process-wide cache. Not thread-safe.
 */
void SetOpenClDevelopNeuralModelCacheForTesting(OpenClDemosaicNetModelCache* cache);

}  // namespace alcedo

#endif
