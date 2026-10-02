//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "edit/geometry/render_request.hpp"
#include "edit/runtime/executor_role.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {

struct PreparedRawInput;

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
  /// Batch only: the source the owner already decoded at decode_res, off the render lock. The
  /// renderer uses it in place of the encoded bytes; null means the renderer decodes them.
  std::shared_ptr<const PreparedRawInput>     prepared_input;
};

/// Long-edge limit, in output pixels, of the editor's full-image Quality Base render.
inline constexpr std::uint32_t kQualityBaseMaxLongEdge = 4096;

/**
 * @brief Full-image Quality Base request for the editor's Interactive executor.
 *
 * FULL decode, the whole edit space resampled with Preview quality to at most
 * @ref kQualityBaseMaxLongEdge pixels on the long edge, and frame role QualityBase, so the render
 * reuses and keeps only the sensor result (ResultPersistenceScope::SensorDevelopOnly). The
 * editor viewport's Quality Base frame and the editor's one-shot host images both start from this
 * request; neither defines its own quality values.
 *
 * Pure value. The result has no sink, no host output, and no cancel callback; the caller sets
 * those for its use.
 *
 * @param metadata Frame metadata of the caller; frame role and source ROI are replaced.
 * @param document_geometry How the render reads the document crop and rotation.
 */
[[nodiscard]] inline auto      MakeQualityBaseApplyRequest(FramePreviewMetadata metadata,
                                                           DocumentGeometryUse  document_geometry)
    -> PipelineApplyRequest {
  metadata.frame_role      = FrameRole::QualityBase;
  metadata.source_roi_norm = {};
  PipelineApplyRequest request;
  request.geometry.resolution.max_edge = kQualityBaseMaxLongEdge;
  request.geometry.resolution.quality  = RenderQuality::Preview;
  request.geometry.document_geometry   = document_geometry;
  request.decode_res                   = DecodeRes::FULL;
  request.role                         = ExecutorRole::Interactive;
  request.submission                   = FrameCompletionSubmission{
                        .metadata = metadata, .mode = FramePresentationMode::ViewportTransformed};
  return request;
}

}  // namespace alcedo
