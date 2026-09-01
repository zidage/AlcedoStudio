//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "edit/geometry/render_request.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

/**
 * @brief One product DAG Apply/Render invocation. Owned by the task, not the executor.
 *
 * Geometry, decode, cache policy, host output, sink, submission, optional export
 * encoding, and optional active Brush rasters are inputs for this run. Apply must
 * not copy these onto long-lived executor members or restore them from JSON.
 * Lifetime: built under the render lock, consumed by Apply, then discarded. Thread:
 * owner render thread. Failure: invalid combinations throw from Apply/Render; they
 * do not leave a partial mode switch on the executor.
 */
struct PipelineApplyRequest {
  RenderRequest                               geometry{};
  DecodeRes                                   decode_res           = DecodeRes::FULL;
  RenderCachePolicy                           cache_policy         = RenderCachePolicy::UseSessionCache;
  bool                                        require_host_output  = false;
  IFrameSink*                                 sink                 = nullptr;
  FrameCompletionSubmission                   submission{};
  std::optional<ExportColorProfileConfig>     output_color;
  std::function<bool()>                       cancel_requested;
  std::vector<ActiveRasterMaskInput>          active_raster_masks;
  bool                                        allow_active_raster_preview = false;
};

}  // namespace alcedo
