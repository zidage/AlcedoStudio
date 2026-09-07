//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/execution_plan.hpp"

namespace alcedo {

/**
 * @brief Selected DRT/Post neighborhood launches after disabled adjustments are skipped.
 *
 * Owner: DrtPostExecutor for one compiled DRT encode. Display transform always runs first.
 * Not persisted.
 */
struct DrtPostSchedule {
  std::vector<GradeNeighborParams> enabled;
  bool                             copy_scene_to_post = false;
};

/**
 * @brief Comparable DRT/Post host decisions for equivalent compiled inputs.
 *
 * Backends may differ in kernel APIs; these fields may not.
 */
struct DrtPostDecisionTrace {
  bool        copy_scene_to_post          = false;
  std::size_t enabled_neighborhood_count  = 0;
  bool        display_transform_scheduled = true;
};

/**
 * @brief Keep enabled neighborhood params in compiler order.
 *
 * Disabled adjustments are skipped. An empty enabled list copies the display-transform result to
 * the final display output.
 */
[[nodiscard]] auto MakeDrtPostSchedule(std::span<const GradeNeighborParams> compiled_order)
    -> DrtPostSchedule;

/** @brief Record the comparable host decisions from @p schedule. */
[[nodiscard]] auto MakeDrtPostDecisionTrace(const DrtPostSchedule& schedule) -> DrtPostDecisionTrace;

/**
 * @brief Ping/pong then final-display destinations for enabled neighborhood writes.
 *
 * The first intermediate write is ping. The last write is always @p scene_output.
 */
[[nodiscard]] auto DrtNeighborhoodDestinations(const NodeId& drt_id, const GraphValueId& scene_output,
                                               std::size_t enabled_count)
    -> std::vector<GraphValueId>;

}  // namespace alcedo
