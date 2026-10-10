//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_image_render_port.hpp"
#include "app/editor_render_coordinator.hpp"
#include "app/editor_render_intent.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/editor_session_pipeline_port.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
class ImagePoolService;
}

namespace alcedo::ui {

using EditorSessionFrameSinkResolver = std::function<alcedo::IFrameSink*()>;

struct EditorSessionSchedulerServices {
  /// Resolve the image pool used for render input acquisition at context bind.
  /// Hot interactive frames must not call this after a successful payload load.
  std::function<std::shared_ptr<alcedo::ImagePoolService>()> image_pool;
};

/// Stable render inputs for the currently open/switched editor image.
/// Bound at open/switch (identity immediately; image/buffer lazy-once).
/// Image switch replaces the whole context under the new epoch. The graph to render is not part
/// of the context: each frame renders the preview snapshot published at dispatch.
struct EditorRenderSessionContext {
  std::uint64_t                        epoch                = 0;
  sl_element_id_t                      element_id           = 0;
  image_id_t                           image_id             = 0;
  /// Presentation sink identity stamped at open/switch. Live `IFrameSink*` is
  /// resolved only at pipeline submit; this id is the session-scoped identity.
  alcedo::PresentationSinkId           presentation_sink_id = 0;
  std::shared_ptr<alcedo::Image>       image;
  std::shared_ptr<alcedo::ImageBuffer> input;
};

/**
 * @brief Render description for one editor viewport request.
 *
 * Maps the intent to render type, viewport region, and frame metadata. Sets
 * `document_geometry_ = RotatedUncroppedSource` exactly when `intent.geometry_overlay_only` is true
 * (Geometry panel open); otherwise the document crop and rotation apply. Pure value; reads no
 * pipeline state.
 */
[[nodiscard]] auto MakeEditorRenderDesc(const alcedo::EditorRenderRequest& request)
    -> alcedo::RenderDesc;

/**
 * @brief The editor's render adapter and the owner of the editor's only executor.
 *
 * Builds a PipelineTask from the bound session context and the preview snapshot the editor
 * history published last, and hands it to the editor's PipelineScheduler(1). No second request
 * queue: the coordinator owns single-flight; PipelineScheduler owns execution. Completion is
 * forward-only via the Schedule `on_complete` callback.
 *
 * Executor: one Interactive PipelineExecutor, created on the first frame with the accelerator
 * preference of the pipeline service and kept for the life of this port. No other module renders
 * on it and nothing shares its render lock. A frame of another image (another lineage) releases
 * every resource of the previous binding before it renders (executor binding rule);
 * @ref ClearSessionContext releases them when the editor closes the image. Frame sink: the
 * viewport sink resolved at submit is attached on the worker before the frame renders.
 *
 * Image jobs (@ref ScheduleImages): one-shot host images of the held image, rendered on the same
 * executor and worker as the frames. A job is one work item that holds the executor for all of
 * its documents, so no frame runs between A and B. Its requests have no sink, so they never
 * present; the attached viewport sink stays attached for the next frame.
 */
class EditorSessionRenderSchedulerPort final : public alcedo::IEditorPipelineSchedulerPort,
                                               public alcedo::IEditorImageRenderPort {
 public:
  explicit EditorSessionRenderSchedulerPort(
      std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler = nullptr);
  ~EditorSessionRenderSchedulerPort() override;

  /**
   * @brief Reject new frames and image jobs, cancel the in-flight frame and the accepted image
   *        job, and wait until both complete.
   *
   * The in-flight frame presents to the viewport sink resolved at submit, and both capture this
   * port in their completions. Call this before the sink owners (the editor session controller
   * and the QML viewport) are destroyed. A GUI-thread caller keeps delivering events while it
   * waits. Idempotent; the destructor calls it.
   */
  void Shutdown();

  void SetSinkResolver(EditorSessionFrameSinkResolver resolver);
  void SetPipelinePort(std::shared_ptr<EditorSessionPipelinePort> pipeline_port);
  void SetServices(EditorSessionSchedulerServices services);

  /// Bind identity for the open/switched image. Replaces any prior context.
  /// Image/buffer load once on first production frame for this bind.
  void BindSessionContext(std::uint64_t epoch, sl_element_id_t element_id, image_id_t image_id,
                          alcedo::PresentationSinkId presentation_sink_id = 0) override;
  /// Drop the bound context and release the executor's binding after the in-flight frame.
  void ClearSessionContext() override;
  /// Install a fully populated context (tests / preloaded open path).
  void InstallSessionContext(EditorRenderSessionContext context);

  auto Schedule(const alcedo::EditorRenderRequest& request,
                alcedo::EditorPipelineScheduleCompletion on_complete = {})
      -> std::uint64_t override;
  void               Cancel(std::uint64_t scheduler_job_id) override;

  /**
   * @brief Render the documents of @p request to host pixels on the editor executor, in order.
   *
   * Checks on the caller thread that the bound context is the requested image, that the editor
   * still holds it, and that every snapshot has the binding (lineage and element) of its current
   * preview, so the executor keeps the prepared source and sensor result of the image. Then
   * queues one work item on the editor worker. That item holds the executor render lock for the
   * whole job and renders each document with the full-image Quality Base request
   * (MakeQualityBaseApplyRequest) and the request geometry: no sink, host output, no scope
   * update. Each image is downloaded before the next document starts.
   *
   * Completion runs once on the worker after the render lock is released: Completed with every
   * image, Failed with the real error and no image, or Cancelled with no image.
   */
  auto               ScheduleImages(alcedo::EditorImageRenderRequest    request,
                                    alcedo::EditorImageRenderCompletion on_complete, std::string* error)
      -> std::uint64_t override;
  void               CancelImages(std::uint64_t job_id) override;
  [[nodiscard]] auto HasImageJob() const -> bool override;
  [[nodiscard]] auto last_scheduled() const -> std::vector<alcedo::EditorRenderRequest>;
  /// The editor's executor, or null before the first frame. Diagnostics and tests only.
  [[nodiscard]] auto interactive_executor() const -> std::shared_ptr<alcedo::PipelineExecutor>;
  /// Snapshot of the bound context identity and payload presence (for tests).
  [[nodiscard]] auto session_context() const -> std::optional<EditorRenderSessionContext>;
  /// Times image-pool resolution ran to load context payload (bind/hot-path).
  [[nodiscard]] auto context_payload_load_count() const -> std::uint64_t;
  /// Times the live sink pointer was resolved at submit (not at bind).
  [[nodiscard]] auto sink_resolve_count() const -> std::uint64_t;

 private:
  struct Job {
    std::uint64_t                              job_id = 0;
    alcedo::EditorRenderRequest                request{};
    alcedo::EditorPipelineScheduleCompletion   on_complete;
    bool                                       cancelled = false;
  };

  /// The accepted image job; the worker clears it when the job completes.
  struct ImageJob {
    std::uint64_t job_id    = 0;
    bool          cancelled = false;
  };

  [[nodiscard]] auto CanProduceFrame(const alcedo::EditorRenderRequest& request) const -> bool;
  [[nodiscard]] auto EnsurePipelineScheduler() -> std::shared_ptr<alcedo::PipelineScheduler>;
  /// Create the Interactive executor on first use with the pipeline service's accelerator
  /// preference.
  [[nodiscard]] auto EnsureExecutor() -> std::shared_ptr<alcedo::PipelineExecutor>;
  /// Ensure context identity matches the request and payload is loaded once.
  [[nodiscard]] auto EnsureContextForRequest(const alcedo::EditorRenderRequest& request,
                                             std::string* error)
      -> std::optional<EditorRenderSessionContext>;
  /// Ensure the context has this identity and its payload is loaded once. A context of another
  /// identity is replaced; @p presentation_sink_id stamps the replacement.
  [[nodiscard]] auto        EnsureContext(std::uint64_t epoch, sl_element_id_t element_id,
                                          image_id_t                 image_id,
                                          alcedo::PresentationSinkId presentation_sink_id,
                                          std::string* error) -> std::optional<EditorRenderSessionContext>;
  [[nodiscard]] static auto ContextMatches(const EditorRenderSessionContext& context,
                                           std::uint64_t epoch, sl_element_id_t element_id,
                                           image_id_t image_id) -> bool;
  [[nodiscard]] auto ContextMatchesRequest(const EditorRenderSessionContext& context,
                                           const alcedo::EditorRenderRequest& request) const
      -> bool;
  [[nodiscard]] auto ContextPayloadReady(const EditorRenderSessionContext& context) const -> bool;
  void               DispatchJob(Job job);
  void               DispatchPipelineFrame(Job job, alcedo::IFrameSink* sink);
  void               FinishJob(const Job& job, bool success, std::string message);
  void CompleteJob(const alcedo::EditorRenderRequest& request, bool success, std::string message,
                   alcedo::EditorPipelineScheduleCompletion on_complete);
  [[nodiscard]] auto JobIsCancelled(const Job& job) const -> bool;
  /// Worker body of one image job: render each document under the executor render lock.
  void RunImageJob(std::uint64_t job_id, const std::shared_ptr<alcedo::PipelineExecutor>& executor,
                   const std::shared_ptr<alcedo::ImageBuffer>& input,
                   const alcedo::EditorImageRenderRequest&     request,
                   alcedo::EditorImageRenderCompletion         on_complete);
  /// Run the completion of the image job, then clear it; a cancelled job publishes nothing.
  void FinishImageJob(std::uint64_t job_id, alcedo::EditorImageRenderResult result,
                      const alcedo::EditorImageRenderCompletion& on_complete);
  [[nodiscard]] auto ImageJobIsCancelled(std::uint64_t job_id) const -> bool;

  std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler_;
  std::shared_ptr<alcedo::PipelineExecutor>  executor_;
  EditorSessionFrameSinkResolver             sink_resolver_;
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port_;
  EditorSessionSchedulerServices             services_{};
  mutable std::mutex                         mutex_;
  std::condition_variable                    jobs_changed_;
  std::uint64_t                              next_job_id_ = 0;
  std::optional<Job>                         running_job_;
  std::uint64_t                              next_image_job_id_ = 0;
  std::optional<ImageJob>                    image_job_;
  std::vector<alcedo::EditorRenderRequest>   scheduled_;
  std::optional<EditorRenderSessionContext>  session_context_;
  std::uint64_t                              context_payload_load_count_ = 0;
  std::uint64_t                              sink_resolve_count_         = 0;
  bool                                       shutting_down_              = false;
};

}  // namespace alcedo::ui
