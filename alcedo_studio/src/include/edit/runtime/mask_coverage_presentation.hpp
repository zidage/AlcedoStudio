//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <type_traits>
#include <variant>

#include "edit/geometry/types.hpp"
#include "edit/mask/ai_mask_recipe.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/frame_presenter.hpp"
#include "edit/runtime/mask_coverage_display.hpp"
#include "ui/edit_viewer/mask_coverage_sink.hpp"

namespace alcedo {

/// The AI Mask candidate of the edit session: its shared original coverage and edge values.
struct MaskCoverageCandidateSelection {
  AiMaskCoveragePixels pixels{};
  Extent2D             analysis_extent{};
  AiMaskEdgeAdjustment edge{};
};

/// The coverage that one presentation displays: the candidate or one committed AI Mask.
using MaskCoverageSelection =
    std::variant<MaskCoverageCandidateSelection, AiSegmentationMaskSource>;

/**
 * @brief Coverage-only presentation (SM8 C4, C5): write the processed coverage of @p selection on
 *        the photo frame grid of @p request and hand it to the coverage channel @p sink.
 *
 * Runs coverage-only work on the device submission path: the coverage acquisition of the
 * workspace (original upload only on a miss, edge work only for a new edge value), one raster
 * sampling dispatch into an RGBA32F display target (`RecordMaskCoverageDisplay` of the backend),
 * then FramePresenter<Backend>::PresentMaskCoverage. It compiles and executes no photo DAG, reads
 * nothing back to the host, and changes no photo result or photo presentation slot.
 *
 * @pre The render thread of @p device; no render or coverage work is open.
 * @return Published, or DiscardedForImageChange when the viewport changed image first.
 * @throws std::runtime_error from the request validation, the coverage acquisition, the display
 *         write, the native target acquisition, or the transfer. Recorded work is then cancelled
 *         or completed, the channel keeps its displayed coverage, and photo results are unchanged.
 */
template <class Device>
auto PresentMaskCoverage(Device& device, const MaskCoverageSelection& selection,
                         const MaskCoverageDisplayRequest& request,
                         const MaskCoverageFrameIdentity& identity, IMaskCoverageSink& sink)
    -> MaskCoveragePublication {
  using Backend = std::remove_reference_t<decltype(device.Workspace().Device())>;
  ValidateMaskCoverageDisplayRequest(request);
  auto& workspace = device.Workspace();
  auto& context   = device.CommandContext();
  device.BeginCoverageWork();
  MaskCoverageDisplayTarget<Backend>* target = nullptr;
  try {
    const auto coverage = std::visit(
        [&](const auto& source) {
          using Source = std::decay_t<decltype(source)>;
          if constexpr (std::is_same_v<Source, MaskCoverageCandidateSelection>) {
            return workspace.AcquireAiMaskCandidateCoverage(source.pixels, source.analysis_extent,
                                                            source.edge, context);
          } else {
            return workspace.AcquireAiMaskCoverage(source, context);
          }
        },
        selection);
    target = &workspace.AcquireMaskCoverageDisplayTarget(request.geometry.render_extent, context);
    RecordMaskCoverageDisplay(device, *coverage, request, target->texture);
    device.EndCoverageWork();
  } catch (...) {
    device.CancelCoverageWork();
    throw;
  }
  return FramePresenter<Backend>::PresentMaskCoverage(
      device, *target, MaskCoveragePresentation{identity, request.geometry}, sink);
}

}  // namespace alcedo
