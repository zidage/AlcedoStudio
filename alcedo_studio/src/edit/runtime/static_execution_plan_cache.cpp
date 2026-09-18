//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/static_execution_plan_cache.hpp"

#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {

StaticExecutionPlanCache::StaticExecutionPlanCache(std::uint32_t backend_capability_version)
    : backend_capability_version_(backend_capability_version) {}

auto StaticExecutionPlanCache::GetOrCompile(const PipelineDocument&     document,
                                            const DevelopCompileSource& source) -> ExecutionPlan {
  StaticPlanKey key;
  {
    diag::PreviewCpuInterval plan_key(diag::PreviewCpuStage::PlanKey);
    key = GraphCompiler::MakeStaticPlanKey(document, source, backend_capability_version_);
  }
  if (auto it = plans_.find(key); it != plans_.end()) {
    diag::PreviewCpuInterval lookup(diag::PreviewCpuStage::PlanLookup);
    ++stats_.hits;
    return it->second;
  }
  ++stats_.misses;
  ++stats_.compiles;
  ExecutionPlan plan;
  {
    diag::PreviewCpuInterval compile(diag::PreviewCpuStage::PlanCompile);
    plan = GraphCompiler::CompileStatic(document, source, backend_capability_version_);
  }
  plans_.emplace(key, plan);
  return plan;
}

}  // namespace alcedo
