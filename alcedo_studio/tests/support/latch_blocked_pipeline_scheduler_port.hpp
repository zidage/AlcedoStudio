//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

/// Test-only scheduler that accepts one job and withholds completion until the
/// test calls Complete(). Ordering is latch-driven; tests must not sleep to
/// prove that a frame is still in flight.

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "app/editor_render_coordinator.hpp"

namespace alcedo::test {

class LatchBlockedPipelineSchedulerPort final : public IEditorPipelineSchedulerPort {
 public:
  auto Schedule(const EditorRenderRequest& request,
                EditorPipelineScheduleCompletion on_complete = {}) -> std::uint64_t override {
    std::scoped_lock lock(mutex_);
    if (running_) {
      ++rejected_while_running_;
      return 0;
    }
    running_      = true;
    job_id_       = ++next_job_id_;
    request_      = request;
    on_complete_  = std::move(on_complete);
    scheduled_.push_back(request);
    return job_id_;
  }

  void Cancel(std::uint64_t job_id) override {
    std::scoped_lock lock(mutex_);
    cancelled_.push_back(job_id);
  }

  void WaitForSessionIdle(std::uint64_t session_epoch) override {
    std::scoped_lock lock(mutex_);
    waited_sessions_.push_back(session_epoch);
  }

  /// Invoke the stored completion on the calling thread. Safe to call once the
  /// test has asserted in-flight state; does nothing when no job is running.
  void Complete(bool success = true, std::string message = {}) {
    EditorPipelineScheduleCompletion on_complete;
    {
      std::scoped_lock lock(mutex_);
      if (!running_) {
        return;
      }
      running_     = false;
      on_complete  = std::move(on_complete_);
      on_complete_ = {};
    }
    if (on_complete) {
      on_complete(success, std::move(message));
    }
  }

  [[nodiscard]] auto running() const -> bool {
    std::scoped_lock lock(mutex_);
    return running_;
  }
  [[nodiscard]] auto scheduled() const -> std::vector<EditorRenderRequest> {
    std::scoped_lock lock(mutex_);
    return scheduled_;
  }
  [[nodiscard]] auto rejected_while_running() const -> int {
    std::scoped_lock lock(mutex_);
    return rejected_while_running_;
  }
  [[nodiscard]] auto last_job_id() const -> std::uint64_t {
    std::scoped_lock lock(mutex_);
    return job_id_;
  }

  std::vector<std::uint64_t> cancelled_;
  std::vector<std::uint64_t> waited_sessions_;

 private:
  mutable std::mutex                   mutex_;
  bool                                 running_                = false;
  std::uint64_t                        next_job_id_            = 0;
  std::uint64_t                        job_id_                 = 0;
  int                                  rejected_while_running_ = 0;
  EditorRenderRequest                  request_{};
  EditorPipelineScheduleCompletion     on_complete_;
  std::vector<EditorRenderRequest>     scheduled_;
};

}  // namespace alcedo::test
