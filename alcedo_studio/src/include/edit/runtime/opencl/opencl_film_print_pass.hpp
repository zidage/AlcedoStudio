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
 * Resolves the tables of the selection on this thread. Does not wait for the GPU, except on
 * Negative Scan with auto colour, which reads the prescan on the host.
 *
 * Print material Negative Scan (issue 664) has its FP6 in the DRT program
 * (`film_print_scan_display`), because it applies the OpenDRT of that program to its linear AP1.
 * The prescan of its auto colour is two kernels of the film print program.
 *
 * With @p grain_strength above 0, the grain of the negative (phase G6 of the plan
 * 'film_grain_format_and_stock_plan.md', the rules of phase G5 on CUDA): the FP6 kernel is
 * `film_print_display_grain` or `film_print_scan_display_grain`, with the steps of
 * film_grain_tile.h, and it adds the grain of the selected film at the film format of @p drt to the
 * record of each pixel (film_grain_negative.h), before the flare amounts and the read of the print
 * table, the light table, or the scanner table. A reversal film gets it on `dye_max - record`. The
 * blurred amounts of the negative grade, the flare canvas, and the prescan read the amounts with
 * no grain: they are means over many pixels.
 *
 * @param raw_cct The colour temperature of the RAW decode for scene light As Shot, or none for an
 *        input with no RAW white balance. See `FilmSceneRawCct`.
 * @param grain_strength The stored value of the Film Grain slider, in [0, 2]; 0 runs no grain
 *        kernel.
 * @param pass_names When set, this function adds the name of each kernel group that it records.
 *        The render path passes null.
 * @param separate_narrow_spread True runs the narrow Gaussian of FP2 as its own two passes, as the
 *        render does when that Gaussian is too wide for one dispatch. A test compares both forms.
 * @throws std::runtime_error for an output that the film print does not support, for a missing or
 *         damaged resource, for an upload, an allocation, an enqueue, or a read failure, for a
 *         program build failure, for a blur beyond the tap limit, and for the stops of Negative
 *         Scan of `RecordCudaFilmPrint`. Nothing is published then; the caller cancels the render.
 *         There is no change of backend. std::invalid_argument for a grain strength outside
 *         [0, 2] (`MakeFilmGrainPlan`), before any GPU work.
 */
void RecordOpenClFilmPrint(OpenClRenderDevice& device, const ExecutionPlan& plan,
                           const DrtNodeModel& drt, std::optional<double> raw_cct,
                           const FrameSceneBinding& scene, const FrameSceneBinding& display,
                           std::uint32_t width, std::uint32_t height, float grain_strength,
                           std::vector<std::string>* pass_names             = nullptr,
                           bool                      separate_narrow_spread = false);

}  // namespace alcedo

#endif  // HAVE_OPENCL
