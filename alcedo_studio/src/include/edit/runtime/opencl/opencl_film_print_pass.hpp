//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_OPENCL

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/opencl/opencl_backend.hpp"

namespace alcedo {

/**
 * @brief Record the film print display transform of @p drt: @p scene (linear AP1) to @p display.
 *
 * Steps FP1 to FP6 of the Film Print DRT Plan, with the rules of `RecordCudaFilmPrint`. The images
 * between the steps are allocations in the transient arena of the render, with 3 floats per pixel.
 * Resolves the tables of the selection on this thread. Does not wait for the GPU.
 *
 * Print material Lab Scan has its FP6 in the DRT program (`film_print_scan_display`), because it
 * applies the OpenDRT of that program to its scene value.
 *
 * @param raw_cct The colour temperature of the RAW decode for scene light As Shot, or none for an
 *        input with no RAW white balance. See `FilmSceneRawCct`.
 * @param pass_names When set, this function adds the name of each kernel group that it records.
 *        The render path passes null.
 * @param separate_narrow_spread True runs the narrow Gaussian of FP2 as its own two passes, as the
 *        render does when that Gaussian is too wide for one dispatch. A test compares both forms.
 * @throws std::runtime_error for an output that the film print does not support, for a missing or
 *         damaged resource, for an upload, an allocation, or an enqueue failure, for a program
 *         build failure, and for a blur beyond the tap limit. Nothing is published then; the caller
 *         cancels the render. There is no change of backend.
 */
void RecordOpenClFilmPrint(OpenClRenderDevice& device, const ExecutionPlan& plan,
                           const DrtNodeModel& drt, std::optional<double> raw_cct,
                           const FrameSceneBinding& scene, const FrameSceneBinding& display,
                           std::uint32_t width, std::uint32_t height,
                           std::vector<std::string>* pass_names             = nullptr,
                           bool                      separate_narrow_spread = false);

}  // namespace alcedo

#endif  // HAVE_OPENCL
