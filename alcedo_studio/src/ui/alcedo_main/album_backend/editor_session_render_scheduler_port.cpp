//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QString>
#include <QThread>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include "app/pipeline_service.hpp"
#include "edit/frame_presentation_types.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/alcedo_main/editor_support/controllers/image_controller.hpp"
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
  meta.session_epoch           = intent.image_load_request_id.value;
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
  QString msg =
      QStringLiteral("[ROI_TRACE][%1] request=%2 image=%3 session_epoch=%4 requested=%5x%6")
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

auto MakeEditorRenderDesc(const alcedo::EditorRenderRequest& request) -> alcedo::RenderDesc {
  const auto&        intent = request.intent;
  alcedo::RenderDesc desc;
  desc.render_type_         = RenderTypeForIntent(intent);
  desc.use_viewport_region_ = intent.quality == alcedo::EditorRenderQuality::Detail ||
                              intent.reason == alcedo::EditorRenderReason::ScopeRefresh;
  desc.viewport_region_                        = intent.view_region;
  desc.frame_metadata_                         = FrameRoleToPreviewMetadata(intent);
  desc.frame_metadata_.presentation_request_id = request.request_id;
  desc.document_geometry_ = intent.geometry_overlay_only
                                ? alcedo::DocumentGeometryUse::RotatedUncroppedSource
                                : alcedo::DocumentGeometryUse::ApplyCropAndRotation;
  return desc;
}

EditorSessionRenderSchedulerPort::EditorSessionRenderSchedulerPort(
    std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler)
    : pipeline_scheduler_(std::move(pipeline_scheduler)) {}

EditorSessionRenderSchedulerPort::~EditorSessionRenderSchedulerPort() {
  std::shared_ptr<alcedo::EditorRenderCancellationToken> running_cancellation;
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    if (running_job_) {
      running_job_->cancelled = true;
      running_cancellation    = running_job_->request.intent.cancellation;
    }
    sink_resolver_ = {};
  }
  if (running_cancellation) {
    running_cancellation->Cancel();
  }

  // Drain in-flight pool work before tearing down this port: its completion calls back into this
  // object. The in-flight frame may wait for the scene graph to hand over a present slot, which
  // needs the GUI thread, so a GUI-thread caller keeps delivering events while it waits.
  for (;;) {
    {
      std::unique_lock lock(mutex_);
      if (!running_job_) {
        return;
      }
      jobs_changed_.wait_for(lock, std::chrono::milliseconds(16),
                             [this] { return !running_job_.has_value(); });
      if (!running_job_) {
        return;
      }
    }
    if (QCoreApplication::instance() != nullptr &&
        QThread::currentThread() == QCoreApplication::instance()->thread()) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 16);
    }
  }
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

void EditorSessionRenderSchedulerPort::BindSessionContext(
    std::uint64_t epoch, sl_element_id_t element_id, image_id_t image_id,
    alcedo::PresentationSinkId presentation_sink_id) {
  std::scoped_lock lock(mutex_);
  // Same open image: keep Image / ImageBuffer. RouteInitialRender
  // (undo, head-move, quality re-route) rebinds with the same element/image and
  // must not force another full-file LoadImageInputBuffer or drop the payload.
  // Only a true image identity change replaces the whole context.
  if (session_context_ && session_context_->element_id == element_id &&
      session_context_->image_id == image_id) {
    session_context_->epoch                = epoch;
    session_context_->presentation_sink_id = presentation_sink_id;
    return;
  }
  EditorRenderSessionContext context;
  context.epoch                = epoch;
  context.element_id           = element_id;
  context.image_id             = image_id;
  context.presentation_sink_id = presentation_sink_id;
  session_context_             = std::move(context);
}

void EditorSessionRenderSchedulerPort::ClearSessionContext() {
  std::shared_ptr<alcedo::PipelineExecutor> executor;
  {
    std::scoped_lock lock(mutex_);
    session_context_.reset();
    executor = executor_;
  }
  if (!executor) {
    return;
  }
  // Every use of the executor runs on the single worker, which gives this work exclusive access
  // after the in-flight frame: the release never waits and never races a render.
  EnsurePipelineScheduler()->ScheduleWork([executor] { executor->ReleaseBinding(); });
}

void EditorSessionRenderSchedulerPort::InstallSessionContext(EditorRenderSessionContext context) {
  std::scoped_lock lock(mutex_);
  session_context_ = std::move(context);
}

auto EditorSessionRenderSchedulerPort::session_context() const
    -> std::optional<EditorRenderSessionContext> {
  std::scoped_lock lock(mutex_);
  return session_context_;
}

auto EditorSessionRenderSchedulerPort::context_payload_load_count() const -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  return context_payload_load_count_;
}

auto EditorSessionRenderSchedulerPort::sink_resolve_count() const -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  return sink_resolve_count_;
}

auto EditorSessionRenderSchedulerPort::ContextMatchesRequest(
    const EditorRenderSessionContext& context, const alcedo::EditorRenderRequest& request) const
    -> bool {
  return context.epoch == request.intent.image_load_request_id.value &&
         context.element_id == request.intent.element_id &&
         context.image_id == request.intent.image_id;
}

auto EditorSessionRenderSchedulerPort::ContextPayloadReady(
    const EditorRenderSessionContext& context) const -> bool {
  return context.image && !context.image->image_path_.empty() && context.input;
}

auto EditorSessionRenderSchedulerPort::Schedule(
    const alcedo::EditorRenderRequest&       request,
    alcedo::EditorPipelineScheduleCompletion on_complete) -> std::uint64_t {
  if (!CanProduceFrame(request)) {
    TraceDetailRequest("scheduler-reject", request, "no-frame-source");
    return 0;
  }
  Job job;
  {
    std::scoped_lock lock(mutex_);
    // Coordinator is single-flight; reject a second job instead of queueing.
    if (shutting_down_ || running_job_) {
      TraceDetailRequest("scheduler-reject", request, "busy-or-shutdown");
      return 0;
    }
    job.job_id      = ++next_job_id_;
    job.request     = request;
    job.on_complete = std::move(on_complete);
    running_job_    = job;
    scheduled_.push_back(request);
  }
  TraceDetailRequest("scheduler-queued", request);
  const std::uint64_t job_id = job.job_id;
  DispatchJob(std::move(job));
  return job_id;
}

auto EditorSessionRenderSchedulerPort::CanProduceFrame(
    const alcedo::EditorRenderRequest& request) const -> bool {
  bool has_matching_context = false;
  bool payload_ready        = false;
  bool has_image_pool       = false;
  {
    std::scoped_lock lock(mutex_);
    if (session_context_ && ContextMatchesRequest(*session_context_, request)) {
      has_matching_context = true;
      payload_ready        = ContextPayloadReady(*session_context_);
    }
    has_image_pool = static_cast<bool>(services_.image_pool);
  }
  // Bound context with payload: no pool probe on the hot path.
  if (has_matching_context && payload_ready) {
    return true;
  }
  // Identity bound or services available: allow Schedule; EnsureContext loads once.
  if (has_matching_context || has_image_pool) {
    return request.intent.image_id != 0 && request.intent.element_id != 0;
  }
  return false;
}

auto EditorSessionRenderSchedulerPort::EnsureContextForRequest(
    const alcedo::EditorRenderRequest& request, std::string* error)
    -> std::optional<EditorRenderSessionContext> {
  {
    std::scoped_lock lock(mutex_);
    if (session_context_ && ContextMatchesRequest(*session_context_, request) &&
        ContextPayloadReady(*session_context_)) {
      return session_context_;
    }
  }

  std::shared_ptr<EditorSessionPipelinePort>                 pipeline_port;
  std::function<std::shared_ptr<alcedo::ImagePoolService>()> image_pool_resolver;
  {
    std::scoped_lock lock(mutex_);
    pipeline_port       = pipeline_port_;
    image_pool_resolver = services_.image_pool;
    // Align identity with this request when open/switch bind was skipped.
    if (!session_context_ || !ContextMatchesRequest(*session_context_, request)) {
      EditorRenderSessionContext context;
      context.epoch                = request.intent.image_load_request_id.value;
      context.element_id           = request.intent.element_id;
      context.image_id             = request.intent.image_id;
      context.presentation_sink_id = request.intent.presentation_sink_id;
      session_context_             = std::move(context);
    }
  }

  if (!pipeline_port) {
    if (error) {
      *error = "Editor pipeline port is not configured";
    }
    return std::nullopt;
  }

  // Rendering only reads the image the editor session holds. A request for an image the
  // session no longer (or not yet) holds is stale; loading here would rebind that image's
  // history and working document behind the session.
  if (!pipeline_port->CurrentPreview(request.intent.element_id)) {
    if (error) {
      *error = "Editor pipeline is not held for this image; the render request is stale";
    }
    return std::nullopt;
  }

  if (!image_pool_resolver) {
    if (error) {
      *error = "Image pool is unavailable";
    }
    return std::nullopt;
  }
  auto image_pool = image_pool_resolver();
  if (!image_pool) {
    if (error) {
      *error = "Image pool is unavailable";
    }
    return std::nullopt;
  }

  std::shared_ptr<alcedo::Image>       image_desc;
  std::shared_ptr<alcedo::ImageBuffer> input;
  try {
    image_desc = image_pool->Read<std::shared_ptr<alcedo::Image>>(
        request.intent.image_id, [](const std::shared_ptr<alcedo::Image>& image) { return image; });
    if (!image_desc || image_desc->image_path_.empty()) {
      if (error) {
        *error = "Image descriptor is missing or has an empty path";
      }
      return std::nullopt;
    }
    input = controllers::LoadImageInputBuffer(image_pool, request.intent.image_id);
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return std::nullopt;
  } catch (...) {
    if (error) {
      *error = "Failed to load session render context payload";
    }
    return std::nullopt;
  }

  std::scoped_lock lock(mutex_);
  // Another bind/switch may have replaced the identity while we loaded.
  if (!session_context_ || !ContextMatchesRequest(*session_context_, request)) {
    if (error) {
      *error = "Session render context was replaced during load";
    }
    return std::nullopt;
  }
  // Only the first successful payload load for this identity counts.
  if (!ContextPayloadReady(*session_context_)) {
    session_context_->image = std::move(image_desc);
    session_context_->input = std::move(input);
    ++context_payload_load_count_;
  }
  return session_context_;
}

auto EditorSessionRenderSchedulerPort::EnsurePipelineScheduler()
    -> std::shared_ptr<alcedo::PipelineScheduler> {
  std::scoped_lock lock(mutex_);
  if (!pipeline_scheduler_) {
    pipeline_scheduler_ = std::make_shared<alcedo::PipelineScheduler>(1);
  }
  return pipeline_scheduler_;
}

auto EditorSessionRenderSchedulerPort::EnsureExecutor()
    -> std::shared_ptr<alcedo::PipelineExecutor> {
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  {
    std::scoped_lock lock(mutex_);
    if (executor_) {
      return executor_;
    }
    pipeline_port = pipeline_port_;
  }
  // The backend preference is fixed at construction; a preference change takes effect after the
  // application restarts, like every other executor.
  const auto service = pipeline_port ? pipeline_port->PipelineMapper() : nullptr;
  auto executor = std::make_shared<alcedo::PipelineExecutor>(alcedo::ExecutorRole::Interactive);
  if (service) {
    executor->SetAcceleratorBackendPreference(service->GetAcceleratorBackendPreference());
  }
  std::scoped_lock lock(mutex_);
  if (!executor_) {
    executor_ = std::move(executor);
  }
  return executor_;
}

auto EditorSessionRenderSchedulerPort::interactive_executor() const
    -> std::shared_ptr<alcedo::PipelineExecutor> {
  std::scoped_lock lock(mutex_);
  return executor_;
}

void EditorSessionRenderSchedulerPort::DispatchJob(Job job) {
  EditorSessionFrameSinkResolver resolver;
  alcedo::PresentationSinkId     bound_sink_id = 0;
  bool                           has_context   = false;
  {
    std::scoped_lock lock(mutex_);
    resolver = sink_resolver_;
    if (session_context_) {
      has_context   = true;
      bound_sink_id = session_context_->presentation_sink_id;
    }
  }

  // Session-scoped sink identity is stamped at bind. Reject mismatched intents
  // before resolving a live pointer for pipeline submit.
  if (has_context && bound_sink_id != 0 && job.request.intent.presentation_sink_id != 0 &&
      job.request.intent.presentation_sink_id != bound_sink_id) {
    auto scheduler = EnsurePipelineScheduler();
    scheduler->ScheduleWork([this, job]() mutable {
      FinishJob(job, false, "Presentation sink identity does not match session context");
    });
    return;
  }

  alcedo::IFrameSink* sink = nullptr;
  {
    std::scoped_lock lock(mutex_);
    ++sink_resolve_count_;
  }
  sink = resolver ? resolver() : nullptr;
  if (!sink) {
    auto scheduler = EnsurePipelineScheduler();
    scheduler->ScheduleWork(
        [this, job]() mutable { FinishJob(job, false, "No presentation frame sink bound"); });
    return;
  }

  DispatchPipelineFrame(std::move(job), sink);
}

void EditorSessionRenderSchedulerPort::DispatchPipelineFrame(Job job, alcedo::IFrameSink* sink) {
  auto scheduler = EnsurePipelineScheduler();
  if (!scheduler) {
    // EnsurePipelineScheduler always creates one; defensive path only.
    FinishJob(job, false, "Editor pipeline scheduler is not fully configured");
    return;
  }

  std::string error;
  auto        context = EnsureContextForRequest(job.request, &error);
  if (!context) {
    scheduler->ScheduleWork([this, job, error = std::move(error)]() mutable {
      FinishJob(job, false,
                error.empty() ? "Session render context is unavailable" : std::move(error));
    });
    return;
  }

  // The latest preview the history published: the working document at dispatch. A frame that
  // starts after a later write renders that later preview, never a partly written document.
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  {
    std::scoped_lock lock(mutex_);
    pipeline_port = pipeline_port_;
  }
  auto snapshot =
      pipeline_port ? pipeline_port->CurrentPreview(job.request.intent.element_id) : nullptr;
  if (!snapshot) {
    scheduler->ScheduleWork([this, job]() mutable {
      FinishJob(job, false,
                "Editor pipeline is not held for this image; the render request is stale");
    });
    return;
  }

  try {
    TraceDetailRequest("pipeline-submit", job.request);
    alcedo::PipelineTask task;
    task.input_                      = context->input;
    task.input_desc_                 = context->image;
    task.pipeline_executor_          = EnsureExecutor();
    task.snapshot_                   = std::move(snapshot);
    task.options_.render_desc_       = MakeEditorRenderDesc(job.request);
    task.request_id_                 = job.request.request_id;
    task.options_.is_callback_       = false;
    task.options_.is_seq_callback_   = false;
    task.options_.is_blocking_       = false;
    // The executor belongs to this port and every use of it runs on the single worker, which has
    // exclusive access, so the viewport sink is attached there before the frame. It changes only
    // when the viewport item is replaced.
    task.prepare_                    = [sink](alcedo::PipelineTask& prepared_task) {
      auto& executor = *prepared_task.pipeline_executor_;
      if (executor.GetFrameSink() != sink) {
        executor.AttachFrameSink(sink);
      }
      return true;
    };
    if (job.request.intent.cancellation) {
      task.cancel_requested_ = [token = job.request.intent.cancellation]() {
        return token && token->IsCancelled();
      };
    }
    const auto request_for_trace = job.request;
    task.on_complete_            = [this, job, request_for_trace](bool success,
                                                       std::string message) mutable {
      TraceDetailRequest("pipeline-return", request_for_trace, success ? "success" : "failed");
      if (JobIsCancelled(job)) {
        FinishJob(job, false, "Cancelled during execution");
        return;
      }
      if (success) {
        FinishJob(job, true, "Frame ready");
        return;
      }
      FinishJob(job, false, message.empty() ? "Pipeline returned an empty result" : std::move(message));
    };
    scheduler->ScheduleTask(std::move(task));
  } catch (const std::exception& ex) {
    TraceDetailRequest("pipeline-exception", job.request, ex.what());
    scheduler->ScheduleWork([this, job, message = std::string(ex.what())]() mutable {
      FinishJob(job, false, std::move(message));
    });
  } catch (...) {
    scheduler->ScheduleWork(
        [this, job]() mutable { FinishJob(job, false, "Pipeline render failed"); });
  }
}

void EditorSessionRenderSchedulerPort::Cancel(std::uint64_t scheduler_job_id) {
  std::shared_ptr<alcedo::EditorRenderCancellationToken> cancellation;
  {
    std::scoped_lock lock(mutex_);
    if (!running_job_ || running_job_->job_id != scheduler_job_id) {
      return;
    }
    running_job_->cancelled = true;
    cancellation            = running_job_->request.intent.cancellation;
  }
  if (cancellation) {
    cancellation->Cancel();
  }
  // Completion arrives from the pipeline pool via on_complete_ / FinishJob.
}

auto EditorSessionRenderSchedulerPort::last_scheduled() const
    -> std::vector<alcedo::EditorRenderRequest> {
  std::scoped_lock lock(mutex_);
  return scheduled_;
}

auto EditorSessionRenderSchedulerPort::JobIsCancelled(const Job& job) const -> bool {
  std::scoped_lock lock(mutex_);
  if (shutting_down_) {
    return true;
  }
  if (running_job_ && running_job_->job_id == job.job_id) {
    if (running_job_->cancelled) {
      return true;
    }
  }
  return job.request.intent.cancellation && job.request.intent.cancellation->IsCancelled();
}

void EditorSessionRenderSchedulerPort::FinishJob(const Job& job, bool success,
                                                 std::string message) {
  bool                                     should_complete = false;
  alcedo::EditorPipelineScheduleCompletion on_complete;
  {
    std::scoped_lock lock(mutex_);
    if (running_job_ && running_job_->job_id == job.job_id) {
      if (running_job_->cancelled || (running_job_->request.intent.cancellation &&
                                      running_job_->request.intent.cancellation->IsCancelled())) {
        success = false;
        if (message.empty() || message == "Frame ready") {
          message = "Cancelled during execution";
        }
      }
      on_complete = std::move(running_job_->on_complete);
      running_job_.reset();
      should_complete = true;
    }
    jobs_changed_.notify_all();
  }
  if (should_complete) {
    CompleteJob(job.request, success, std::move(message), std::move(on_complete));
  }
}

void EditorSessionRenderSchedulerPort::CompleteJob(
    const alcedo::EditorRenderRequest& /*request*/, bool success, std::string message,
    alcedo::EditorPipelineScheduleCompletion on_complete) {
  if (!on_complete) {
    return;
  }
  // Forward-only: coordinator installed this at Schedule. Pool completion already
  // deferred ScheduleNext; the callback typically ends with Pump().
  on_complete(success, std::move(message));
}

}  // namespace alcedo::ui
