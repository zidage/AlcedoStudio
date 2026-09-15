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

auto DrtWriteSequence(std::size_t neighborhood_count) -> std::vector<DrtWriteTarget> {
  const std::size_t writes = neighborhood_count + 1;
  std::vector<DrtWriteTarget> sequence;
  sequence.reserve(writes);
  auto current = neighborhood_count % 2 == 0 ? DrtWriteTarget::Display : DrtWriteTarget::FreeWorkMember;
  for (std::size_t index = 0; index < writes; ++index) {
    sequence.push_back(current);
    current = current == DrtWriteTarget::Display ? DrtWriteTarget::FreeWorkMember
                                                 : DrtWriteTarget::Display;
  }
  return sequence;
}

}  // namespace alcedo
