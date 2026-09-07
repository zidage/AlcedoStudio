//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/grade_schedule.hpp"

namespace alcedo {

auto CompactGradeOps(std::vector<GradeScheduledOp> ops, bool local_tone_active)
    -> std::vector<GradeScheduledOp> {
  std::vector<GradeScheduledOp> compacted;
  compacted.reserve(ops.size());
  for (auto& op : ops) {
    if (op.kind == GradeOpKind::LlfBarrier && !local_tone_active) {
      continue;
    }
    if (op.kind == GradeOpKind::Fused && op.fused_offsets.empty()) {
      continue;
    }
    if (op.kind == GradeOpKind::Fused && !compacted.empty() &&
        compacted.back().kind == GradeOpKind::Fused) {
      compacted.back().fused_offsets.insert(compacted.back().fused_offsets.end(),
                                            op.fused_offsets.begin(), op.fused_offsets.end());
      continue;
    }
    compacted.push_back(std::move(op));
  }
  return compacted;
}

auto CountGradeGpuWrites(const std::vector<GradeScheduledOp>& ops, bool skip_final_mix)
    -> std::size_t {
  std::size_t count = skip_final_mix ? 0 : 1;
  for (const auto& op : ops) {
    if (op.kind == GradeOpKind::Fused || op.kind == GradeOpKind::Detail ||
        op.kind == GradeOpKind::LlfBarrier) {
      ++count;
    }
  }
  return count;
}

auto GradeWriteSlots(std::size_t write_count) -> std::vector<GradeImageSlot> {
  std::vector<GradeImageSlot> slots;
  slots.reserve(write_count);
  for (std::size_t index = 0; index < write_count; ++index) {
    if (index + 1 == write_count) {
      slots.push_back(GradeImageSlot::Output);
      continue;
    }
    slots.push_back(index % 2 == 0 ? GradeImageSlot::Ping : GradeImageSlot::Pong);
  }
  return slots;
}

auto MakeGradeSchedule(std::span<const GradeScheduleInput> inputs, float mix, bool has_mask)
    -> GradeSchedule {
  GradeSchedule schedule;
  if (mix == 0.0f) {
    schedule.alias_to_input = true;
    return schedule;
  }

  std::vector<GradeScheduledOp> ops;
  ops.reserve(inputs.size());
  auto FlushFused = [&]() -> GradeScheduledOp& {
    if (ops.empty() || ops.back().kind != GradeOpKind::Fused) {
      ops.push_back(GradeScheduledOp{GradeOpKind::Fused, {}, {}});
    }
    return ops.back();
  };

  for (const auto& input : inputs) {
    if (input.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian) {
      if (input.behavior == AdjustmentBehavior::Shadows) {
        schedule.shadows_slider = input.llf_control;
      } else if (input.behavior == AdjustmentBehavior::Highlights) {
        schedule.highlights_slider = input.llf_control;
      }
      if (ops.empty() || ops.back().kind != GradeOpKind::LlfBarrier) {
        ops.push_back(GradeScheduledOp{GradeOpKind::LlfBarrier, {}, {}});
      }
      continue;
    }
    if (input.algorithm == CompiledAdjustmentAlgorithm::Neighborhood ||
        IsNeighborhoodBehavior(input.behavior)) {
      if (input.neighbor.enabled != 0U) {
        GradeScheduledOp detail;
        detail.kind           = GradeOpKind::Detail;
        detail.fused_offsets  = {input.fused_offset};
        detail.neighbor       = input.neighbor;
        ops.push_back(std::move(detail));
      }
      continue;
    }
    FlushFused().fused_offsets.push_back(input.fused_offset);
  }

  schedule.local_tone_active = local_tone_mapping::ShouldRun(
      schedule.shadows_slider * local_tone_mapping::kHighlightStrengthScale / 80.0f,
      -schedule.highlights_slider * local_tone_mapping::kHighlightStrengthScale / 100.0f);
  schedule.ops            = CompactGradeOps(std::move(ops), schedule.local_tone_active);
  schedule.skip_final_mix = mix == 1.0f && !has_mask;
  if (schedule.ops.empty()) {
    schedule.alias_to_input  = true;
    schedule.skip_final_mix  = false;
    schedule.gpu_write_count = 0;
    return schedule;
  }
  schedule.gpu_write_count = CountGradeGpuWrites(schedule.ops, schedule.skip_final_mix);
  return schedule;
}

auto MakeGradeDecisionTrace(const GradeSchedule& schedule) -> GradeDecisionTrace {
  GradeDecisionTrace trace;
  trace.alias_to_input    = schedule.alias_to_input;
  trace.skip_final_mix    = schedule.skip_final_mix;
  trace.local_tone_active = schedule.local_tone_active;
  trace.gpu_write_count   = schedule.gpu_write_count;
  trace.op_kinds.reserve(schedule.ops.size());
  for (const auto& op : schedule.ops) {
    trace.op_kinds.push_back(op.kind);
    trace.fused_command_count += op.fused_offsets.size();
  }
  return trace;
}

}  // namespace alcedo
