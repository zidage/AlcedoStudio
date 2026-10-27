//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/mask_coverage_display.hpp"
#include "edit/runtime/mask_coverage_probe.hpp"
#include "ui/edit_viewer/mask_coverage_sink.hpp"

namespace alcedo {

/**
 * @brief The probe of one render and the display target that the plan executor wrote for it.
 *
 * Created by the caller of PlanExecutor::Execute with @ref probe set. When the executor binds the
 * `effective_output` of the probed Mask (executed or a valid result), it records
 * RecordMaskValueDisplay into a coverage display target of the render extent and sets @ref target.
 * @ref target stays null when the render does not evaluate the probed Mask.
 *
 * Owner: the render call. Lifetime: one render; @ref target is valid until the next display
 * target acquisition of the workspace (MaskCoverageDisplayTargets::Acquire).
 */
template <class Backend>
struct MaskCoverageProbeRecord {
  MaskCoverageProbe                   probe;
  MaskCoverageDisplayTarget<Backend>* target = nullptr;
};

/**
 * @brief Publish the coverage of a probed render to @p sink after the render ended and its photo
 *        frame was presented.
 *
 * With a written target: FramePresenter<Backend>::PresentMaskCoverage with @p presentation (the
 * identity and the geometry of the presented photo frame). Without one: IMaskCoverageSink::
 * ClearCoverage, so the viewport shows no coverage of an older render.
 *
 * @pre The recorded work of the render is submitted (EndRender). Thread: the render thread.
 * @throws std::runtime_error from the presenter (no writable target, a suspended viewport, a
 *         failed copy). The photo frame and the photo results are not changed by the failure.
 */
template <class Backend, class Device>
auto PublishMaskCoverageProbe(Device& device, const MaskCoverageProbeRecord<Backend>& record,
                              const MaskCoveragePresentation& presentation, IMaskCoverageSink& sink)
    -> MaskCoverageProbeOutcome {
  if (record.target == nullptr) {
    sink.ClearCoverage();
    return MaskCoverageProbeOutcome::NotEvaluated;
  }
  const auto publication =
      FramePresenter<Backend>::PresentMaskCoverage(device, *record.target, presentation, sink);
  return publication == MaskCoveragePublication::Published
             ? MaskCoverageProbeOutcome::Published
             : MaskCoverageProbeOutcome::DiscardedForImageChange;
}

}  // namespace alcedo
