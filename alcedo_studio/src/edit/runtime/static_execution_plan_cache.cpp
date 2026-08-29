//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/static_execution_plan_cache.hpp"

namespace alcedo {

StaticExecutionPlanCache::StaticExecutionPlanCache(std::uint32_t backend_capability_version)
    : backend_capability_version_(backend_capability_version) {}

auto StaticExecutionPlanCache::GetOrCompile(const PipelineDocument&     document,
                                            const DevelopCompileSource& source) -> ExecutionPlan {
  const auto key = GraphCompiler::MakeStaticPlanKey(document, source, backend_capability_version_);
  if (auto it = plans_.find(key); it != plans_.end()) {
    ++stats_.hits;
    return it->second;
  }
  ++stats_.misses;
  ++stats_.compiles;
  auto plan = GraphCompiler::CompileStatic(document, source, backend_capability_version_);
  plans_.emplace(key, plan);
  return plan;
}

}  // namespace alcedo
