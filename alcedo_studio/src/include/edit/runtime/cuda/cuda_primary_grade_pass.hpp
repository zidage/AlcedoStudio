//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "decoders/processor/raw_color_context.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

struct CudaPrimaryGradeResult {
  GraphValueId  output{NodeId{"grade.primary"}, PortId{"image"}};
  std::uint64_t local_tone_reference_resource_id = 0;
};

/**
 * @brief Execute the serialized primary-grade Model order in the AP1/ACEScc working space.
 *
 * CameraColor produces the encoded graph input. This pass keeps every intermediate and its graph
 * output in AP1/ACEScc, including mix, mask application, and the local-Laplacian tone stage.
 *
 * Must run between CudaRenderDevice::BeginRender and EndRender. Parameters, output images,
 * execution order, and local-tone reference data are owned by the device workspace. A failed
 * parameter transfer restores the affected Model dirty bits. No CPU image-processing fallback.
 */
[[nodiscard]] auto ExecuteCudaPrimaryGrade(CudaRenderDevice& device, const ExecutionPlan& plan,
                                           const RawRuntimeColorContext& color_context,
                                           PipelineDocument& document) -> CudaPrimaryGradeResult;

}  // namespace alcedo
