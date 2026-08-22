//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "edit/geometry/render_request.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/develop_compile_source.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Builds a Develop-only ExecutionPlan. Does not allocate GPU memory.
 *
 * Validates the three-node document. ColorGrade and DRT are accepted as graph
 * nodes but are not emitted as GPU passes in G4. Camera-to-AP1 is not a pass.
 *
 * Recompile when topology is dirty, input kind changes, or decoded extent
 * changes. Highlight / demosaic / lens flags are encoder branches, not new
 * pass lists.
 */
class GraphCompiler {
 public:
  [[nodiscard]] static auto Compile(const PipelineDocument&     document,
                                    const DevelopCompileSource& source,
                                    const RenderRequest&        request) -> ExecutionPlan;

  [[nodiscard]] static auto NeedsRecompile(const ExecutionPlan&        previous,
                                           const PipelineDocument&     document,
                                           const DevelopCompileSource& source) -> bool;
};

}  // namespace alcedo
