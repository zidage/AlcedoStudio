//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/drt_post_schedule.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {

/**
 * @brief Shared DRT/Post encode outcome used by CUDA, OpenCL, and Metal wrappers.
 */
struct DrtPostExecutionResult {
  GraphValueId         output{NodeId{"drt"}, PortId{"display"}};
  GraphValueId         display_post{NodeId{"drt"}, PortId{"display"}};
  std::uint32_t        post_neighborhood_count = 0;
  DrtPostDecisionTrace trace;
};

/**
 * @brief Common display transform followed by DRT/Post neighborhood processing.
 *
 * @tparam Ops Backend neighborhood starts, display bind/start, and native errors.
 *         Shared code owns skip, destinations, scratch lifetime, and display position.
 */
template <class Ops>
class DrtPostExecutor {
 public:
  using Device     = typename Ops::Device;
  using LutBinding = typename Ops::LutBinding;

  /**
   * @brief Apply the display transform, then enabled display-referred neighborhood writes.
   *
   * The last write is always the existing display output. Intermediate Post writes use
   * the free scene-work member. Parameter slots are uploaded before any GPU start.
   * A failed dispatch throws after Ops::CheckAfterEncode.
   */
  static auto Execute(Device& device, const ExecutionPlan& plan, PipelineDocument& document,
                      const FrameSceneBinding& scene) -> DrtPostExecutionResult {
    auto& workspace = device.Workspace();
    if (!workspace.IsRendering()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": BeginRender has not been called");
    }
    auto* drt = document.Drt();
    if (drt == nullptr) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": missing DRT endpoint");
    }
    const auto width  = Ops::BindingWidth(device, scene);
    const auto height = Ops::BindingHeight(device, scene);

    std::vector<PendingParameterPatch> pending;
    std::vector<GradeNeighborParams>   compiled_order;
    std::vector<NeighborWork>          works;
    compiled_order.reserve(plan.drt.post_adjustments.size());
    works.reserve(plan.drt.post_adjustments.size());
    std::vector<std::uint32_t> command_offsets;
    command_offsets.reserve(plan.drt.post_adjustments.size());
    for (const auto& compiled : plan.drt.post_adjustments) {
      auto* model = drt->FindAdjustment(compiled.instance_id);
      if (model == nullptr || model->Type() != compiled.type) {
        throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                                 ": compiled DRT/Post adjustment no longer matches");
      }
      const auto behavior = TryResolveAdjustmentBehavior(compiled.type);
      if (!behavior.has_value() || !IsNeighborhoodBehavior(*behavior)) {
        throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                                 ": DRT/Post adjustment is not a neighborhood operation");
      }
      const ParameterSlotKey key{drt->Id(), compiled.instance_id};
      if (auto change = Ops::RefreshNeighborhoodAdjustment(device, *model, key, *behavior)) {
        pending.push_back(std::move(*change));
      }
      auto neighbor = MakeGradeNeighborParams(*model, *behavior, plan.geometry);
      compiled_order.push_back(neighbor);
      if (neighbor.enabled == 0U) {
        continue;
      }
      NeighborWork work;
      work.params        = neighbor;
      work.owner         = drt->Id();
      work.fused_offset  = workspace.Parameters().Contains(key)
                                ? workspace.Parameters().Binding(key).offset
                                : 0;
      work.command_index = static_cast<std::uint32_t>(works.size());
      command_offsets.push_back(work.fused_offset);
      works.push_back(std::move(work));
    }

    Ops::BindDisplayParams(device, plan, *drt, pending);
    workspace.Parameters().UploadDirty(device.CommandContext());
    for (auto& patch : pending) {
      patch.Commit();
    }
    Ops::PrepareNeighborCommands(device, drt->Id(), command_offsets);

    const auto schedule = MakeDrtPostSchedule(compiled_order);
    DrtPostExecutionResult result;
    result.output                  = plan.display_output;
    result.display_post            = plan.display_output;
    result.post_neighborhood_count = static_cast<std::uint32_t>(schedule.enabled.size());
    result.trace                   = MakeDrtPostDecisionTrace(schedule);

    const auto free_member =
        scene.IsWorkImage() ? PeerOf(scene.member) : SceneWorkMember::Member0;
    const auto sequence = DrtWriteSequence(works.size());
    auto MakeDest = [&](DrtWriteTarget target) -> FrameSceneBinding {
      if (target == DrtWriteTarget::Display) {
        return FrameSceneBinding::DisplayImage(plan.display_output);
      }
      return FrameSceneBinding::WorkImage(free_member);
    };

    Ops::AcquireDisplayOutput(device, plan.display_output, width, height);
    FrameSceneBinding current = scene;
    for (std::size_t index = 0; index < sequence.size(); ++index) {
      const auto dest = MakeDest(sequence[index]);
      if (index == 0) {
        Ops::DispatchDisplayTransform(device, current, dest, drt->Id(), width, height);
      } else {
        const auto lut = Ops::NeighborLut(device);
        NeighborExecutor<Ops>::Execute(device, current, dest, current, lut, works[index - 1], 1.0f,
                                       nullptr, width, height);
      }
      current = dest;
    }
    Ops::CheckAfterEncode(device);
    return result;
  }
};

}  // namespace alcedo
