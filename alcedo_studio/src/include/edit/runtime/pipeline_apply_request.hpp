//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "edit/geometry/render_request.hpp"
#include "edit/runtime/executor_role.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

/**
 * @brief One product DAG Apply/Render invocation. Owned by the task, not the executor.
 *
 * Role, geometry, decode, host output, sink, submission, and optional export
 * encoding are inputs for this run. The pipeline graph is not part of the request; the
 * caller passes an immutable snapshot next to it. Apply must
 * not copy these onto long-lived executor members or restore them from JSON.
 * Lifetime: built under the render lock, consumed by Apply, then discarded. Thread:
 * owner render thread. Failure: invalid combinations throw from Apply/Render; they
 * do not leave a partial mode switch on the executor.
 */
struct PipelineApplyRequest {
  RenderRequest                               geometry{};
  DecodeRes                                   decode_res           = DecodeRes::FULL;
  /// Selects the renderer that runs this request; it must match that renderer's role.
  ExecutorRole                                role                 = ExecutorRole::Interactive;
  bool                                        require_host_output  = false;
  IFrameSink*                                 sink                 = nullptr;
  FrameCompletionSubmission                   submission{};
  std::optional<ExportColorProfileConfig>     output_color;
  std::function<bool()>                       cancel_requested;
};

}  // namespace alcedo
