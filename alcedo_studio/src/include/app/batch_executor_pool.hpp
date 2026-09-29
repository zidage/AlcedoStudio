//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <iterator>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "edit/pipeline/pipeline_accelerator.hpp"
#include "edit/pipeline/pipeline_executor.hpp"
#include "edit/runtime/executor_role.hpp"

namespace alcedo {

/**
 * @brief Fixed set of batch executors that one service owns for its render workers.
 *
 * Owner: one non-editor render service (ThumbnailService, ExportService). A render takes an idle
 * executor for the whole task and returns it afterwards, so no two renders use one executor at a
 * time. Each owner gives its scheduler one worker per executor, so a take never waits in practice.
 * Executors are created on first take with the accelerator preference captured at construction; a
 * preference whose backend is unavailable fails that render with the real error. Each batch render
 * releases its resources when it ends (ExecutorRole::Batch), so an idle executor holds only its
 * device.
 *
 * Thread: Take and Return are safe to call from any thread.
 */
class BatchExecutorPool {
 public:
  BatchExecutorPool(std::size_t count, AcceleratorBackendPreference preference)
      : preference_(preference),
        slots_(std::max<std::size_t>(1, count)),
        idle_(slots_.size(), true) {}

  [[nodiscard]] auto Size() const -> std::size_t { return slots_.size(); }

  /// Take an idle executor, creating it on first use. Blocks while every executor renders.
  auto               Take() -> std::pair<std::size_t, std::shared_ptr<PipelineExecutor>> {
    std::unique_lock lock(mutex_);
    std::size_t      index = 0;
    cv_.wait(lock, [&] {
      const auto it = std::find(idle_.begin(), idle_.end(), true);
      index         = static_cast<std::size_t>(std::distance(idle_.begin(), it));
      return it != idle_.end();
    });
    if (!slots_[index]) {
      auto executor = std::make_shared<PipelineExecutor>(ExecutorRole::Batch);
      executor->SetAcceleratorBackendPreference(preference_);
      slots_[index] = std::move(executor);
    }
    idle_[index] = false;
    return {index, slots_[index]};
  }

  /// Return the executor taken at @p index.
  void Return(std::size_t index) {
    {
      std::scoped_lock lock(mutex_);
      idle_[index] = true;
    }
    cv_.notify_one();
  }

 private:
  AcceleratorBackendPreference                   preference_;
  std::mutex                                     mutex_;
  std::condition_variable                        cv_;
  std::vector<std::shared_ptr<PipelineExecutor>> slots_;
  std::vector<bool>                              idle_;
};

}  // namespace alcedo
