//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_session_production.hpp"

#include <algorithm>
#include <thread>
#include <utility>

#include "edit/frame_presentation_types.hpp"
#include "edit/pipeline/pipeline_cpu.hpp"
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
  meta.frame_role          = intent.frame_role;
  meta.preview_generation  = intent.render_generation;
  meta.detail_serial       = 0;
  return meta;
}

}  // namespace

// ── Pipeline port ───────────────────────────────────────────────────────────

void EditorSessionProductionPipelinePort::SetServices(EditorSessionProductionServices services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

auto EditorSessionProductionPipelinePort::Acquire(sl_element_id_t element_id, std::string* /*error*/)
    -> alcedo::EditorPipelineGuardHandle {
  // Intentionally does not call LoadPipeline here. Open must stay non-blocking
  // for shell/synthetic ids; first-frame production loads the real guard on demand.
  return alcedo::EditorPipelineGuardHandle{element_id, true};
}

void EditorSessionProductionPipelinePort::Release(const alcedo::EditorPipelineGuardHandle& guard) {
  if (!guard.valid) {
    return;
  }
  std::shared_ptr<alcedo::PipelineGuard> held;
  std::shared_ptr<alcedo::PipelineMgmtService> service;
  {
    std::scoped_lock lock(mutex_);
    auto             it = guards_.find(guard.element_id);
    if (it != guards_.end()) {
      held = it->second;
      guards_.erase(it);
    }
    if (services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (service && held) {
    try {
      service->SavePipeline(held);
    } catch (...) {
    }
  }
}

auto EditorSessionProductionPipelinePort::CurrentGuard(sl_element_id_t element_id) const
    -> std::shared_ptr<alcedo::PipelineGuard> {
  std::scoped_lock lock(mutex_);
  auto             it = guards_.find(element_id);
  return it == guards_.end() ? nullptr : it->second;
}

auto EditorSessionProductionPipelinePort::EnsureLoaded(sl_element_id_t element_id,
                                                       std::string*    error)
    -> std::shared_ptr<alcedo::PipelineGuard> {
  {
    std::scoped_lock lock(mutex_);
    auto             it = guards_.find(element_id);
    if (it != guards_.end()) {
      return it->second;
    }
  }
  std::shared_ptr<alcedo::PipelineMgmtService> service;
  {
    std::scoped_lock lock(mutex_);
    if (services_.pipeline_service) {
      service = services_.pipeline_service();
    }
  }
  if (!service) {
    if (error) {
      *error = "Pipeline service is unavailable";
    }
    return nullptr;
  }
  try {
    auto guard = service->LoadPipeline(element_id);
    if (!guard || !guard->pipeline_) {
      if (error) {
        *error = "Failed to load pipeline for editor session";
      }
      return nullptr;
    }
    std::scoped_lock lock(mutex_);
    guards_[element_id] = guard;
    return guard;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return nullptr;
  } catch (...) {
    if (error) {
      *error = "Unknown pipeline load failure";
    }
    return nullptr;
  }
}

// ── History port ────────────────────────────────────────────────────────────

void EditorSessionProductionHistoryPort::SetServices(EditorSessionProductionServices services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

auto EditorSessionProductionHistoryPort::Acquire(sl_element_id_t element_id, std::string* /*error*/)
    -> alcedo::EditorHistoryGuardHandle {
  // Mirror pipeline: non-blocking open; real LoadHistory is on demand later.
  return alcedo::EditorHistoryGuardHandle{element_id, true};
}

void EditorSessionProductionHistoryPort::Release(const alcedo::EditorHistoryGuardHandle& guard) {
  if (!guard.valid) {
    return;
  }
  std::shared_ptr<alcedo::EditHistoryGuard> held;
  std::shared_ptr<alcedo::EditHistoryMgmtService> service;
  {
    std::scoped_lock lock(mutex_);
    auto             it = guards_.find(guard.element_id);
    if (it != guards_.end()) {
      held = it->second;
      guards_.erase(it);
    }
    if (services_.history_service) {
      service = services_.history_service();
    }
  }
  if (service && held) {
    try {
      service->SaveHistory(held);
    } catch (...) {
    }
  }
}

auto EditorSessionProductionHistoryPort::Undo(const alcedo::EditorHistoryGuardHandle& /*guard*/,
                                              std::string* /*error*/) -> bool {
  // Full undo/redo against WorkingVersion + pipeline is Phase 5C. Phase 5B only
  // needs the history guard so open/first-frame can run.
  return true;
}

auto EditorSessionProductionHistoryPort::Redo(const alcedo::EditorHistoryGuardHandle& /*guard*/,
                                              std::string* /*error*/) -> bool {
  return true;
}

auto EditorSessionProductionHistoryPort::ReadAdjustmentSnapshot(
    const alcedo::EditorHistoryGuardHandle& /*guard*/,
    alcedo::EditorRenderAdjustmentSnapshot* snapshot, std::string* /*error*/) -> bool {
  if (snapshot) {
    // Full history→adjustment mapping is Phase 5C/6; empty means unedited.
    *snapshot = {};
  }
  return true;
}

// ── Production scheduler ────────────────────────────────────────────────────

EditorSessionProductionSchedulerPort::EditorSessionProductionSchedulerPort(
    std::shared_ptr<alcedo::PipelineScheduler> pipeline_scheduler)
    : pipeline_scheduler_(std::move(pipeline_scheduler)) {
  // Lazy-create the pipeline scheduler on first real produce so shell host
  // construction never starts a worker thread pool.
}

EditorSessionProductionSchedulerPort::~EditorSessionProductionSchedulerPort() {
  std::vector<std::jthread> workers;
  std::vector<std::shared_ptr<alcedo::EditorRenderCancellationToken>> cancellations;
  {
    std::scoped_lock lock(mutex_);
    shutting_down_ = true;
    for (auto& entry : jobs_) {
      auto& job = entry.second;
      job.cancelled = true;
      if (job.request.intent.cancellation) {
        cancellations.push_back(job.request.intent.cancellation);
      }
    }
    sink_resolver_ = {};
    workers.swap(workers_);
  }
  for (const auto& cancellation : cancellations) {
    cancellation->Cancel();
  }
  // jthread destruction joins outside mutex_, allowing workers to finish their
  // cancellation/error bookkeeping without a teardown deadlock.
  workers.clear();
}

void EditorSessionProductionSchedulerPort::SetCoordinator(
    std::weak_ptr<alcedo::EditorRenderCoordinator> coordinator) {
  std::scoped_lock lock(mutex_);
  coordinator_ = std::move(coordinator);
}

void EditorSessionProductionSchedulerPort::SetSinkResolver(EditorFrameSinkResolver resolver) {
  std::scoped_lock lock(mutex_);
  sink_resolver_ = std::move(resolver);
}

void EditorSessionProductionSchedulerPort::SetPipelinePort(
    std::shared_ptr<EditorSessionProductionPipelinePort> pipeline_port) {
  std::scoped_lock lock(mutex_);
  pipeline_port_ = std::move(pipeline_port);
}

void EditorSessionProductionSchedulerPort::SetServices(EditorSessionProductionServices services) {
  std::scoped_lock lock(mutex_);
  services_ = std::move(services);
}

void EditorSessionProductionSchedulerPort::SetTestFrameProducer(EditorTestFrameProducer producer) {
  std::scoped_lock lock(mutex_);
  test_producer_ = std::move(producer);
}

auto EditorSessionProductionSchedulerPort::Schedule(const alcedo::EditorRenderRequest& request)
    -> std::uint64_t {
  Job  job;
  bool has_producer = false;
  {
    std::scoped_lock lock(mutex_);
    if (shutting_down_) {
      return 0;
    }
    job.job_id        = ++next_job_id_;
    job.request       = request;
    jobs_[job.job_id] = job;
    scheduled_.push_back(request);

    // Prefer an injected test producer (shell / integration tests). Real pipeline
    // only when the image pool can resolve a descriptor with a path.
    if (test_producer_) {
      has_producer = true;
    } else if (services_.image_pool) {
      try {
        if (auto pool = services_.image_pool()) {
          const auto img = pool->Read<std::shared_ptr<alcedo::Image>>(
              request.intent.image_id,
              [](const std::shared_ptr<alcedo::Image>& image) { return image; });
          has_producer = static_cast<bool>(img) && !img->image_path_.empty();
        }
      } catch (...) {
        has_producer = false;
      }
    }
  }

  // Shell / synthetic ids: accept the job like the bootstrap scheduler and leave
  // it in-flight so the session stays Loading without a fake Failed transition.
  if (!has_producer) {
    return job.job_id;
  }

  {
    std::scoped_lock lock(mutex_);
    auto it = jobs_.find(job.job_id);
    if (it != jobs_.end()) {
      it->second.running = true;
    }
  }

  // Complete asynchronously so the coordinator can mark the request in-flight
  // before NotifySchedulerCompleted runs (Schedule is called under ScheduleNext).
  std::jthread worker([this, job]() mutable {
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
    // Cancellation can re-enter scheduler/coordinator code, and local jthread
    // destruction joins. Both must happen after releasing mutex_.
    if (job.request.intent.cancellation) {
      job.request.intent.cancellation->Cancel();
    }
    return 0;
  }
  return job.job_id;
}

void EditorSessionProductionSchedulerPort::Cancel(std::uint64_t scheduler_job_id) {
  std::shared_ptr<alcedo::EditorRenderCancellationToken> cancellation;
  {
    std::scoped_lock lock(mutex_);
    auto             it = jobs_.find(scheduler_job_id);
    if (it == jobs_.end()) {
      return;
    }
    it->second.cancelled = true;
    cancellation = it->second.request.intent.cancellation;
    pending_presentations_.erase(it->second.request.request_id);
    if (!it->second.running) {
      jobs_.erase(it);
      jobs_changed_.notify_all();
    }
  }
  // Cancellation callbacks can re-enter the coordinator and this scheduler.
  // Never invoke them while holding mutex_.
  if (cancellation) {
    cancellation->Cancel();
  }
}

void EditorSessionProductionSchedulerPort::WaitForSessionIdle(
    std::uint64_t session_generation) {
  std::unique_lock lock(mutex_);
  jobs_changed_.wait(lock, [&] {
    return std::none_of(jobs_.begin(), jobs_.end(), [&](const auto& entry) {
      return entry.second.request.intent.session_generation == session_generation;
    });
  });
}

void EditorSessionProductionSchedulerPort::RemoveJob(std::uint64_t job_id) {
  std::scoped_lock lock(mutex_);
  jobs_.erase(job_id);
  jobs_changed_.notify_all();
}

void EditorSessionProductionSchedulerPort::NotifyPresentationAcknowledged(
    std::uint64_t request_id, std::uint64_t image_generation,
    std::uint64_t image_identity) {
  bool notify = false;
  std::shared_ptr<alcedo::EditorRenderCoordinator> coordinator;
  {
    std::scoped_lock lock(mutex_);
    auto it = pending_presentations_.find(request_id);
    if (it == pending_presentations_.end()) {
      return;
    }
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
  if (notify && coordinator) {
    coordinator->NotifyFramePresented(request_id);
  }
}

auto EditorSessionProductionSchedulerPort::last_scheduled() const
    -> std::vector<alcedo::EditorRenderRequest> {
  std::scoped_lock lock(mutex_);
  return scheduled_;
}

auto EditorSessionProductionSchedulerPort::pending_present_request_id() const -> std::uint64_t {
  std::scoped_lock lock(mutex_);
  return pending_presentations_.empty() ? 0 : pending_presentations_.begin()->first;
}

void EditorSessionProductionSchedulerPort::ExecuteJob(Job job) {
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

  alcedo::IFrameSink* sink = nullptr;
  EditorFrameSinkResolver resolver;
  EditorTestFrameProducer test_producer;
  {
    std::scoped_lock lock(mutex_);
    resolver      = sink_resolver_;
    test_producer = test_producer_;
  }
  if (resolver) {
    sink = resolver();
  }

  // No presentation sink yet: accept schedule for shell routing but do not
  // claim success. Session stays Loading until a later retry with a bound sink.
  if (!sink) {
    // When presentation target is not bound, treat as deferred failure so the
    // coordinator can start the next request. Open path typically has sink id
    // stamped before Schedule; missing sink is a real error.
    RemoveJob(job.job_id);
    CompleteJob(job.request, false, false, "No presentation frame sink bound");
    return;
  }

  // One-shot first-frame composition only. QualityBase / DetailPatch / superseded
  // interactive frames must not accumulate in an application-level pending map.
  if (auto* direct_sink = dynamic_cast<alcedo::editor_rhi::DirectFrameSink*>(sink)) {
    direct_sink->SetFirstFrameCompositionCallback(
        [weak = weak_from_this()](std::uint64_t request_id, std::uint64_t image_generation,
                                 std::uint64_t image_identity) {
          if (const auto self = weak.lock()) {
            self->NotifyPresentationAcknowledged(request_id, image_generation, image_identity);
          }
        });
  }

  const int width  = std::max(1, job.request.intent.requested_width);
  const int height = std::max(1, job.request.intent.requested_height);
  sink->EnsureSize(width, height);

  alcedo::FramePreviewMetadata meta = FrameRoleToPreviewMetadata(job.request.intent);
  meta.presentation_request_id = job.request.request_id;
  sink->SetNextFramePreviewMetadata(meta);
  sink->SetNextFramePresentationMode(alcedo::FramePresentationMode::FullFrame);

  auto* direct_sink = dynamic_cast<alcedo::editor_rhi::DirectFrameSink*>(sink);
  const auto submitted_before = direct_sink ? direct_sink->submitted_frame_count() : 0;

  std::string error;
  bool        submitted = false;
  bool        ok        = false;

  // Only InteractivePrimary first-frame work is tracked for composition. Pipeline
  // scheduling capacity is released on complete/submit without waiting for a
  // window-frame confirmation for every request.
  const bool track_first_composition =
      job.request.intent.frame_role == alcedo::FrameRole::InteractivePrimary;
  if (track_first_composition) {
    std::scoped_lock lock(mutex_);
    pending_presentations_[job.request.request_id] = PendingPresentation{
        job.request.intent.session_generation, job.request.intent.image_id, false, false};
  }

  if (test_producer) {
    ok        = test_producer(sink, job.request);
    submitted = ok && (!direct_sink || direct_sink->submitted_frame_count() > submitted_before);
    if (!ok) {
      error = "Test frame producer failed";
    }
  } else {
    ok = TryProducePipelineFrame(job.request, sink, &error);
    // Pipeline writes through Map/Unmap/NotifyFrameReady when successful.
    submitted = ok && (!direct_sink || direct_sink->submitted_frame_count() > submitted_before);
    if (ok && !submitted) {
      ok = false;
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
      if (!ok || !submitted) {
        pending_presentations_.erase(job.request.request_id);
      }
      jobs_.erase(job.job_id);
      jobs_changed_.notify_all();
    }
  }
  if (cancelled_during_execution) {
    CompleteJob(job.request, false, false, "Cancelled during execution");
    return;
  }

  CompleteJob(job.request, ok, submitted, error.empty() ? (ok ? "Render completed" : "Render failed")
                                                        : error);
}

auto EditorSessionProductionSchedulerPort::TryProducePipelineFrame(
    const alcedo::EditorRenderRequest& request, alcedo::IFrameSink* sink, std::string* error)
    -> bool {
  if (!sink) {
    if (error) {
      *error = "Presentation sink is null";
    }
    return false;
  }

  std::shared_ptr<EditorSessionProductionPipelinePort> pipeline_port;
  EditorSessionProductionServices                      services;
  std::shared_ptr<alcedo::PipelineScheduler>           scheduler;
  {
    std::scoped_lock lock(mutex_);
    pipeline_port = pipeline_port_;
    services      = services_;
    if (!pipeline_scheduler_) {
      pipeline_scheduler_ = std::make_shared<alcedo::PipelineScheduler>(1);
    }
    scheduler = pipeline_scheduler_;
  }

  if (!pipeline_port || !scheduler) {
    if (error) {
      *error = "Production pipeline scheduler is not fully configured";
    }
    return false;
  }

  auto guard = pipeline_port->EnsureLoaded(request.intent.element_id, error);
  if (!guard || !guard->pipeline_) {
    if (error && error->empty()) {
      *error = "No pipeline guard for image; open may lack a project";
    }
    return false;
  }

  std::shared_ptr<alcedo::ImagePoolService> image_pool;
  if (services.image_pool) {
    image_pool = services.image_pool();
  }
  if (!image_pool) {
    if (error) {
      *error = "Image pool is unavailable";
    }
    return false;
  }

  try {
    auto exec = guard->pipeline_;
    controllers::EnsureLoadingOperatorDefaults(exec);
    controllers::AttachExecutionStages(exec, sink);

    try {
      auto img = image_pool->Read<std::shared_ptr<alcedo::Image>>(
          request.intent.image_id,
          [](const std::shared_ptr<alcedo::Image>& image) { return image; });
      if (img && img->HasRawColorContext()) {
        exec->InjectRawMetadata(img->GetRawColorContext());
      }
    } catch (...) {
    }

    alcedo::PipelineTask task;
    task.input_             = controllers::LoadImageInputBuffer(image_pool, request.intent.image_id);
    task.pipeline_executor_ = exec;
    task.options_.render_desc_.render_type_         = RenderTypeForIntent(request.intent);
    // Phase 5D A5: a DetailPatch (Detail quality) must load the visible viewport
    // ROI from the executor/sink so the produced frame carries the correct
    // source_roi_norm. Full-frame renders (Interactive/Quality) keep the whole
    // frame; AttachExecutionStages wired the executor's GetViewportRenderRegion
    // to the DirectFrameSink, whose region the interaction controller keeps
    // current via applyViewStateToViewport before the render intent is submitted.
    task.options_.render_desc_.use_viewport_region_ =
        request.intent.quality == alcedo::EditorRenderQuality::Detail;
    task.options_.render_desc_.frame_metadata_      = FrameRoleToPreviewMetadata(request.intent);
    task.options_.is_callback_                      = false;
    task.options_.is_seq_callback_                  = false;
    task.options_.is_blocking_                      = true;

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
      if (error) {
        *error = "Pipeline returned an empty result";
      }
      return false;
    }
    return true;
  } catch (const std::exception& ex) {
    if (error) {
      *error = ex.what();
    }
    return false;
  } catch (...) {
    if (error) {
      *error = "Pipeline render failed";
    }
    return false;
  }
}

void EditorSessionProductionSchedulerPort::CompleteJob(const alcedo::EditorRenderRequest& request,
                                                       bool success, bool frame_submitted,
                                                       std::string message) {
  std::shared_ptr<alcedo::EditorRenderCoordinator> coordinator;
  {
    std::scoped_lock lock(mutex_);
    coordinator = coordinator_.lock();
    if (!success || !frame_submitted) {
      pending_presentations_.erase(request.request_id);
    }
  }
  if (!coordinator) {
    return;
  }
  coordinator->NotifySchedulerCompleted(request.request_id, success, std::move(message));
  if (success && frame_submitted) {
    coordinator->NotifyFrameSubmitted(request.request_id);
    bool acknowledged = false;
    {
      std::scoped_lock lock(mutex_);
      auto it = pending_presentations_.find(request.request_id);
      if (it != pending_presentations_.end()) {
        it->second.frame_submitted = true;
        acknowledged = it->second.acknowledged;
        if (acknowledged) {
          pending_presentations_.erase(it);
        }
      }
    }
    if (acknowledged) {
      coordinator->NotifyFramePresented(request.request_id);
    }
  }
}

}  // namespace alcedo::ui
