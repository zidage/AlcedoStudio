//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "app/editor_session_request_ids.hpp"
#include "edit/geometry/render_request.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/pipeline/rendered_pipeline_image.hpp"
#include "edit/runtime/pipeline_apply_request.hpp"
#include "type/type.hpp"

/**
 * @file
 * @brief Application seam for one-shot host images of the image the editor holds.
 *
 * The editor comparison renders its two inputs through this seam. The implementation renders on
 * the editor's own executor and worker; callers never see the executor, the GPU backend, or the
 * viewport sink.
 */
namespace alcedo {

/// Most documents that one image job renders: a single image, or a pair rendered consecutively.
inline constexpr std::size_t kMaxEditorImagesPerJob = 2;

/**
 * @brief Documents of the held image to render to host pixels, in order.
 *
 * Every snapshot must be a preview or derivative of the loaded image (its lineage and element),
 * so the editor executor keeps its binding and reuses the image's prepared source and sensor
 * result. The job renders them one after another on the editor worker and never presents them.
 */
struct EditorImageRenderRequest {
  /// Image the editor holds; the job renders its already bound encoded input.
  sl_element_id_t                                           element_id = 0;
  image_id_t                                                image_id   = 0;
  /// Open/switch identity the render context of the image was bound with.
  ImageLoadRequestId                                        image_load_request_id{};
  /// One document, or two (A, then B). Immutable; the caller keeps no write access to them.
  std::vector<std::shared_ptr<const PipelineGraphSnapshot>> snapshots;
  /// Output geometry of every image. Default: the editor's full-image Quality Base geometry
  /// (whole edit space, at most kQualityBaseMaxLongEdge pixels, Preview resampling).
  RenderRequest                                             geometry =
      MakeQualityBaseApplyRequest({}, DocumentGeometryUse::ApplyCropAndRotation).geometry;
};

/// Terminal state of one image job.
enum class EditorImageRenderStatus : std::uint8_t {
  /// Every document rendered; the result holds one image per snapshot.
  Completed,
  /// A render or download failed; the result holds no image and the real error.
  Failed,
  /// CancelImages or port shutdown ended the job; the result holds no image.
  Cancelled,
};

/**
 * @brief Outcome of one image job. Images are published only all together.
 */
struct EditorImageRenderResult {
  EditorImageRenderStatus            status = EditorImageRenderStatus::Failed;
  /// Failure or cancellation reason; empty when Completed.
  std::string                        message;
  /// One image per requested snapshot, in request order, when Completed; empty otherwise.
  std::vector<RenderedPipelineImage> images;
};

/// Completion of one image job. Runs once, on the editor render worker, after the executor is
/// released; the receiver moves the result to its own owner thread. The job stays accepted until
/// the completion returns, so a ScheduleImages call from inside it is rejected.
using EditorImageRenderCompletion = std::function<void(EditorImageRenderResult result)>;

/**
 * @brief Render one image, or a pair consecutively, of the held image on the editor executor.
 *
 * One image job is accepted at a time. Normal viewport frames keep their own single-flight
 * scheduling; the worker runs frames and the image job in submission order and never between A
 * and B of one job.
 */
class IEditorImageRenderPort {
 public:
  virtual ~IEditorImageRenderPort() = default;

  /**
   * @brief Accept @p request and render it later on the editor worker.
   * @return Job id, or 0 with @p error set when the request is empty, has more than
   *         kMaxEditorImagesPerJob snapshots, another image job is accepted, or the port is
   *         shutting down. A rejected request never calls @p on_complete.
   */
  virtual auto ScheduleImages(EditorImageRenderRequest    request,
                              EditorImageRenderCompletion on_complete, std::string* error)
      -> std::uint64_t                            = 0;

  /**
   * @brief Cancel the accepted image job @p job_id. Unknown or finished ids are ignored.
   *
   * A document whose render has not started is skipped; a render already running finishes, and
   * its pixels are dropped. The completion still runs, with status Cancelled.
   */
  virtual void CancelImages(std::uint64_t job_id) = 0;
};

}  // namespace alcedo
