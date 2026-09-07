//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/drt_post_schedule.hpp"

namespace alcedo {

auto MakeDrtPostSchedule(std::span<const GradeNeighborParams> compiled_order) -> DrtPostSchedule {
  DrtPostSchedule schedule;
  schedule.enabled.reserve(compiled_order.size());
  for (const auto& neighbor : compiled_order) {
    if (neighbor.enabled != 0U) {
      schedule.enabled.push_back(neighbor);
    }
  }
  schedule.copy_scene_to_post = schedule.enabled.empty();
  return schedule;
}

auto MakeDrtPostDecisionTrace(const DrtPostSchedule& schedule) -> DrtPostDecisionTrace {
  DrtPostDecisionTrace trace;
  trace.copy_scene_to_post         = schedule.copy_scene_to_post;
  trace.enabled_neighborhood_count = schedule.enabled.size();
  return trace;
}

auto DrtNeighborhoodDestinations(const NodeId& drt_id, const GraphValueId& scene_output,
                                 std::size_t enabled_count) -> std::vector<GraphValueId> {
  std::vector<GraphValueId> destinations;
  destinations.reserve(enabled_count);
  const GraphValueId ping{drt_id, PortId{"runtime.ping"}};
  const GraphValueId pong{drt_id, PortId{"runtime.pong"}};
  GraphValueId       scene_id{};
  for (std::size_t remaining = enabled_count; remaining > 0;) {
    --remaining;
    GraphValueId dest = scene_output;
    if (remaining != 0) {
      dest = scene_id == ping ? pong : ping;
    }
    destinations.push_back(dest);
    scene_id = dest;
  }
  return destinations;
}

}  // namespace alcedo
