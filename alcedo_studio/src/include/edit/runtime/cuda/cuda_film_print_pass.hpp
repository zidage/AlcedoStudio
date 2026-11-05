//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
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
 * Steps FP1 to FP6 of the Film Print DRT Plan. The entry has the ACES reference gamut compression,
 * except on Negative Scan. Resolves the tables of the selection on this thread, so a new selection
 * takes about 0.5 s here. Does not wait for the GPU, unless @p pass_times is set or Negative Scan
 * has auto colour.
 *
 * A colour reversal film has the light table form of FP6: the light table in place of the print
 * table, then the loupe flare and the surround in linear display light. Print material Negative
 * Scan (issue 664) has the scan form of FP6, with no Enlarger flare: the scanner table of the
 * scanner of the document in place of the print table, the point form `FilmKernelNegativeScanPixel`
 * to linear AP1, and then the OpenDRT of the document with its settings and its encoding, through
 * the arithmetic of method OpenDRT. With auto colour on, the prescan of the scan density
 * (`FilmScanPrescanResultId`) is reduced on the GPU, the host reads it and computes the shift of
 * rule 'frame_auto_colour', and the display kernel applies it; with auto colour off, no prescan
 * runs and nothing is read.
 *
 * Halation and Enlarger flare read the canonical results of `FilmHalationResultId` and
 * `FilmFlareResultId`, and the loupe flare of a reversal film reads `FilmLoupeFlareResultId`: a
 * render that covers the full edit space builds and publishes them, and a
 * viewport ROI render samples the published images. An ROI with no valid published image builds a
 * temporary image from its own pixels and publishes nothing. The prescan of Negative Scan follows
 * the same rule.
 *
 * @param raw_cct The colour temperature of the RAW decode for scene light As Shot, or none for an
 *        input with no RAW white balance. See `FilmSceneRawCct`.
 * @param pass_times When set, this function adds the time of each kernel group and waits for the
 *        GPU. The render path passes null.
 * @param separate_narrow_spread True runs the narrow Gaussian of FP2 as its own two passes, as the
 *        render does when that Gaussian is too wide for one dispatch. A test compares both forms.
 * @throws std::runtime_error for an output that the film print does not support, for a missing or
 *         damaged resource, for an upload or an allocation failure, for a blur beyond the tap
 *         limit, and on Negative Scan for a scanner outside the lab limits, a black at or above the
 *         grey (`film_negative_scan::MakeRenderTerms`), or a prescan with too few valid pixels.
 *         Nothing is published then; the caller cancels the render.
 */
void RecordCudaFilmPrint(CudaRenderDevice& device, const ExecutionPlan& plan,
                         const DrtNodeModel& drt, std::optional<double> raw_cct,
                         const FrameSceneBinding& scene, const FrameSceneBinding& display,
                         std::uint32_t width, std::uint32_t height,
                         std::vector<FilmPrintPassTime>* pass_times             = nullptr,
                         bool                            separate_narrow_spread = false);

}  // namespace alcedo
