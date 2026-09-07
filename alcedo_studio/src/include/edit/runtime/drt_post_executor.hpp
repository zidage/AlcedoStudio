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
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/pending_parameter_patch.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/drt_post_schedule.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/parameter_binding.hpp"

namespace alcedo {

/**
 * @brief Shared DRT/Post encode outcome used by CUDA, OpenCL, and Metal wrappers.
 */
struct DrtPostExecutionResult {
  GraphValueId          output{NodeId{"drt"}, PortId{"display"}};
  GraphValueId          display_post{NodeId{"drt"}, PortId{"display"}};
  std::uint32_t         post_neighborhood_count = 0;
  DrtPostDecisionTrace  trace;
};

/**
 * @brief Common display transform followed by DRT/Post neighborhood processing.
 *
 * @tparam Ops Backend neighborhood starts, display bind/start, copies, and native errors.
 *         Shared code owns skip, copy, destinations, scratch lifetime, and display position.
 */
template <class Ops>
class DrtPostExecutor {
 public:
  using Device     = typename Ops::Device;
  using LutBinding = typename Ops::LutBinding;

  /**
   * @brief Apply the display transform, then enabled display-referred neighborhood writes.
   *
   * An empty enabled list copies the display base to the final output. Parameter
   * slots are uploaded before any GPU start. A failed dispatch throws after
   * Ops::CheckAfterEncode; pending Model dirty bits restore unless committed.
   */
  static auto Execute(Device& device, const ExecutionPlan& plan, PipelineDocument& document)
      -> DrtPostExecutionResult {
    auto& workspace = device.Workspace();
    if (!workspace.IsRendering()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": BeginRender has not been called");
    }
    auto* drt = document.Drt();
    if (drt == nullptr) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": missing DRT endpoint");
    }
    auto* input = workspace.Images().Find(plan.SceneInputForDrt());
    if (input == nullptr || input->Empty()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": missing DRT scene input");
    }
    const auto width  = input->Texture().Width();
    const auto height = input->Texture().Height();

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
    result.output                   = plan.display_output;
    result.display_post             = plan.display_output;
    result.post_neighborhood_count  = static_cast<std::uint32_t>(schedule.enabled.size());
    result.trace                    = MakeDrtPostDecisionTrace(schedule);

    DispatchDisplay(device, plan.SceneInputForDrt(), plan.drt.scene_output, drt->Id(), width,
                    height);
    const auto lut = Ops::NeighborLut(device);
    (void)ApplyNeighborhoods(device, schedule, works, plan.drt.scene_output, plan.display_output,
                             drt->Id(), lut, width, height);
    Ops::CheckAfterEncode(device);
    return result;
  }

  /**
   * @brief Copy or ping/pong neighborhood writes onto @p scene_output.
   *
   * @return Graph value that holds the display-referred image after neighborhood work.
   */
  static auto ApplyNeighborhoods(Device& device, const DrtPostSchedule& schedule,
                                 const std::vector<NeighborWork>& works,
                                 const GraphValueId& scene_input, const GraphValueId& scene_output,
                                 const NodeId& drt_id, const LutBinding& lut, std::uint32_t width,
                                 std::uint32_t height) -> GraphValueId {
    if (schedule.copy_scene_to_post) {
      (void)Ops::AcquireOutput(device, scene_output, width, height);
      const auto* input = device.Workspace().Images().Find(scene_input);
      if (input == nullptr) {
        throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                                 ": DRT scene input lost during scene copy");
      }
      Ops::CopyTexture(device, scene_input, scene_output);
      return scene_output;
    }
    if (works.size() != schedule.enabled.size()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                               ": neighborhood destination underflow");
    }
    const auto destinations =
        DrtNeighborhoodDestinations(drt_id, scene_output, works.size());
    GraphValueId scene_id = scene_input;
    for (std::size_t index = 0; index < works.size(); ++index) {
      const auto dest = destinations[index];
      (void)Ops::AcquireOutput(device, dest, width, height);
      NeighborExecutor<Ops>::Execute(device, scene_id, dest, lut, works[index], width, height);
      scene_id = dest;
    }
    return scene_id;
  }

  /**
   * @brief Acquire the compiled display image and start the display transform.
   *
   * Runs before neighborhood writes and converts ACEScc scene values to display-referred values.
   */
  static void DispatchDisplay(Device& device, const GraphValueId& scene_id,
                              const GraphValueId& display_output, const NodeId& drt_id,
                              std::uint32_t width, std::uint32_t height) {
    (void)Ops::AcquireOutput(device, display_output, width, height);
    auto& scene   = Ops::SceneTexture(device, scene_id);
    auto& display = Ops::SceneTexture(device, display_output);
    Ops::DispatchDisplayTransform(device, scene, display, drt_id, width, height);
  }
};

}  // namespace alcedo
