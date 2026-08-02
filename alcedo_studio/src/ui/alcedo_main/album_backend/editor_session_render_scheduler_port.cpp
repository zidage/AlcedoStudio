//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"

#include <QString>

#include <algorithm>
#include <chrono>
#include <future>
#include <stdexcept>
#include <utility>

#include "app/editor_adjustment_pipeline.hpp"
#include "edit/frame_presentation_types.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/image_controller.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/pipeline_controller.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::ui {
using alcedo::diag::editorPresentLog;

namespace {

auto RenderTypeForIntent(const alcedo::EditorRenderIntent& intent) -> alcedo::RenderType {
  switch (intent.quality) {
    case alcedo::EditorRenderQuality::Detail:
      return alcedo::RenderType::DETAIL_ROI_PREVIEW;
    case alcedo::EditorRenderQuality::Quality:
      return alcedo::RenderType::QUALITY_BASE_PREVIEW;
    case alcedo::EditorRenderQuality::Interactive:
      return alcedo::RenderType::FAST_PREVIEW;
  }
  return alcedo::RenderType::FAST_PREVIEW;
}

auto FrameRoleToPreviewMetadata(const alcedo::EditorRenderIntent& intent)
    -> alcedo::FramePreviewMetadata {
  alcedo::FramePreviewMetadata meta;
  meta.frame_role              = intent.frame_role;
  meta.detail_serial           = 0;
  meta.image_identity          = static_cast<std::uint64_t>(intent.image_id);
  meta.session_epoch        = intent.image_load_request_id.value;
  meta.scope_update_allowed    = alcedo::ScopeUpdateAllowedForReason(intent.reason);
  meta.scope_refresh_requested = intent.reason == alcedo::EditorRenderReason::ScopeRefresh;
  return meta;
}

void TraceDetailRequest(const char* stage, const alcedo::EditorRenderRequest& request,
                        const char* outcome = "", long long elapsed_ms = -1) {
  if (request.intent.frame_role != alcedo::FrameRole::DetailPatch) {
    return;
  }
  if (!editorPresentLog().isDebugEnabled()) {
    return;
  }
  QString msg = QStringLiteral("[ROI_TRACE][%1] request=%2 image=%3 session_epoch=%4 requested=%5x%6")
                    .arg(QLatin1String(stage))
                    .arg(request.request_id)
                    .arg(request.intent.image_id)
                    .arg(request.intent.image_load_request_id.value)
                    .arg(request.intent.requested_width)
                    .arg(request.intent.requested_height);
  if (request.intent.view_region) {
    const auto& roi = *request.intent.view_region;
    msg += QStringLiteral(" region_px=%1,%2 scale=%3,%4 reference=%5x%6 target=%7x%8")
               .arg(roi.x_)
               .arg(roi.y_)
               .arg(roi.scale_x_)
               .arg(roi.scale_y_)
               .arg(roi.reference_width_)
               .arg(roi.reference_height_)
               .arg(roi.target_width_)
               .arg(roi.target_height_);
  } else {
    msg += QStringLiteral(" region=none");
  }
  if (outcome && *outcome) {
    msg += QStringLiteral(" outcome=%1").arg(QLatin1String(outcome));
  }
  if (elapsed_ms >= 0) {
    msg += QStringLiteral(" elapsed_ms=%1").arg(elapsed_ms);
  }
  qCDebug(editorPresentLog).noquote() << msg;
}

}  // namespace

EditorSessionRenderSchedulerPort::EditorSessionRenderSchedulerPort(
    std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler)
    : pipeline_scheduler_(std::move(pipeline_scheduler)),
      worker_([this] { WorkerLoop(); }) {}

EditorSessionRenderSchedulerPort::~EditorSessionRenderSchedulerPort() {
  std::shared_ptr<alcedo::EditorRenderCancellationToken> queued_cancellation;
  std::shared_ptr<alcedo::EditorRenderCancellationToken> running_cancellation;
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    if (queued_job_) {
      queued_job_->cancelled = true;
      queued_cancellation    = queued_job_->request.intent.cancellation;
    }
    if (running_job_) {
      running_job_->cancelled = true;
      running_cancellation    = running_job_->request.intent.cancellation;
    }
    sink_resolver_ = {};
  }
  if (queued_cancellation) queued_cancellation->Cancel();
  if (running_cancellation) running_cancellation->Cancel();
  work_available_.notify_all();
  if (worker_.joinable()) worker_.join();
}

void EditorSessionRenderSchedulerPort::SetCoordinator(
    std::weak_ptr<alcedo::EditorRenderCoordinator> coordinator) {
  std::scoped_lock lock(mutex_);
  coordinator_ = std::move(coordinator);
}

void EditorSessionRenderSchedulerPort::SetSinkResolver(EditorSessionFrameSinkResolver resolver) {
  std::scoped_lock lock(mutex_);
  sink_resolver_ = std::move(resolver);
}

void EditorSessionRenderSchedulerPort::SetPipelinePort(
    std::shared_ptr<EditorSessionPipelinePort> pipeline_port) {
  std::scoped_lock lock(mutex_);
  pipeline_port_ = std::move(pipeline_port);
}

void EditorSessionRenderSchedulerPort::SetServices(EditorSessionSchedulerServices services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

void EditorSessionRenderSchedulerPort::SetTestFrameProducer(
    EditorSessionTestFrameProducer producer) {
  std::scoped_lock lock(mutex_);
  test_producer_ = std::move(producer);
}

auto EditorSessionRenderSchedulerPort::Schedule(const alcedo::EditorRenderRequest& request)
    -> std::uint64_t {
  if (!CanProduceFrame(request)) {
    TraceDetailRequest("scheduler-reject", request, "no-frame-source");
    return 0;
  }
  std::scoped_lock lock(mutex_);
  // The coordinator owns the request queue and never schedules a second job
  // until the first blocking execution completes.
  if (shutting_down_ || queued_job_ || running_job_) {
    TraceDetailRequest("scheduler-reject", request, "worker-busy-or-shutdown");
    return 0;
  }
  Job job;
  job.job_id  = ++next_job_id_;
  job.request = request;
  queued_job_ = job;
  scheduled_.push_back(request);
  TraceDetailRequest("scheduler-queued", request);
  work_available_.notify_one();
  return job.job_id;
}

auto EditorSessionRenderSchedulerPort::CanProduceFrame(
    const alcedo::EditorRenderRequest& request) const -> bool {
  EditorSessionTestFrameProducer test_producer;
  std::function<std::shared_ptr<alcedo::ImagePoolService>()> image_pool;
  {
    std::scoped_lock lock(mutex_);
    test_producer = test_producer_;
    image_pool    = services_.image_pool;
  }
  if (test_producer) return true;
  if (!image_pool) return false;
  try {
    const auto pool = image_pool();
    if (!pool) return false;
    const auto image = pool->Read<std::shared_ptr<alcedo::Image>>(
        request.intent.image_id,
        [](const std::shared_ptr<alcedo::Image>& value) { return value; });
    return image && !image->image_path_.empty();
  } catch (...) {
    return false;
  }
}

void EditorSessionRenderSchedulerPort::Cancel(std::uint64_t scheduler_job_id) {
  std::shared_ptr<alcedo::EditorRenderCancellationToken> cancellation;
  std::optional<alcedo::EditorRenderRequest>              cancelled_before_start;
  {
    std::scoped_lock lock(mutex_);
    if (queued_job_ && queued_job_->job_id == scheduler_job_id) {
      queued_job_->cancelled = true;
      cancellation          = queued_job_->request.intent.cancellation;
      cancelled_before_start = queued_job_->request;
      queued_job_.reset();
      jobs_changed_.notify_all();
    } else if (running_job_ && running_job_->job_id == scheduler_job_id) {
      running_job_->cancelled = true;
      cancellation           = running_job_->request.intent.cancellation;
    } else {
      return;
    }
  }
  if (cancellation) cancellation->Cancel();
  if (cancelled_before_start) {
    CompleteJob(*cancelled_before_start, false, "Cancelled before execution");
  }
}

void EditorSessionRenderSchedulerPort::WaitForSessionIdle(std::uint64_t session_epoch) {
  std::unique_lock lock(mutex_);
  jobs_changed_.wait(lock, [&] {
    const auto belongs_to_session = [session_epoch](const std::optional<Job>& job) {
      return job && job->request.intent.image_load_request_id.value == session_epoch;
    };
    return !belongs_to_session(queued_job_) && !belongs_to_session(running_job_);
  });
}

auto EditorSessionRenderSchedulerPort::last_scheduled() const
    -> std::vector<alcedo::EditorRenderRequest> {
  std::scoped_lock lock(mutex_);
  return scheduled_;
}

auto EditorSessionRenderSchedulerPort::IsCancelled(std::uint64_t job_id) const -> bool {
  std::scoped_lock lock(mutex_);
  if (shutting_down_) return true;
  if (!running_job_ || running_job_->job_id != job_id) return false;
  return running_job_->cancelled ||
         (running_job_->request.intent.cancellation &&
          running_job_->request.intent.cancellation->IsCancelled());
}

void EditorSessionRenderSchedulerPort::WorkerLoop() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      work_available_.wait(lock, [this] { return shutting_down_ || queued_job_.has_value(); });
      if (shutting_down_ && !queued_job_) return;
      job = std::move(*queued_job_);
      queued_job_.reset();
      running_job_ = job;
    }

    bool        success = false;
    std::string message;
    TraceDetailRequest("worker-begin", job.request);
    const auto worker_started = std::chrono::steady_clock::now();
    try {
      ExecuteJob(job);
      success = !IsCancelled(job.job_id);
      message = success ? "Frame ready" : "Cancelled during execution";
    } catch (const std::exception& ex) {
      message = ex.what();
    } catch (...) {
      message = "Frame producer exception";
    }
    const auto worker_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - worker_started)
                                    .count();
    TraceDetailRequest("worker-end", job.request, success ? "frame-ready" : message.c_str(),
                       worker_elapsed);

    {
      std::scoped_lock lock(mutex_);
      if (running_job_ && running_job_->job_id == job.job_id) running_job_.reset();
      jobs_changed_.notify_all();
    }
    CompleteJob(job.request, success, std::move(message));
  }
}

void EditorSessionRenderSchedulerPort::ExecuteJob(Job job) {
  if (IsCancelled(job.job_id)) return;

  alcedo::IFrameSink*            sink = nullptr;
  EditorSessionFrameSinkResolver resolver;
  EditorSessionTestFrameProducer test_producer;
  {
    std::scoped_lock lock(mutex_);
    resolver      = sink_resolver_;
    test_producer = test_producer_;
  }
  if (resolver) sink = resolver();
  if (!sink) throw std::runtime_error("No presentation frame sink bound");

  alcedo::FramePreviewMetadata meta = FrameRoleToPreviewMetadata(job.request.intent);
  meta.presentation_request_id      = job.request.request_id;
  const alcedo::FramePresentationMode mode =
      job.request.intent.frame_role == alcedo::FrameRole::DetailPatch
          ? alcedo::FramePresentationMode::ViewportTransformed
          : alcedo::FramePresentationMode::FullFrame;
  // Bind before produce so test producers and pipeline Apply share the same
  // request-id / scope metadata. PipelineTask::SetExecutorRenderParams may
  // refine ROI fields under the render lock later.
  sink->BindFrameSubmission(alcedo::FrameCompletionSubmission{meta, mode});

  std::string error;
  bool        ok        = false;

  if (test_producer) {
    // Test producers do not execute a pipeline stage that knows the real output
    // dimensions, so preserve their explicit target setup. Production CUDA,
    // OpenCL and Metal paths publish the actual output themselves; pre-sizing
    // them with viewport dimensions rewrites render-reference geometry twice.
    sink->EnsureSize(std::max(1, job.request.intent.requested_width),
                     std::max(1, job.request.intent.requested_height));
    ok = test_producer(sink, job.request);
    if (!ok) error = "Test frame producer failed";
  } else {
    ok = TryProducePipelineFrame(job.request, sink, &error);
  }
  if (!ok) throw std::runtime_error(error.empty() ? "Render failed" : error);
}

auto EditorSessionRenderSchedulerPort::TryProducePipelineFrame(
    const alcedo::EditorRenderRequest& request, alcedo::IFrameSink* sink, std::string* error)
    -> bool {
  if (!sink) {
    if (error) *error = "Presentation sink is null";
    return false;
  }
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  EditorSessionSchedulerServices             services;
  std::shared_ptr<alcedo::PipelineScheduler> scheduler;
  {
    std::scoped_lock lock(mutex_);
    pipeline_port = pipeline_port_;
    services      = services_;
    if (!pipeline_scheduler_) pipeline_scheduler_ = std::make_shared<alcedo::PipelineScheduler>(1);
    scheduler = pipeline_scheduler_;
  }
  if (!pipeline_port || !scheduler) {
    if (error) *error = "Editor pipeline scheduler is not fully configured";
    return false;
  }
  auto guard = pipeline_port->EnsureLoaded(request.intent.element_id, error);
  if (!guard || !guard->pipeline_) {
    if (error && error->empty()) *error = "No pipeline guard for image; open may lack a project";
    return false;
  }
  std::shared_ptr<alcedo::ImagePoolService> image_pool;
  if (services.image_pool) image_pool = services.image_pool();
  if (!image_pool) {
    if (error) *error = "Image pool is unavailable";
    return false;
  }

  try {
    TraceDetailRequest("pipeline-submit", request);
    const auto pipeline_started = std::chrono::steady_clock::now();
    auto                           exec = guard->pipeline_;
    std::shared_ptr<alcedo::Image> image_desc;
    try {
      image_desc = image_pool->Read<std::shared_ptr<alcedo::Image>>(
          request.intent.image_id,
          [](const std::shared_ptr<alcedo::Image>& image) { return image; });
    } catch (...) {
    }
    std::shared_ptr<alcedo::ImageBuffer> input;
    {
      std::scoped_lock lock(mutex_);
      if (cached_input_image_id_ == request.intent.image_id) input = cached_input_;
    }
    if (!input) {
      auto loaded = controllers::LoadImageInputBuffer(image_pool, request.intent.image_id);
      std::scoped_lock lock(mutex_);
      if (cached_input_image_id_ != request.intent.image_id || !cached_input_) {
        cached_input_image_id_ = request.intent.image_id;
        cached_input_          = std::move(loaded);
      }
      input = cached_input_;
    }

    alcedo::PipelineTask task;
    task.input_                             = std::move(input);
    task.input_desc_                        = std::move(image_desc);
    task.pipeline_executor_                 = exec;
    task.options_.render_desc_.render_type_ = RenderTypeForIntent(request.intent);
    task.options_.render_desc_.use_viewport_region_ =
        request.intent.quality == alcedo::EditorRenderQuality::Detail ||
        request.intent.reason == alcedo::EditorRenderReason::ScopeRefresh;
    task.options_.render_desc_.viewport_region_ = request.intent.view_region;
    task.options_.render_desc_.frame_metadata_ = FrameRoleToPreviewMetadata(request.intent);
    task.options_.render_desc_.frame_metadata_.presentation_request_id = request.request_id;
    task.request_id_                                                 = request.request_id;
    task.options_.is_callback_                                         = false;
    task.options_.is_seq_callback_                                     = false;
    task.options_.is_blocking_                                         = true;
    // Content renders install operator params (SetOperator + SetGlobalParams).
    // View-dependent ROI/detail/scope frames only retarget Geometry via
    // SetExecutorRenderParams (RESIZE ROI); replaying the full adjustment
    // snapshot every pan/zoom would thrash Image Loading (RAW_DECODE) cache.
    const bool apply_adjustment =
        alcedo::ReasonAppliesAdjustmentSnapshot(request.intent.reason);
    task.configure_under_render_lock_ =
        [snapshot              = request.intent.adjustment, sink,
         geometry_overlay_only = request.intent.geometry_overlay_only,
         apply_adjustment](alcedo::PipelineTask& locked_task) {
          auto locked_exec = locked_task.pipeline_executor_;
          if (!locked_exec) return false;
          if (apply_adjustment) {
            std::string apply_error;
            if (!alcedo::ApplyEditorAdjustmentSnapshot(*locked_exec, snapshot, &apply_error)) {
              throw std::runtime_error(apply_error.empty() ? "Failed to apply editor adjustment"
                                                           : apply_error);
            }
            // Never touch Image Loading defaults on tone/color slider frames —
            // EnsureLoadingOperatorDefaults can SetOperator(RAW_DECODE) and
            // wipe the demosaic cache even when the edit only moved exposure.
            if (alcedo::SnapshotTouchesImageLoading(snapshot)) {
              controllers::EnsureLoadingOperatorDefaults(locked_exec);
            }
          }
          if (geometry_overlay_only) {
            alcedo::DisableEditorGeometryOperatorForOverlay(*locked_exec);
          }
          controllers::AttachExecutionStages(locked_exec, sink);
          return true;
        };
    auto promise = std::make_shared<std::promise<std::shared_ptr<alcedo::ImageBuffer>>>();
    auto future  = promise->get_future();
    task.result_ = promise;
    if (request.intent.cancellation) {
      task.cancel_requested_ = [token = request.intent.cancellation]() {
        return token && token->IsCancelled();
      };
    }
    scheduler->ScheduleTask(std::move(task));
    const auto result = future.get();
    const auto pipeline_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      std::chrono::steady_clock::now() - pipeline_started)
                                      .count();
    if (!result) {
      TraceDetailRequest("pipeline-return", request, "empty-result", pipeline_elapsed);
      if (error) *error = "Pipeline returned an empty result";
      return false;
    }
    TraceDetailRequest("pipeline-return", request, "success", pipeline_elapsed);
    return true;
  } catch (const std::exception& ex) {
    TraceDetailRequest("pipeline-exception", request, ex.what());
    if (error) *error = ex.what();
  } catch (...) {
    if (error) *error = "Pipeline render failed";
  }
  return false;
}

void EditorSessionRenderSchedulerPort::CompleteJob(const alcedo::EditorRenderRequest& request,
                                                   bool success, std::string message) {
  std::shared_ptr<alcedo::EditorRenderCoordinator> coordinator;
  {
    std::scoped_lock lock(mutex_);
    coordinator = coordinator_.lock();
  }
  if (!coordinator) return;
  coordinator->NotifySchedulerCompleted(request.request_id, success, std::move(message));
}

}  // namespace alcedo::ui
