//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_render_scheduler_port.hpp"

#include <QMetaObject>
#include <algorithm>
#include <future>
#include <stdexcept>
#include <utility>

#include "app/editor_adjustment_pipeline.hpp"
#include "edit/frame_presentation_types.hpp"
#include "edit/scope/final_display_frame_tap.hpp"
#include "image/image.hpp"
#include "image/image_buffer.hpp"
#include "io/image/image_loader.hpp"
#include "renderer/pipeline_task.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/image_controller.hpp"
#include "ui/alcedo_main/editor_dialog/controllers/pipeline_controller.hpp"
#include "ui/editor_rhi/direct_frame_sink.hpp"

namespace alcedo::ui {
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
  meta.preview_generation      = intent.reason == alcedo::EditorRenderReason::ScopeRefresh
                                     ? intent.view_generation
                                     : intent.render_generation;
  meta.detail_serial           = 0;
  meta.image_identity          = static_cast<std::uint64_t>(intent.image_id);
  meta.image_generation        = intent.image_load_request_id.value;
  meta.scope_update_allowed    = alcedo::ScopeUpdateAllowedForReason(intent.reason);
  meta.scope_refresh_requested = intent.reason == alcedo::EditorRenderReason::ScopeRefresh;
  return meta;
}

auto DirectSinkFor(alcedo::IFrameSink* sink) -> alcedo::editor_rhi::DirectFrameSink* {
  if (auto* direct_sink = dynamic_cast<alcedo::editor_rhi::DirectFrameSink*>(sink)) {
    return direct_sink;
  }
  if (auto* tap = dynamic_cast<alcedo::FinalDisplayFrameTapSink*>(sink)) {
    return dynamic_cast<alcedo::editor_rhi::DirectFrameSink*>(tap->downstream_sink());
  }
  return nullptr;
}

}  // namespace

EditorSessionRenderSchedulerPort::EditorSessionRenderSchedulerPort(
    std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler)
    : pipeline_scheduler_(std::move(pipeline_scheduler)) {}

EditorSessionRenderSchedulerPort::~EditorSessionRenderSchedulerPort() {
  std::vector<std::thread>                                            workers;
  std::vector<std::shared_ptr<alcedo::EditorRenderCancellationToken>> cancellations;
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    for (auto& entry : jobs_) {
      auto& job     = entry.second;
      job.cancelled = true;
      if (job.request.intent.cancellation) cancellations.push_back(job.request.intent.cancellation);
    }
    sink_resolver_ = {};
    workers.swap(workers_);
  }
  for (const auto& cancellation : cancellations) cancellation->Cancel();
  for (auto& worker : workers) {
    if (worker.joinable()) worker.join();
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
  Job  job;
  bool has_producer = false;
  {
    std::scoped_lock lock(mutex_);
    if (shutting_down_) return 0;
    job.job_id        = ++next_job_id_;
    job.request       = request;
    jobs_[job.job_id] = job;
    scheduled_.push_back(request);
    if (test_producer_) {
      has_producer = true;
    } else if (services_.image_pool) {
      try {
        if (auto pool = services_.image_pool()) {
          const auto image = pool->Read<std::shared_ptr<alcedo::Image>>(
              request.intent.image_id,
              [](const std::shared_ptr<alcedo::Image>& value) { return value; });
          has_producer = static_cast<bool>(image) && !image->image_path_.empty();
        }
      } catch (...) {
        has_producer = false;
      }
    }
  }
  // Synthetic IDs stay in flight for the shell route until a real sink/image
  // is available, preserving the loading state without fake success.
  if (!has_producer) return job.job_id;

  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(job.job_id);
    if (it != jobs_.end()) it->second.running = true;
  }
  std::thread worker([this, job]() mutable {
    try {
      ExecuteJob(job);
    } catch (const std::exception& ex) {
      RemoveJob(job.job_id);
      CompleteJob(job.request, false, false, ex.what());
    } catch (...) {
      RemoveJob(job.job_id);
      CompleteJob(job.request, false, false, "First-frame producer exception");
    }
  });
  bool cancel_started_worker = false;
  {
    std::scoped_lock lock(mutex_);
    if (shutting_down_) {
      cancel_started_worker = true;
    } else {
      workers_.push_back(std::move(worker));
    }
  }
  if (cancel_started_worker) {
    if (job.request.intent.cancellation) job.request.intent.cancellation->Cancel();
    if (worker.joinable()) worker.join();
    return 0;
  }
  return job.job_id;
}

void EditorSessionRenderSchedulerPort::Cancel(std::uint64_t scheduler_job_id) {
  std::shared_ptr<alcedo::EditorRenderCancellationToken> cancellation;
  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(scheduler_job_id);
    if (it == jobs_.end()) return;
    it->second.cancelled = true;
    cancellation         = it->second.request.intent.cancellation;
    pending_presentations_.erase(it->second.request.request_id);
    if (!it->second.running) {
      jobs_.erase(it);
      jobs_changed_.notify_all();
    }
  }
  if (cancellation) cancellation->Cancel();
}

void EditorSessionRenderSchedulerPort::WaitForSessionIdle(std::uint64_t session_generation) {
  std::unique_lock lock(mutex_);
  jobs_changed_.wait(lock, [&] {
    return std::none_of(jobs_.begin(), jobs_.end(), [&](const auto& entry) {
      return entry.second.request.intent.image_load_request_id.value == session_generation;
    });
  });
}

void EditorSessionRenderSchedulerPort::RemoveJob(std::uint64_t job_id) {
  std::scoped_lock lock(mutex_);
  jobs_.erase(job_id);
  jobs_changed_.notify_all();
}

void EditorSessionRenderSchedulerPort::NotifyPresentationAcknowledged(
    std::uint64_t request_id, std::uint64_t image_generation, std::uint64_t image_identity) {
  bool                                             notify = false;
  std::shared_ptr<alcedo::EditorRenderCoordinator> coordinator;
  {
    std::scoped_lock lock(mutex_);
    auto             it = pending_presentations_.find(request_id);
    if (it == pending_presentations_.end()) return;
    if (it->second.image_generation != image_generation ||
        it->second.image_identity != image_identity) {
      return;
    }
    it->second.acknowledged = true;
    if (it->second.frame_submitted) {
      notify = true;
      pending_presentations_.erase(it);
      coordinator = coordinator_.lock();
    }
  }
  if (notify && coordinator) coordinator->NotifyFramePresented(request_id);
}

auto EditorSessionRenderSchedulerPort::last_scheduled() const
    -> std::vector<alcedo::EditorRenderRequest> {
  std::scoped_lock lock(mutex_);
  return scheduled_;
}

auto EditorSessionRenderSchedulerPort::pending_present_request_id() const -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  return pending_presentations_.empty() ? 0 : pending_presentations_.begin()->first;
}

void EditorSessionRenderSchedulerPort::ExecuteJob(Job job) {
  bool cancelled_before_execution = false;
  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(job.job_id);
    if (it != jobs_.end() && it->second.cancelled) {
      jobs_.erase(it);
      jobs_changed_.notify_all();
      cancelled_before_execution = true;
    }
  }
  if (cancelled_before_execution) {
    CompleteJob(job.request, false, false, "Cancelled before execution");
    return;
  }

  alcedo::IFrameSink*            sink = nullptr;
  EditorSessionFrameSinkResolver resolver;
  EditorSessionTestFrameProducer test_producer;
  {
    std::scoped_lock lock(mutex_);
    resolver      = sink_resolver_;
    test_producer = test_producer_;
  }
  if (resolver) sink = resolver();
  if (!sink) {
    RemoveJob(job.job_id);
    CompleteJob(job.request, false, false, "No presentation frame sink bound");
    return;
  }

  if (auto* direct_sink = DirectSinkFor(sink)) {
    direct_sink->SetFirstFrameCompositionCallback(
        [weak = weak_from_this()](std::uint64_t request_id, std::uint64_t image_generation,
                                  std::uint64_t image_identity) {
          if (const auto self = weak.lock()) {
            self->NotifyPresentationAcknowledged(request_id, image_generation, image_identity);
          }
        });
  }

  alcedo::FramePreviewMetadata meta = FrameRoleToPreviewMetadata(job.request.intent);
  meta.presentation_request_id      = job.request.request_id;
  sink->SetNextFramePreviewMetadata(meta);
  sink->SetNextFramePresentationMode(job.request.intent.frame_role == alcedo::FrameRole::DetailPatch
                                         ? alcedo::FramePresentationMode::ViewportTransformed
                                         : alcedo::FramePresentationMode::FullFrame);
  sink->EnsureSize(std::max(1, job.request.intent.requested_width),
                   std::max(1, job.request.intent.requested_height));

  auto*       direct_sink      = DirectSinkFor(sink);
  const auto  submitted_before = direct_sink ? direct_sink->submitted_frame_count() : 0;
  std::string error;
  bool        submitted = false;
  bool        ok        = false;
  const auto  reason    = job.request.intent.reason;
  const bool  track_first_composition =
      job.request.intent.frame_role == alcedo::FrameRole::InteractivePrimary &&
      (reason == alcedo::EditorRenderReason::InitialFrame ||
       reason == alcedo::EditorRenderReason::ImageSwitch ||
       reason == alcedo::EditorRenderReason::Retry);
  if (track_first_composition) {
    std::scoped_lock lock(mutex_);
    pending_presentations_[job.request.request_id] = PendingPresentation{
        job.request.intent.image_load_request_id.value, job.request.intent.image_id, false, false};
  }

  if (test_producer) {
    ok        = test_producer(sink, job.request);
    // A test producer owns the frame-submission seam.  Offscreen QML sinks do
    // not expose a native submitted-frame counter even after NotifyFrameReady,
    // so use its result as the explicit submission acknowledgement.  Native
    // RHI tests still return false when resource mapping or the device copy
    // fails, preserving their failure signal.
    submitted = ok;
    if (!ok) error = "Test frame producer failed";
  } else {
    ok        = TryProducePipelineFrame(job.request, sink, &error);
    submitted = ok && (!direct_sink || direct_sink->submitted_frame_count() > submitted_before);
    if (ok && !submitted) {
      ok    = false;
      error = "Pipeline completed without submitting a native presentation frame";
    }
  }

  bool cancelled_during_execution = false;
  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(job.job_id);
    if (it != jobs_.end() && it->second.cancelled) {
      jobs_.erase(it);
      jobs_changed_.notify_all();
      pending_presentations_.erase(job.request.request_id);
      cancelled_during_execution = true;
    } else {
      if (!ok || !submitted) pending_presentations_.erase(job.request.request_id);
      jobs_.erase(job.job_id);
      jobs_changed_.notify_all();
    }
  }
  if (cancelled_during_execution) {
    CompleteJob(job.request, false, false, "Cancelled during execution");
    return;
  }
  CompleteJob(job.request, ok, submitted,
              error.empty() ? (ok ? "Render completed" : "Render failed") : error);
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
    task.options_.render_desc_.frame_metadata_ = FrameRoleToPreviewMetadata(request.intent);
    task.options_.render_desc_.frame_metadata_.presentation_request_id = request.request_id;
    task.options_.is_callback_                                         = false;
    task.options_.is_seq_callback_                                     = false;
    task.options_.is_blocking_                                         = true;
    task.prepare_with_render_lock_ = [snapshot              = request.intent.adjustment, sink,
                                      geometry_overlay_only = request.intent.geometry_overlay_only](
                                         alcedo::PipelineTask& locked_task) {
      auto locked_exec = locked_task.pipeline_executor_;
      if (!locked_exec) return false;
      std::string apply_error;
      if (!alcedo::ApplyEditorAdjustmentSnapshot(*locked_exec, snapshot, &apply_error)) {
        throw std::runtime_error(apply_error.empty() ? "Failed to apply editor adjustment"
                                                     : apply_error);
      }
      if (geometry_overlay_only) {
        alcedo::DisableEditorGeometryOperatorForOverlay(*locked_exec);
      }
      controllers::EnsureLoadingOperatorDefaults(locked_exec);
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
    if (!result) {
      if (error) *error = "Pipeline returned an empty result";
      return false;
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) *error = ex.what();
  } catch (...) {
    if (error) *error = "Pipeline render failed";
  }
  return false;
}

void EditorSessionRenderSchedulerPort::CompleteJob(const alcedo::EditorRenderRequest& request,
                                                   bool success, bool frame_submitted,
                                                   std::string message) {
  std::shared_ptr<alcedo::EditorRenderCoordinator> coordinator;
  bool                                             acknowledged = false;
  {
    std::scoped_lock lock(mutex_);
    coordinator = coordinator_.lock();
    if (!success || !frame_submitted) {
      pending_presentations_.erase(request.request_id);
    } else {
      auto it = pending_presentations_.find(request.request_id);
      if (it != pending_presentations_.end()) {
        it->second.frame_submitted = true;
        acknowledged               = it->second.acknowledged;
        if (acknowledged) pending_presentations_.erase(it);
      }
    }
  }
  if (!coordinator) return;
  coordinator->NotifySchedulerCompleted(request.request_id, success, std::move(message));
  if (success && frame_submitted) {
    coordinator->NotifyFrameSubmitted(request.request_id);
    if (acknowledged) coordinator->NotifyFramePresented(request.request_id);
  }
}

}  // namespace alcedo::ui
