//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// @file harness_completing_pipeline_scheduler_port.hpp
/// @brief Test-only IEditorPipelineSchedulerPort that completes frames via a
/// harness producer. Lives outside production so EditorSessionRenderSchedulerPort
/// never grows test-producer Dispatch branches (Phase R4).
/// Completion is forward-only via the Schedule `on_complete` callback (no reverse
/// SetCompletionNotifier / SetCoordinator plane).

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_render_coordinator.hpp"
#include "app/editor_render_intent.hpp"
#include "edit/frame_presentation_types.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo::test {

using HarnessFrameSinkResolver = std::function<alcedo::IFrameSink*()>;
using HarnessFrameProducer =
    std::function<bool(alcedo::IFrameSink*, const alcedo::EditorRenderRequest&)>;

/// Completing fake for shell / e2e hosts that need FrameReady without RAW decode.
/// Owns BindFrameSubmission / EnsureSize for its producer path (test seam only).
class HarnessCompletingPipelineSchedulerPort final : public alcedo::IEditorPipelineSchedulerPort {
 public:
  void SetSinkResolver(HarnessFrameSinkResolver resolver) {
    std::scoped_lock lock(mutex_);
    sink_resolver_ = std::move(resolver);
  }

  void SetFrameProducer(HarnessFrameProducer producer) {
    std::scoped_lock lock(mutex_);
    producer_ = std::move(producer);
  }

  auto Schedule(const alcedo::EditorRenderRequest& request,
                alcedo::EditorPipelineScheduleCompletion on_complete = {})
      -> std::uint64_t override {
    if (!producer_) {
      return 0;
    }
    Job job;
    {
      std::scoped_lock lock(mutex_);
      if (shutting_down_ || running_job_) {
        return 0;
      }
      job.job_id      = ++next_job_id_;
      job.request     = request;
      job.on_complete = std::move(on_complete);
      running_job_    = job;
    }
    Dispatch(std::move(job));
    return job.job_id;
  }

  void Cancel(std::uint64_t scheduler_job_id) override {
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
  }

  void WaitForSessionIdle(std::uint64_t session_epoch) override {
    const auto idle = [this, session_epoch] {
      return !running_job_ ||
             running_job_->request.intent.image_load_request_id.value != session_epoch;
    };
    std::unique_lock lock(mutex_);
    jobs_changed_.wait(lock, idle);
  }

  void BindSessionContext(std::uint64_t epoch, sl_element_id_t element_id, image_id_t image_id,
                          alcedo::PresentationSinkId presentation_sink_id = 0) override {
    std::scoped_lock lock(mutex_);
    bound_epoch_   = epoch;
    bound_element_ = element_id;
    bound_image_   = image_id;
    bound_sink_id_ = presentation_sink_id;
  }

  void ClearSessionContext() override {
    std::scoped_lock lock(mutex_);
    bound_epoch_   = 0;
    bound_element_ = 0;
    bound_image_   = 0;
    bound_sink_id_ = 0;
  }

 private:
  struct Job {
    std::uint64_t                            job_id = 0;
    alcedo::EditorRenderRequest              request{};
    alcedo::EditorPipelineScheduleCompletion on_complete;
    bool                                     cancelled = false;
  };

  auto EnsurePool() -> std::shared_ptr<alcedo::PipelineScheduler> {
    std::scoped_lock lock(mutex_);
    if (!pool_) {
      pool_ = std::make_shared<alcedo::PipelineScheduler>(1);
    }
    return pool_;
  }

  void Dispatch(Job job) {
    HarnessFrameSinkResolver resolver;
    HarnessFrameProducer     producer;
    {
      std::scoped_lock lock(mutex_);
      resolver = sink_resolver_;
      producer = producer_;
    }
    auto* sink = resolver ? resolver() : nullptr;
    auto  pool = EnsurePool();
    pool->ScheduleWork([this, job = std::move(job), sink, producer = std::move(producer)]() {
      if (IsCancelled(job)) {
        Finish(job, false, "Cancelled during execution");
        return;
      }
      if (!sink || !producer) {
        Finish(job, false, "Harness frame producer is not fully configured");
        return;
      }
      alcedo::FramePreviewMetadata meta;
      meta.frame_role              = job.request.intent.frame_role;
      meta.image_identity          = static_cast<std::uint64_t>(job.request.intent.image_id);
      meta.session_epoch           = job.request.intent.image_load_request_id.value;
      meta.scope_update_allowed    = alcedo::ScopeUpdateAllowedForReason(job.request.intent.reason);
      meta.scope_refresh_requested =
          job.request.intent.reason == alcedo::EditorRenderReason::ScopeRefresh;
      meta.presentation_request_id = job.request.request_id;
      const alcedo::FramePresentationMode mode =
          job.request.intent.frame_role == alcedo::FrameRole::DetailPatch
              ? alcedo::FramePresentationMode::ViewportTransformed
              : alcedo::FramePresentationMode::FullFrame;
      // Test seam owns bind/size; production adapter never does this.
      sink->BindFrameSubmission(alcedo::FrameCompletionSubmission{meta, mode});
      sink->EnsureSize(std::max(1, job.request.intent.requested_width),
                       std::max(1, job.request.intent.requested_height));
      bool        ok = false;
      std::string message;
      try {
        ok      = producer(sink, job.request);
        message = ok ? "Frame ready" : "Harness frame producer failed";
      } catch (const std::exception& ex) {
        message = ex.what();
      } catch (...) {
        message = "Harness frame producer exception";
      }
      if (IsCancelled(job)) {
        Finish(job, false, "Cancelled during execution");
        return;
      }
      Finish(job, ok, std::move(message));
    });
  }

  [[nodiscard]] auto IsCancelled(const Job& job) const -> bool {
    std::scoped_lock lock(mutex_);
    if (shutting_down_) {
      return true;
    }
    if (running_job_ && running_job_->job_id == job.job_id && running_job_->cancelled) {
      return true;
    }
    return job.request.intent.cancellation && job.request.intent.cancellation->IsCancelled();
  }

  void Finish(const Job& job, bool success, std::string message) {
    alcedo::EditorPipelineScheduleCompletion on_complete;
    bool                                     should_complete = false;
    {
      std::scoped_lock lock(mutex_);
      if (running_job_ && running_job_->job_id == job.job_id) {
        on_complete     = std::move(running_job_->on_complete);
        running_job_.reset();
        should_complete = true;
      }
      jobs_changed_.notify_all();
    }
    if (should_complete && on_complete) {
      on_complete(success, std::move(message));
    }
  }

  mutable std::mutex                         mutex_;
  std::condition_variable                    jobs_changed_;
  HarnessFrameSinkResolver                   sink_resolver_;
  HarnessFrameProducer                       producer_;
  std::shared_ptr<alcedo::PipelineScheduler> pool_;
  std::optional<Job>                         running_job_;
  std::uint64_t                              next_job_id_   = 0;
  std::uint64_t                              bound_epoch_   = 0;
  sl_element_id_t                            bound_element_ = 0;
  image_id_t                                 bound_image_   = 0;
  alcedo::PresentationSinkId                 bound_sink_id_ = 0;
  bool                                       shutting_down_ = false;
};

}  // namespace alcedo::test
