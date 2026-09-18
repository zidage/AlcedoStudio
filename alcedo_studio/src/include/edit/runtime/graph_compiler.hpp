//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>

#include "edit/geometry/render_request.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/runtime/develop_compile_source.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Builds Develop -> every backbone Color Grade -> DRT. Does not allocate GPU memory.
 *
 * Validates the document graph and image backbone. Zero Color Grades omits
 * PrimaryColorGrade and feeds DRT from Develop. Each Color Grade is a distinct pass
 * with explicit scene (and optional mask) bindings; the node id need not be grade.primary.
 * SensorDevelop, GeometryResample, and CameraToAp1 are separate compiled passes with
 * distinct GraphValueIds. DRT/Post neighborhood steps and the display transform are
 * recorded on the compiled DRT node.
 *
 * The static plan key covers graph topology, adjustment types and order, source layout,
 * and backend capability version. Viewport, crop, CCT, Grade values, and DRT values are
 * bound per frame and do not recompile the static pass list.
 */
class GraphCompiler {
 public:
  /**
   * @brief Compile pass list and parameter bindings. Does not resolve viewport geometry.
   *
   * @param backend_capability_version Backend trait revision mixed into @ref StaticPlanKey.
   */
  [[nodiscard]] static auto CompileStatic(const PipelineDocument&     document,
                                          const DevelopCompileSource& source,
                                          std::uint32_t backend_capability_version = 0)
      -> ExecutionPlan;

  /**
   * @brief Fill @p plan.geometry from the document crop/rotation and the per-frame request.
   *
   * Does not change @p plan.static_key or the pass list. Must run before execute.
   */
  static void BindFrameGeometry(ExecutionPlan& plan, const PipelineDocument& document,
                                const RenderRequest& request);

  /**
   * @brief CompileStatic plus BindFrameGeometry. Existing tests and one-shot callers use this.
   */
  [[nodiscard]] static auto Compile(const PipelineDocument&     document,
                                    const DevelopCompileSource& source,
                                    const RenderRequest&        request) -> ExecutionPlan;

  /**
   * @brief True when topology, source layout, or backend capability differs from @p previous.
   *
   * Parameter values and viewport are not inputs and never force a recompile.
   */
  [[nodiscard]] static auto NeedsRecompile(const ExecutionPlan&        previous,
                                           const PipelineDocument&     document,
                                           const DevelopCompileSource& source) -> bool;

  [[nodiscard]] static auto MakeStaticPlanKey(const PipelineDocument&     document,
                                              const DevelopCompileSource& source,
                                              std::uint32_t backend_capability_version = 0)
      -> StaticPlanKey;
};

}  // namespace alcedo
