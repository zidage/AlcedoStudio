//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/input/prepared_raw_input.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/grade_schedule.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/parameter_arena.hpp"

namespace alcedo {

/**
 * @brief Shared Grade encode outcome used by CUDA, OpenCL, and Metal wrappers.
 *
 * Backend result structs copy these fields. Dispatch counts are optional and stay 0
 * when a backend does not track them.
 */
struct GradeExecutionResult {
  GraphValueId         output{NodeId{""}, PortId{"image"}};
  std::uint64_t        lut_resource_id                        = 0;
  std::uint64_t        local_tone_reference_resource_id       = 0;
  bool                 local_tone_rebuilt_reference           = false;
  bool                 local_tone_sampled_canonical_reference = false;
  std::uint32_t        pointwise_dispatch_count               = 0;
  std::uint32_t        detail_pass_count                      = 0;
  std::uint32_t        local_tone_pass_count                  = 0;
  std::uint32_t        local_tone_transient_bytes             = 0;
  std::uint32_t        command_upload_bytes                   = 0;
  GradeDecisionTrace   trace;
};

/**
 * @brief Common Color Grade Point, Neighborhood, mix, and Local Laplacian order.
 *
 * @tparam Ops Backend operations: allocation, fused-command upload, dispatch, and
 *         native error reporting. Shared code owns compiler-stage order, ping-pong,
 *         Neighbor scratch lifetime, and destination choice.
 *
 * @pre Ops::Device exposes Workspace() with IsRendering, Parameters, and Images.
 *      Ops static methods are documented at each call site below.
 */
template <class Ops>
class GradeExecutor {
 public:
  using Device = typename Ops::Device;

  /**
   * @brief Encode one compiled Color Grade with shared host decisions.
   *
   * Disabled or zero-mix Grades alias the input. A failed dispatch throws after
   * Ops::CheckAfterEncode; unpublished canonical LLF writes stay unpublished.
   */
  static auto Execute(Device& device, const ExecutionPlan& plan, const PreparedRawInput& prepared,
                      PipelineDocument& document, const CompiledGradeNode& compiled_grade)
      -> GradeExecutionResult {
    (void)prepared;
    auto& workspace = device.Workspace();
    if (!workspace.IsRendering()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": BeginRender has not been called");
    }
    auto* grade =
        dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(compiled_grade.node_id));
    if (grade == nullptr) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": compiled Color Grade is missing");
    }
    auto* input = workspace.Images().Find(compiled_grade.scene_input);
    if (input == nullptr || input->Empty()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": missing Color Grade scene input");
    }

    GradeExecutionResult result;
    result.output = compiled_grade.scene_output;
    auto& arena   = workspace.Parameters();
    std::size_t slot_count = 0;
    for (const auto& compiled_node : plan.grade_nodes) {
      slot_count += compiled_node.adjustments.size();
    }
    arena.Reserve(slot_count *
                  (kGradeRuntimeParamBytes + ParameterArena<typename Ops::Backend>::kSlotAlignment));

    std::vector<PendingParameterPatch> pending;
    auto schedule =
        BindAndScheduleGrade(arena, *grade, compiled_grade, plan.geometry, pending, Ops::kErrorPrefix);
    result.trace = MakeGradeDecisionTrace(schedule);
    if (schedule.alias_to_input) {
      Ops::AliasOutput(device, compiled_grade.scene_output, compiled_grade.scene_input);
      return result;
    }

    auto& context = device.CommandContext();
    arena.UploadDirty(context);
    for (auto& patch : pending) {
      patch.Commit();
    }

    std::vector<std::uint32_t> fused_offsets;
    std::vector<std::uint32_t> fused_starts;
    fused_starts.reserve(schedule.ops.size());
    for (const auto& op : schedule.ops) {
      fused_starts.push_back(static_cast<std::uint32_t>(fused_offsets.size()));
      fused_offsets.insert(fused_offsets.end(), op.fused_offsets.begin(), op.fused_offsets.end());
    }
    result.command_upload_bytes =
        Ops::UploadFusedCommands(device, grade->Id(), fused_offsets);
    auto lut                 = Ops::LoadLut(device, *grade);
    result.lut_resource_id   = Ops::LutResourceId(lut);

    input = workspace.Images().Find(compiled_grade.scene_input);
    if (input == nullptr) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                               ": Color Grade scene input lost during parameter bind");
    }
    const auto width  = input->Texture().Width();
    const auto height = input->Texture().Height();
    const auto slots  = GradeWriteSlots(schedule.gpu_write_count);
    const GraphValueId ping_id{grade->Id(), PortId{"runtime.ping"}};
    const GraphValueId pong_id{grade->Id(), PortId{"runtime.pong"}};
    bool need_ping = false;
    bool need_pong = false;
    for (const auto slot : slots) {
      need_ping = need_ping || slot == GradeImageSlot::Ping;
      need_pong = need_pong || slot == GradeImageSlot::Pong;
    }
    if (schedule.gpu_write_count > 0) {
      (void)Ops::AcquireOutput(device, compiled_grade.scene_output, width, height);
    }
    typename Ops::Scratch ping = need_ping ? Ops::AcquireScratch(device, width, height, ping_id)
                                           : typename Ops::Scratch{};
    typename Ops::Scratch pong = need_pong ? Ops::AcquireScratch(device, width, height, pong_id)
                                           : typename Ops::Scratch{};

    auto Resolve = [&](GradeImageSlot slot) -> typename Ops::Texture& {
      switch (slot) {
        case GradeImageSlot::Input:
          return Ops::SceneTexture(device, compiled_grade.scene_input);
        case GradeImageSlot::Output:
          return Ops::SceneTexture(device, compiled_grade.scene_output);
        case GradeImageSlot::Ping:
          return Ops::ScratchTexture(ping);
        case GradeImageSlot::Pong:
          return Ops::ScratchTexture(pong);
      }
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": invalid grade image slot");
    };

    auto SlotId = [&](GradeImageSlot slot) -> GraphValueId {
      switch (slot) {
        case GradeImageSlot::Input:
          return compiled_grade.scene_input;
        case GradeImageSlot::Output:
          return compiled_grade.scene_output;
        case GradeImageSlot::Ping:
          return ping_id;
        case GradeImageSlot::Pong:
          return pong_id;
      }
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": invalid grade image slot");
    };

    GradeImageSlot current = GradeImageSlot::Input;
    for (std::size_t index = 0; index < schedule.ops.size(); ++index) {
      const auto& op   = schedule.ops[index];
      const auto  dest = slots[index];
      if (op.kind == CompiledGradeStageKind::Neighborhood) {
        NeighborWork work;
        work.params        = op.neighbor;
        work.owner         = grade->Id();
        work.fused_offset  = op.fused_offsets.empty() ? 0 : op.fused_offsets.front();
        work.command_index = fused_starts[index];
        NeighborExecutor<Ops>::Execute(device, SlotId(current), SlotId(dest), lut, work, width,
                                       height);
        ++result.detail_pass_count;
      } else if (op.kind == CompiledGradeStageKind::LocalLaplacian) {
        auto&      src  = Resolve(current);
        auto&      dst  = Resolve(dest);
        const auto tone =
            Ops::ExecuteLocalTone(device, src, dst, grade->Id(), schedule.shadows_slider,
                                  schedule.highlights_slider, plan.geometry);
        result.local_tone_reference_resource_id       = tone.reference_resource_id;
        result.local_tone_rebuilt_reference           = tone.rebuilt_reference;
        result.local_tone_sampled_canonical_reference = tone.sampled_canonical_reference;
        result.local_tone_transient_bytes             = tone.transient_bytes;
        ++result.local_tone_pass_count;
      } else {
        auto& src = Resolve(current);
        auto& dst = Resolve(dest);
        Ops::DispatchPointwise(device, src, dst, lut, grade->Id(), fused_starts[index],
                               static_cast<std::uint32_t>(op.fused_offsets.size()), width, height);
        ++result.pointwise_dispatch_count;
      }
      current = dest;
    }

    if (!schedule.skip_final_mix) {
      const auto dest                    = slots.back();
      auto&      source                  = Resolve(GradeImageSlot::Input);
      auto&      adjusted                = Resolve(current);
      auto&      destination             = Resolve(dest);
      const typename Ops::Texture* mask = nullptr;
      if (compiled_grade.mask_stack.has_value()) {
        mask = Ops::MaskTexture(device, compiled_grade.mask_output, width, height);
      }
      const float mix = grade->Enabled() ? grade->Mix() : 0.0f;
      Ops::DispatchMix(device, source, adjusted, destination, mix, mask, width, height);
    }
    Ops::CheckAfterEncode(device);
    return result;
  }
};

}  // namespace alcedo
