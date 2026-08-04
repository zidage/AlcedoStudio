//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

#include "concurrency/thread_pool.hpp"
#include "ui/edit_viewer/frame_sink.hpp"
#include "pipeline_task.hpp"
#include "utils/id/id_generator.hpp"

namespace alcedo {
class PipelineScheduler {
 private:
  IncrID::IDGenerator<uint32_t> id_generator_{0};

  std::mutex                    scheduler_lock_;
  ThreadPool                    thread_pool_;  // use thred pool for now, can be changed to task scheduler later
  std::uint64_t                 next_request_id_{1};
  std::unordered_map<IFrameSink*, std::uint64_t> latest_submitted_request_id_;

  [[nodiscard]] bool IsStaleForSink(IFrameSink* sink, std::uint64_t request_id);
  void               MarkSinkApplyStarted(IFrameSink* sink, std::uint64_t request_id);

 public:
  explicit PipelineScheduler();
  explicit PipelineScheduler(size_t thread_count);

  /**
   * @brief Schedule a pipeline task
   *
   * @param task
   */
  void ScheduleTask(PipelineTask&& task);
};
};  // namespace alcedo