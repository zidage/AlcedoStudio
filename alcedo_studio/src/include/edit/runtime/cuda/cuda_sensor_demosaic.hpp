//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include <opencv2/core/cuda.hpp>

#include "decoders/processor/nn/demosaicnet_cache.hpp"
#include "decoders/processor/raw_demosaic_method.hpp"
#include "decoders/processor/raw_processor_pattern.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/develop_demosaic.hpp"

namespace alcedo {

class CudaRenderDevice;

/**
 * @brief Test hook: inject a model cache so Neural load failure can be asserted.
 *
 * Null restores the process-wide cache. Not thread-safe.
 */
void SetDevelopNeuralModelCacheForTesting(DemosaicNetModelCache* cache);

/**
 * @brief Demosaic linearized CFA and pack camera RGB into @p packed_rgba.
 *
 * Highlight reconstruction runs on RGB when enabled. Neural Engine failures throw
 * with the engine error string; there is no Legacy fallback.
 */
void ExecuteCudaSensorDemosaicAndPack(CudaRenderDevice& device, const PreparedRawInput& input,
                                      const DevelopPayload& params, cv::cuda::GpuMat linear,
                                      cv::cuda::GpuMat packed, cv::cuda::Stream& stream);

}  // namespace alcedo
