//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#ifdef HAVE_METAL

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/metal/metal_backend.hpp"

namespace alcedo {

/**
 * @brief Record the film print display transform of @p drt: @p scene (linear AP1) to @p display.
 *
 * Steps FP1 to FP6 of the Film Print DRT Plan, with the rules of `RecordCudaFilmPrint`. Every
 * dispatch goes into the serial compute encoder of the frame, and the pipeline states come from
 * `metal::ComputePipelineCache`. The images between the steps are allocations in the transient
 * arena of the render, with 3 floats per pixel. Resolves the tables of the selection on this
 * thread. Does not commit or wait, except on Negative Scan with auto colour.
 *
 * Print material Negative Scan (issue 662) has the scan form of FP6 of `RecordCudaFilmPrint`, with
 * no Enlarger flare: the scanner table of the scanner of the document, the point form
 * `FilmKernelNegativeScanPixel` to linear AP1, and the display transform of method OpenDRT of
 * drt.metal (`film_print_scan_display`, in the DRT AIR file). With auto colour on, the GPU reduces
 * the scan density of the full frame to the canonical prescan (`FilmScanPrescanResultId`), and the
 * read of the prescan commits the recorded work of the frame and waits for it; the host computes
 * the shift of rule 'frame_auto_colour'. With auto colour off, no prescan runs and nothing is read.
 *
 * @param raw_cct The colour temperature of the RAW decode for scene light As Shot, or none for an
 *        input with no RAW white balance. See `FilmSceneRawCct`.
 * @param pass_names When set, this function adds the name of each kernel group that it records.
 *        The render path passes null.
 * @param separate_narrow_spread True runs the narrow Gaussian of FP2 as its own two passes, as the
 *        render does when that Gaussian is too wide for one dispatch. A test compares both forms.
 * @throws std::runtime_error for an output that the film print does not support, for a missing or
 *         damaged resource, for an allocation failure, for a pipeline or an encoder error, for a
 *         blur beyond the tap limit, and on Negative Scan for a scanner outside the lab limits, a
 *         black at or above the grey (`film_negative_scan::MakeRenderTerms`), a prescan with too
 *         few valid pixels, or a command buffer error at the read of the prescan. Nothing is
 *         published then; the caller cancels the render.
 *         There is no change of backend.
 */
void RecordMetalFilmPrint(MetalRenderDevice& device, const ExecutionPlan& plan,
                          const DrtNodeModel& drt, std::optional<double> raw_cct,
                          const FrameSceneBinding& scene, const FrameSceneBinding& display,
                          std::uint32_t width, std::uint32_t height,
                          std::vector<std::string>* pass_names             = nullptr,
                          bool                      separate_narrow_spread = false);

}  // namespace alcedo

#endif  // HAVE_METAL
