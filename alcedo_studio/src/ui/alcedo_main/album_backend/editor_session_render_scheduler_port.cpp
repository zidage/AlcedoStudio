//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"

#include <QCoreApplication>
#include <QEventLoop>
#include <QString>
#include <QThread>

#include <algorithm>
#include <chrono>
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

  // Drain in-flight pool work before tearing down this port.
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
  Job job;
  {
    std::scoped_lock lock(mutex_);
    // Coordinator is single-flight; reject a second job instead of queueing.
    if (shutting_down_ || running_job_) {
      TraceDetailRequest("scheduler-reject", request, "busy-or-shutdown");
      return 0;
    }
    job.job_id   = ++next_job_id_;
    job.request  = request;
    running_job_ = job;
    scheduled_.push_back(request);
  }
  TraceDetailRequest("scheduler-queued", request);
  const std::uint64_t job_id = job.job_id;
  DispatchJob(std::move(job));
  return job_id;
}

auto EditorSessionRenderSchedulerPort::CanProduceFrame(
    const alcedo::EditorRenderRequest& request) const -> bool {
  EditorSessionTestFrameProducer                          test_producer;
  std::function<std::shared_ptr<alcedo::ImagePoolService>()> image_pool;
  {
    std::scoped_lock lock(mutex_);
    test_producer = test_producer_;
    image_pool    = services_.image_pool;
  }
  if (test_producer) {
    return true;
  }
  if (!image_pool) {
    return false;
  }
  try {
    const auto pool = image_pool();
    if (!pool) {
      return false;
    }
    const auto image = pool->Read<std::shared_ptr<alcedo::Image>>(
        request.intent.image_id,
        [](const std::shared_ptr<alcedo::Image>& value) { return value; });
    return image && !image->image_path_.empty();
  } catch (...) {
    return false;
  }
}

auto EditorSessionRenderSchedulerPort::EnsurePipelineScheduler()
    -> std::shared_ptr<alcedo::PipelineScheduler> {
  std::scoped_lock lock(mutex_);
  if (!pipeline_scheduler_) {
    pipeline_scheduler_ = std::make_shared<alcedo::PipelineScheduler>(1);
  }
  return pipeline_scheduler_;
}

void EditorSessionRenderSchedulerPort::DispatchJob(Job job) {
  EditorSessionFrameSinkResolver resolver;
  EditorSessionTestFrameProducer test_producer;
  {
    std::scoped_lock lock(mutex_);
    resolver      = sink_resolver_;
    test_producer = test_producer_;
  }
  alcedo::IFrameSink* sink = resolver ? resolver() : nullptr;
  if (!sink) {
    auto scheduler = EnsurePipelineScheduler();
    scheduler->ScheduleWork([this, job]() mutable {
      FinishJob(job, false, "No presentation frame sink bound");
    });
    return;
  }

  if (test_producer) {
    DispatchTestProducer(std::move(job), sink, std::move(test_producer));
    return;
  }
  DispatchPipelineFrame(std::move(job), sink);
}

void EditorSessionRenderSchedulerPort::DispatchTestProducer(
    Job job, alcedo::IFrameSink* sink, EditorSessionTestFrameProducer producer) {
  auto scheduler = EnsurePipelineScheduler();
  scheduler->ScheduleWork([this, job = std::move(job), sink, producer = std::move(producer)]() {
    if (JobIsCancelled(job)) {
      FinishJob(job, false, "Cancelled during execution");
      return;
    }
    alcedo::FramePreviewMetadata meta = FrameRoleToPreviewMetadata(job.request.intent);
    meta.presentation_request_id      = job.request.request_id;
    const alcedo::FramePresentationMode mode =
        job.request.intent.frame_role == alcedo::FrameRole::DetailPatch
            ? alcedo::FramePresentationMode::ViewportTransformed
            : alcedo::FramePresentationMode::FullFrame;
    // ponytail: test producers still need BindFrameSubmission; production path
    // leaves binding to PipelineTask::SetExecutorRenderParams.
    sink->BindFrameSubmission(alcedo::FrameCompletionSubmission{meta, mode});
    sink->EnsureSize(std::max(1, job.request.intent.requested_width),
                     std::max(1, job.request.intent.requested_height));
    bool        ok = false;
    std::string message;
    try {
      ok      = producer(sink, job.request);
      message = ok ? "Frame ready" : "Test frame producer failed";
    } catch (const std::exception& ex) {
      message = ex.what();
    } catch (...) {
      message = "Test frame producer exception";
    }
    if (JobIsCancelled(job)) {
      FinishJob(job, false, "Cancelled during execution");
      return;
    }
    FinishJob(job, ok, std::move(message));
  });
}

void EditorSessionRenderSchedulerPort::DispatchPipelineFrame(Job job, alcedo::IFrameSink* sink) {
  std::shared_ptr<EditorSessionPipelinePort> pipeline_port;
  EditorSessionSchedulerServices             services;
  auto                                       scheduler = EnsurePipelineScheduler();
  {
    std::scoped_lock lock(mutex_);
    pipeline_port = pipeline_port_;
    services      = services_;
  }
  if (!pipeline_port || !scheduler) {
    scheduler->ScheduleWork([this, job]() mutable {
      FinishJob(job, false, "Editor pipeline scheduler is not fully configured");
    });
    return;
  }

  std::string error;
  auto        guard = pipeline_port->EnsureLoaded(job.request.intent.element_id, &error);
  if (!guard || !guard->pipeline_) {
    if (error.empty()) {
      error = "No pipeline guard for image; open may lack a project";
    }
    scheduler->ScheduleWork([this, job, error]() mutable {
      FinishJob(job, false, std::move(error));
    });
    return;
  }

  std::shared_ptr<alcedo::ImagePoolService> image_pool;
  if (services.image_pool) {
    image_pool = services.image_pool();
  }
  if (!image_pool) {
    scheduler->ScheduleWork([this, job]() mutable {
      FinishJob(job, false, "Image pool is unavailable");
    });
    return;
  }

  try {
    TraceDetailRequest("pipeline-submit", job.request);
    auto                           exec = guard->pipeline_;
    std::shared_ptr<alcedo::Image> image_desc;
    try {
      image_desc = image_pool->Read<std::shared_ptr<alcedo::Image>>(
          job.request.intent.image_id,
          [](const std::shared_ptr<alcedo::Image>& image) { return image; });
    } catch (...) {
    }
    std::shared_ptr<alcedo::ImageBuffer> input;
    {
      std::scoped_lock lock(mutex_);
      if (cached_input_image_id_ == job.request.intent.image_id) {
        input = cached_input_;
      }
    }
    if (!input) {
      auto loaded = controllers::LoadImageInputBuffer(image_pool, job.request.intent.image_id);
      std::scoped_lock lock(mutex_);
      if (cached_input_image_id_ != job.request.intent.image_id || !cached_input_) {
        cached_input_image_id_ = job.request.intent.image_id;
        cached_input_          = std::move(loaded);
      }
      input = cached_input_;
    }

    alcedo::PipelineTask task;
    task.input_                             = std::move(input);
    task.input_desc_                        = std::move(image_desc);
    task.pipeline_executor_                 = exec;
    task.options_.render_desc_.render_type_ = RenderTypeForIntent(job.request.intent);
    task.options_.render_desc_.use_viewport_region_ =
        job.request.intent.quality == alcedo::EditorRenderQuality::Detail ||
        job.request.intent.reason == alcedo::EditorRenderReason::ScopeRefresh;
    task.options_.render_desc_.viewport_region_ = job.request.intent.view_region;
    task.options_.render_desc_.frame_metadata_  = FrameRoleToPreviewMetadata(job.request.intent);
    task.options_.render_desc_.frame_metadata_.presentation_request_id = job.request.request_id;
    task.request_id_                           = job.request.request_id;
    task.options_.is_callback_                 = false;
    task.options_.is_seq_callback_             = false;
    task.options_.is_blocking_                 = false;
    const bool apply_adjustment = alcedo::ReasonAppliesAdjustmentSnapshot(job.request.intent.reason);
    task.configure_under_render_lock_ =
        [snapshot              = job.request.intent.adjustment, sink,
         geometry_overlay_only = job.request.intent.geometry_overlay_only,
         apply_adjustment](alcedo::PipelineTask& locked_task) {
          auto locked_exec = locked_task.pipeline_executor_;
          if (!locked_exec) {
            return false;
          }
          if (apply_adjustment) {
            std::string apply_error;
            if (!alcedo::ApplyEditorAdjustmentSnapshot(*locked_exec, snapshot, &apply_error)) {
              throw std::runtime_error(apply_error.empty() ? "Failed to apply editor adjustment"
                                                           : apply_error);
            }
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
    if (job.request.intent.cancellation) {
      task.cancel_requested_ = [token = job.request.intent.cancellation]() {
        return token && token->IsCancelled();
      };
    }
    const auto request_for_trace = job.request;
    task.on_complete_            = [this, job, request_for_trace](bool success) mutable {
      TraceDetailRequest("pipeline-return", request_for_trace, success ? "success" : "failed");
      if (JobIsCancelled(job)) {
        FinishJob(job, false, "Cancelled during execution");
        return;
      }
      FinishJob(job, success, success ? "Frame ready" : "Pipeline returned an empty result");
    };
    scheduler->ScheduleTask(std::move(task));
  } catch (const std::exception& ex) {
    TraceDetailRequest("pipeline-exception", job.request, ex.what());
    scheduler->ScheduleWork([this, job, message = std::string(ex.what())]() mutable {
      FinishJob(job, false, std::move(message));
    });
  } catch (...) {
    scheduler->ScheduleWork([this, job]() mutable {
      FinishJob(job, false, "Pipeline render failed");
    });
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

void EditorSessionRenderSchedulerPort::WaitForSessionIdle(std::uint64_t session_epoch) {
  const auto idle = [this, session_epoch] {
    return !running_job_ ||
           running_job_->request.intent.image_load_request_id.value != session_epoch;
  };

  for (;;) {
    {
      std::unique_lock lock(mutex_);
      if (idle()) {
        return;
      }
      jobs_changed_.wait_for(lock, std::chrono::milliseconds(16), idle);
      if (idle()) {
        return;
      }
    }
    if (QCoreApplication::instance() != nullptr &&
        QThread::currentThread() == QCoreApplication::instance()->thread()) {
      QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents, 16);
    }
  }
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
  bool should_complete = false;
  {
    std::scoped_lock lock(mutex_);
    if (running_job_ && running_job_->job_id == job.job_id) {
      if (running_job_->cancelled ||
          (running_job_->request.intent.cancellation &&
           running_job_->request.intent.cancellation->IsCancelled())) {
        success = false;
        if (message.empty() || message == "Frame ready") {
          message = "Cancelled during execution";
        }
      }
      running_job_.reset();
      should_complete = true;
    }
    jobs_changed_.notify_all();
  }
  if (should_complete) {
    CompleteJob(job.request, success, std::move(message));
  }
}

void EditorSessionRenderSchedulerPort::CompleteJob(const alcedo::EditorRenderRequest& request,
                                                   bool success, std::string message) {
  std::shared_ptr<alcedo::EditorRenderCoordinator> coordinator;
  {
    std::scoped_lock lock(mutex_);
    coordinator = coordinator_.lock();
  }
  if (!coordinator) {
    return;
  }
  coordinator->NotifySchedulerCompleted(request.request_id, success, std::move(message));
}

}  // namespace alcedo::ui
