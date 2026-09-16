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
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/grade_schedule.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/gpu_work_sample.hpp"
#include "utils/diagnostics/preview_performance.hpp"

namespace alcedo {

/**
 * @brief Shared Grade encode outcome used by CUDA, OpenCL, and Metal wrappers.
 *
 * Backend result structs copy these fields. Dispatch counts are optional and stay 0
 * when a backend does not track them. @ref output_binding is the physical scene
 * location after this Grade; @ref output remains the compiled logical GraphValueId.
 */
struct GradeExecutionResult {
  GraphValueId         output{NodeId{""}, PortId{"image"}};
  FrameSceneBinding    output_binding{};
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
 * @tparam Ops Backend operations: fused-command upload, dispatch, and native error
 *         reporting. Shared code owns compiler-stage order, the two scene-work
 *         members, Neighbor scratch lifetime, and Mix fusion into the last write.
 *
 * @pre Ops::Device exposes Workspace() with IsRendering, Parameters, and SceneWork.
 */
template <class Ops>
class GradeExecutor {
 public:
  using Device = typename Ops::Device;

  /**
   * @brief Encode one compiled Color Grade with shared host decisions.
   *
   * Disabled or zero-mix Grades return @p scene without a pixel copy. A failed
   * dispatch throws after Ops::CheckAfterEncode; unpublished canonical LLF writes
   * stay unpublished. Grade scene_output is never acquired or published.
   */
  static auto Execute(Device& device, const ExecutionPlan& plan, const PreparedRawInput& prepared,
                      PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                      const FrameSceneBinding& scene) -> GradeExecutionResult {
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

    GradeExecutionResult result;
    result.output          = compiled_grade.scene_output;
    result.output_binding  = scene;
    auto& arena            = workspace.Parameters();
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
      diag::PreviewPerformance::SetPassState(diag::PreviewExecutionState::Aliased);
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
    result.command_upload_bytes = Ops::UploadFusedCommands(device, grade->Id(), fused_offsets);
    auto lut                    = Ops::LoadLut(device, *grade);
    result.lut_resource_id      = Ops::LutResourceId(lut);

    const auto width  = Ops::BindingWidth(device, scene);
    const auto height = Ops::BindingHeight(device, scene);
    const auto dest   = DestinationWorkMember(scene);
    const auto dest_binding = FrameSceneBinding::WorkImage(dest);
    const float mix = grade->Enabled() ? grade->Mix() : 0.0f;
    const bool  needs_mix = !schedule.skip_final_mix;
    const GraphValueId* mask_id =
        compiled_grade.mask_stack.has_value() ? &compiled_grade.mask_output : nullptr;

    bool wrote_dest = false;
    for (std::size_t index = 0; index < schedule.ops.size(); ++index) {
      const auto& op   = schedule.ops[index];
      const bool  last = index + 1 == schedule.ops.size();
      const auto  src  = wrote_dest ? dest_binding : scene;
      if (op.kind == CompiledGradeStageKind::Neighborhood) {
        diag::PreviewSubStageInterval neighborhood(diag::PreviewSubStageKind::Neighborhood);
        GpuWorkSample<Device> gpu(device);
        NeighborWork work;
        work.params        = op.neighbor;
        work.owner         = grade->Id();
        work.fused_offset  = op.fused_offsets.empty() ? 0 : op.fused_offsets.front();
        work.command_index = fused_starts[index];
        const float stage_mix          = last ? mix : 1.0f;
        const GraphValueId* stage_mask = last && needs_mix ? mask_id : nullptr;
        NeighborExecutor<Ops>::Execute(device, src, dest_binding, scene, lut, work, stage_mix,
                                       stage_mask, width, height);
        ++result.detail_pass_count;
        wrote_dest = true;
      } else if (op.kind == CompiledGradeStageKind::LocalLaplacian) {
        const float stage_mix          = last ? mix : 1.0f;
        const GraphValueId* stage_mask = last && needs_mix ? mask_id : nullptr;
        const auto tone =
            Ops::ExecuteLocalTone(device, src, dest_binding, scene, grade->Id(),
                                  schedule.shadows_slider, schedule.highlights_slider,
                                  plan.geometry, stage_mix, stage_mask);
        result.local_tone_reference_resource_id       = tone.reference_resource_id;
        result.local_tone_rebuilt_reference           = tone.rebuilt_reference;
        result.local_tone_sampled_canonical_reference = tone.sampled_canonical_reference;
        result.local_tone_transient_bytes             = tone.transient_bytes;
        ++result.local_tone_pass_count;
        wrote_dest = true;
      } else {
        diag::PreviewSubStageInterval pointwise(diag::PreviewSubStageKind::Pointwise);
        GpuWorkSample<Device> gpu(device);
        if (last && needs_mix) {
          Ops::DispatchPointwiseWithMix(device, src, dest_binding, scene, lut, grade->Id(),
                                        fused_starts[index],
                                        static_cast<std::uint32_t>(op.fused_offsets.size()), mix,
                                        mask_id, width, height);
        } else {
          Ops::DispatchPointwise(device, src, dest_binding, lut, grade->Id(), fused_starts[index],
                                 static_cast<std::uint32_t>(op.fused_offsets.size()), width,
                                 height);
        }
        ++result.pointwise_dispatch_count;
        wrote_dest = true;
      }
    }

    Ops::CheckAfterEncode(device);
    result.output_binding = dest_binding;
    return result;
  }
};

}  // namespace alcedo
