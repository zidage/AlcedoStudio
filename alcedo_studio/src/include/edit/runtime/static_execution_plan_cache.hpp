//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <map>

#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/graph_compiler.hpp"

namespace alcedo {

/**
 * @brief Cache of static ExecutionPlan values keyed by topology and source layout.
 *
 * Viewport, crop, CCT, Grade, and DRT parameter values are not part of the key.
 * Returned plans are copies; callers bind per-frame geometry without mutating the cache.
 * Not thread-safe.
 */
class StaticExecutionPlanCache {
 public:
  struct Stats {
    std::uint64_t hits     = 0;
    std::uint64_t misses   = 0;
    std::uint64_t compiles = 0;
  };

  /**
   * @param backend_capability_version Mixed into every lookup key so a backend
   *        capability change cannot reuse another backend's pass list.
   */
  explicit StaticExecutionPlanCache(std::uint32_t backend_capability_version = 0);

  /**
   * @brief Return a copy of the cached static plan, compiling on miss.
   *
   * @post Returned plan has empty/default geometry until BindFrameGeometry.
   */
  [[nodiscard]] auto GetOrCompile(const PipelineDocument&     document,
                                  const DevelopCompileSource& source) -> ExecutionPlan;

  [[nodiscard]] auto GetStats() const -> Stats { return stats_; }
  void               ResetStats() { stats_ = {}; }
  [[nodiscard]] auto EntryCount() const -> std::size_t { return plans_.size(); }
  [[nodiscard]] auto BackendCapabilityVersion() const -> std::uint32_t {
    return backend_capability_version_;
  }
  void Clear() { plans_.clear(); }

 private:
  std::uint32_t                     backend_capability_version_ = 0;
  std::map<StaticPlanKey, ExecutionPlan> plans_;
  Stats                             stats_{};
};

}  // namespace alcedo
