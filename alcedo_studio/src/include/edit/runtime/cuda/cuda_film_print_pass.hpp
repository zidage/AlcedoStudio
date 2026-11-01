//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"

namespace alcedo {

/**
 * @brief Record the film print display transform of @p drt: @p scene (linear AP1) to @p display.
 *
 * Steps FP1, FP4, FP5, and FP6 of the Film Print DRT Plan, with no blur and no Enlarger flare. The
 * entry has no reference gamut compression. Resolves the tables of the selection on this thread, so
 * a new selection takes about 0.5 s here. Does not wait for the GPU.
 *
 * @throws std::runtime_error for an output that the film print does not support, for a missing or
 *         damaged resource, and for an upload failure. Nothing is published then; the caller
 * cancels the render.
 */
void RecordCudaFilmPrint(CudaRenderDevice& device, const ExecutionPlan& plan,
                         const DrtNodeModel& drt, const FrameSceneBinding& scene,
                         const FrameSceneBinding& display, std::uint32_t width,
                         std::uint32_t height);

}  // namespace alcedo
