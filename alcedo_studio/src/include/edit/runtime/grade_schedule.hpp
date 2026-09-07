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

/**
 * @brief One compiler stage after runtime Local Laplacian gating and disabled-neighborhood skip.
 *
 * @ref fused_offsets are ParameterArena byte offsets. Neighborhood ops also carry the packed
 * parameters used by separable GPU kernels.
 */
struct GradeScheduledOp {
  CompiledGradeStageKind     kind = CompiledGradeStageKind::Pointwise;
  std::vector<std::uint32_t> fused_offsets;
  GradeNeighborParams        neighbor{};
};

/** @brief Ping-pong destination for one Grade GPU write. */
enum class GradeImageSlot : std::uint8_t { Input, Ping, Pong, Output };

/**
 * @brief Backend-neutral Grade stage list, mix policy, and Local Laplacian slider values.
 *
 * Owner: GradeExecutor for one compiled Color Grade encode. Not persisted.
 */
struct GradeSchedule {
  std::vector<GradeScheduledOp> ops;
  float                         shadows_slider    = 0.0f;
  float                         highlights_slider = 0.0f;
  bool                          local_tone_active = false;
  bool                          alias_to_input    = false;
  bool                          skip_final_mix    = false;
  std::size_t                   gpu_write_count   = 0;
};

/**
 * @brief Comparable Grade decisions for equivalent compiled inputs.
 *
 * Host orchestration must produce the same trace on every backend. Dispatch APIs
 * may differ; these fields may not.
 */
struct GradeDecisionTrace {
  bool                               alias_to_input    = false;
  bool                               skip_final_mix    = false;
  bool                               local_tone_active = false;
  std::vector<CompiledGradeStageKind> op_kinds;
  std::size_t                        fused_command_count = 0;
  std::size_t                        gpu_write_count     = 0;
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
 * @brief Drop inactive Local Laplacian stages, empty Point groups, and adjacent Point groups.
 *
 * @param local_tone_active When false, Local Laplacian stages are removed before Point concatenation.
 */
[[nodiscard]] auto CompactGradeOps(std::vector<GradeScheduledOp> ops, bool local_tone_active)
    -> std::vector<GradeScheduledOp>;

/**
 * @brief GPU writes for compacted ops plus the optional final mix.
 *
 * Each Point, Neighborhood, and Local Laplacian op writes once. Mix adds one write unless skipped.
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
 * @brief Select launches from compiler-fixed stages and already-bound adjustments.
 *
 * @param stages Compiler order. Runtime does not reclassify adjustments into stages.
 * @param bound Bound slots indexed like the compiled adjustment list.
 * @param mix Enabled mix in [0, 1]. Mix 0 aliases the input without GPU work.
 * @param has_mask When true, mix cannot be skipped even at mix 1.
 */
[[nodiscard]] auto MakeGradeSchedule(std::span<const CompiledGradeStage> stages,
                                     std::span<const GradeScheduleInput> bound, float mix,
                                     bool has_mask) -> GradeSchedule;

/** @brief Record the comparable host decisions from @p schedule. */
[[nodiscard]] auto MakeGradeDecisionTrace(const GradeSchedule& schedule) -> GradeDecisionTrace;

/**
 * @brief Bind dirty Grade slots, then select launches from compiled stages.
 *
 * @tparam Backend ParameterArena backend.
 * @param error_prefix Thrown message prefix when compiled adjustments do not match the graph.
 * @return Compacted schedule. Caller uploads dirty slots after this returns.
 */
template <class Backend>
[[nodiscard]] auto BindAndScheduleGrade(ParameterArena<Backend>& arena, ColorGradeNodeModel& grade,
                                        const CompiledGradeNode&           compiled_grade,
                                        const ResolvedRenderGeometry&      geometry,
                                        std::vector<PendingParameterPatch>& pending,
                                        std::string_view                   error_prefix)
    -> GradeSchedule {
  auto Fail = [error_prefix](std::string_view detail) {
    throw std::runtime_error(std::string{error_prefix} + std::string{detail});
  };

  std::vector<GradeScheduleInput> bound;
  bound.reserve(compiled_grade.adjustments.size());
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
    bound.push_back(std::move(input));
  }

  for (const auto& stage : compiled_grade.stages) {
    const auto end = static_cast<std::size_t>(stage.begin) + stage.count;
    if (end > bound.size()) {
      Fail(": compiled grade stage is out of range");
    }
    for (std::uint32_t index = stage.begin; index < stage.begin + stage.count; ++index) {
      const auto expected = bound[index].algorithm;
      if ((stage.kind == CompiledGradeStageKind::Pointwise &&
           expected != CompiledAdjustmentAlgorithm::Pointwise) ||
          (stage.kind == CompiledGradeStageKind::LocalLaplacian &&
           expected != CompiledAdjustmentAlgorithm::LocalLaplacian) ||
          (stage.kind == CompiledGradeStageKind::Neighborhood &&
           expected != CompiledAdjustmentAlgorithm::Neighborhood)) {
        Fail(": compiled grade stage does not match adjustment algorithm");
      }
    }
  }

  const float mix = grade.Enabled() ? grade.Mix() : 0.0f;
  return MakeGradeSchedule(compiled_grade.stages, bound, mix, compiled_grade.mask_stack.has_value());
}

}  // namespace alcedo
