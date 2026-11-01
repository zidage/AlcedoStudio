//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"

namespace alcedo {

/// GPU time of one group of kernels of the film print, for the cost record of the plan.
struct FilmPrintPassTime {
  std::string name;
  float       milliseconds = 0.0f;
};

/**
 * @brief Record the film print display transform of @p drt: @p scene (linear AP1) to @p display.
 *
 * Steps FP1 to FP6 of the Film Print DRT Plan. The entry has no reference gamut compression.
 * Resolves the tables of the selection on this thread, so a new selection takes about 0.5 s here.
 * Does not wait for the GPU, unless @p pass_times is set.
 *
 * Halation and Enlarger flare read the canonical results of `FilmHalationResultId` and
 * `FilmFlareResultId`: a render that covers the full edit space builds and publishes them, and a
 * viewport ROI render samples the published images. An ROI with no valid published image builds a
 * temporary image from its own pixels and publishes nothing.
 *
 * @param pass_times When set, this function adds the time of each kernel group and waits for the
 *        GPU. The render path passes null.
 * @throws std::runtime_error for an output that the film print does not support, for a missing or
 *         damaged resource, for an upload or an allocation failure, and for a blur beyond the
 *         tap limit. Nothing is published then; the caller cancels the render.
 */
void RecordCudaFilmPrint(CudaRenderDevice& device, const ExecutionPlan& plan,
                         const DrtNodeModel& drt, const FrameSceneBinding& scene,
                         const FrameSceneBinding& display, std::uint32_t width,
                         std::uint32_t                   height,
                         std::vector<FilmPrintPassTime>* pass_times = nullptr);

}  // namespace alcedo
