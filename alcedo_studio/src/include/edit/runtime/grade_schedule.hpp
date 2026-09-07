//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/grade_parameter_slot.hpp"
#include "edit/runtime/parameter_arena.hpp"

namespace alcedo {

/** @brief One compiled Color Grade GPU stage after fusion and LLF gating. */
enum class GradeOpKind : std::uint8_t { Fused, Detail, LlfBarrier };

/**
 * @brief One fused pointwise group, neighborhood barrier, or LLF barrier.
 *
 * @ref fused_offsets are ParameterArena byte offsets. Detail ops also carry the
 * packed neighborhood parameters used by separable GPU kernels.
 */
struct GradeScheduledOp {
  GradeOpKind                kind = GradeOpKind::Fused;
  std::vector<std::uint32_t> fused_offsets;
  GradeNeighborParams        neighbor{};
};

/** @brief Ping-pong destination for one Grade GPU write. */
enum class GradeImageSlot : std::uint8_t { Input, Ping, Pong, Output };

/**
 * @brief Backend-neutral Grade stage list, mix policy, and LLF slider values.
 *
 * Owner: GradeExecutor for one compiled Color Grade encode. Not persisted.
 */
struct GradeSchedule {
  std::vector<GradeScheduledOp> ops;
  float                         shadows_slider     = 0.0f;
  float                         highlights_slider  = 0.0f;
  bool                          local_tone_active  = false;
  bool                          alias_to_input     = false;
  bool                          skip_final_mix     = false;
  std::size_t                   gpu_write_count    = 0;
};

/**
 * @brief Comparable Grade decisions for equivalent compiled inputs.
 *
 * Host orchestration must produce the same trace on every backend. Dispatch APIs
 * may differ; these fields may not.
 */
struct GradeDecisionTrace {
  bool                     alias_to_input    = false;
  bool                     skip_final_mix    = false;
  bool                     local_tone_active = false;
  std::vector<GradeOpKind> op_kinds;
  std::size_t              fused_command_count = 0;
  std::size_t              gpu_write_count     = 0;
};

/** @brief One compiled adjustment after its GPU slot has been bound. */
struct GradeScheduleInput {
  CompiledAdjustmentAlgorithm algorithm    = CompiledAdjustmentAlgorithm::Pointwise;
  AdjustmentBehavior          behavior     = AdjustmentBehavior::Exposure;
  std::uint32_t               fused_offset = 0;
  GradeNeighborParams         neighbor{};
  float                       llf_control = 0.0f;
};

/**
 * @brief Drop inactive LLF barriers, empty fused groups, and adjacent fused groups.
 *
 * @param local_tone_active When false, LLF barriers are removed before fusion.
 */
[[nodiscard]] auto CompactGradeOps(std::vector<GradeScheduledOp> ops, bool local_tone_active)
    -> std::vector<GradeScheduledOp>;

/**
 * @brief GPU writes for compacted ops plus the optional final mix.
 *
 * Each fused, detail, and LLF op writes once. Mix adds one write unless skipped.
 */
[[nodiscard]] auto CountGradeGpuWrites(const std::vector<GradeScheduledOp>& ops, bool skip_final_mix)
    -> std::size_t;

/**
 * @brief Ping-pong destinations for @p write_count GPU writes.
 *
 * The last write is always Output. Earlier writes alternate Ping/Pong starting at Ping.
 */
[[nodiscard]] auto GradeWriteSlots(std::size_t write_count) -> std::vector<GradeImageSlot>;

/**
 * @brief Build a Grade schedule from already-bound compiled adjustments.
 *
 * @param mix Enabled mix in [0, 1]. Mix 0 aliases the input without GPU work.
 * @param has_mask When true, mix cannot be skipped even at mix 1.
 */
[[nodiscard]] auto MakeGradeSchedule(std::span<const GradeScheduleInput> inputs, float mix,
                                     bool has_mask) -> GradeSchedule;

/** @brief Record the comparable host decisions from @p schedule. */
[[nodiscard]] auto MakeGradeDecisionTrace(const GradeSchedule& schedule) -> GradeDecisionTrace;

/**
 * @brief Bind dirty Grade slots, then build the shared host schedule.
 *
 * @tparam Backend ParameterArena backend.
 * @param error_prefix Thrown message prefix when compiled adjustments do not match the graph.
 * @return Compacted schedule. Caller uploads dirty slots after this returns.
 */
template <class Backend>
[[nodiscard]] auto BindAndScheduleGrade(ParameterArena<Backend>& arena, ColorGradeNodeModel& grade,
                                        const CompiledGradeNode&       compiled_grade,
                                        const ResolvedRenderGeometry&  geometry,
                                        std::vector<PendingParameterPatch>& pending,
                                        std::string_view               error_prefix)
    -> GradeSchedule {
  auto Fail = [error_prefix](std::string_view detail) {
    throw std::runtime_error(std::string{error_prefix} + std::string{detail});
  };

  std::vector<GradeScheduleInput> inputs;
  inputs.reserve(compiled_grade.adjustments.size());
  for (const auto& compiled : compiled_grade.adjustments) {
    auto* model = grade.FindAdjustment(compiled.instance_id);
    if (model == nullptr || model->Type() != compiled.type) {
      Fail(": compiled adjustment no longer matches graph");
    }
    const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (!behavior.has_value()) {
      Fail(": unregistered adjustment type '" + std::string{compiled.type.Text()} + "'");
    }
    if (IsLocalToneBehavior(*behavior) &&
        compiled.algorithm != CompiledAdjustmentAlgorithm::LocalLaplacian) {
      Fail(": Shadows/Highlights were not compiled for LLF");
    }
    if (compiled.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian &&
        !IsLocalToneBehavior(*behavior)) {
      Fail(": non-local adjustment was compiled for LLF");
    }
    const ParameterSlotKey key{grade.Id(), compiled.instance_id};
    if (auto change = BindOrRefreshGradeRuntimeSlot(arena, key, *model, *behavior)) {
      pending.push_back(std::move(*change));
    }

    GradeScheduleInput input;
    input.algorithm    = compiled.algorithm;
    input.behavior     = *behavior;
    input.fused_offset = arena.Binding(key).offset;
    if (compiled.algorithm == CompiledAdjustmentAlgorithm::LocalLaplacian) {
      input.llf_control = PackedGradeControlValue(arena, key);
    } else if (compiled.algorithm == CompiledAdjustmentAlgorithm::Neighborhood ||
               IsNeighborhoodBehavior(*behavior)) {
      input.neighbor = MakeGradeNeighborParams(*model, *behavior, geometry);
    } else if (compiled.algorithm != CompiledAdjustmentAlgorithm::Pointwise) {
      Fail(": unsupported grade algorithm");
    }
    inputs.push_back(std::move(input));
  }

  const float mix = grade.Enabled() ? grade.Mix() : 0.0f;
  return MakeGradeSchedule(inputs, mix, compiled_grade.mask_stack.has_value());
}

}  // namespace alcedo
