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
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/drt_post_schedule.hpp"
#include "edit/runtime/execution_plan.hpp"
#include "edit/runtime/film_grain_plan.hpp"
#include "edit/runtime/film_print_plan.hpp"
#include "edit/runtime/frame_scene_binding.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/profile_tone_stage.hpp"

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
   * A failed dispatch throws after Ops::CheckAfterEncode. A film print on a backend with no film
   * print kernels throws before any GPU start; an encoding that the film print does not support
   * reports its own error first. On a backend with these kernels, Ops::DispatchFilmPrint replaces
   * the display parameter bind and the display transform dispatch, and it receives the colour
   * temperature of the RAW decode of @p document for scene light As Shot. The Film Grain write
   * runs Ops::DispatchFilmGrain with the grain plan of this render; the other writes run the
   * neighborhood kernels. The film print skips the Film Grain write, and Ops::DispatchFilmPrint
   * receives the stored strength of Film Grain (0 when it is disabled) for the grain of its
   * negative (decision U4).
   *
   * With a profile tone stage (ResolveProfileToneStage), the stage buffer is uploaded before any
   * GPU start, and Ops::DispatchDisplayTransform receives it; it receives null without the stage.
   * The stage resolve throws for an unbound DCP reference before any GPU start.
   */
  static auto Execute(Device& device, const ExecutionPlan& plan, const PipelineDocument& document,
                      const FrameSceneBinding& scene) -> DrtPostExecutionResult {
    auto& workspace = device.Workspace();
    if (!workspace.IsRendering()) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": BeginRender has not been called");
    }
    const auto* drt = document.Drt();
    if (drt == nullptr) {
      throw std::runtime_error(std::string{Ops::kErrorPrefix} + ": missing DRT endpoint");
    }
    const auto method = drt->Params().Method();
    if constexpr (!Ops::kImplementsFilmPrint) {
      if (method == DrtMethod::FilmPrint) {
        (void)DrtOutputResolver::ResolveNode(*drt, plan.output_color_override, Ops::kErrorPrefix);
        throw std::runtime_error(std::string{Ops::kErrorPrefix} +
                                 ": the film print is not implemented on this backend");
      }
    }
    const auto width  = Ops::BindingWidth(device, scene);
    const auto height = Ops::BindingHeight(device, scene);

    std::vector<GradeNeighborParams>   compiled_order;
    std::vector<NeighborWork>          works;
    compiled_order.reserve(plan.drt.post_adjustments.size());
    works.reserve(plan.drt.post_adjustments.size());
    std::vector<std::uint32_t> command_offsets;
    command_offsets.reserve(plan.drt.post_adjustments.size());
    // The grain plan of the Film Grain write (Ops::DispatchFilmGrain).
    std::optional<FilmGrainPlan> film_grain;
    // The Film Grain strength of the negative of the film print; 0 with no enabled Film Grain.
    float                        film_print_grain = 0.0f;
    for (const auto& compiled : plan.drt.post_adjustments) {
      const auto* model = drt->FindAdjustment(compiled.instance_id);
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
      Ops::RefreshNeighborhoodAdjustment(device, *model, key, *behavior);
      auto neighbor = MakeGradeNeighborParams(*model, *behavior, plan.geometry);
      compiled_order.push_back(neighbor);
      if (!DrtPostNeighborhoodRuns(method, neighbor)) {
        // Decision U4: the film print reads the stored value of Film Grain for its negative.
        if (method == DrtMethod::FilmPrint && neighbor.enabled != 0U &&
            *behavior == AdjustmentBehavior::FilmGrain) {
          film_print_grain = DrtFilmGrainStrength(*model);
        }
        continue;
      }
      if (*behavior == AdjustmentBehavior::FilmGrain) {
        film_grain = MakeDrtPostFilmGrainPlan(*drt, *model, plan.geometry);
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

    const bool film_print = method == DrtMethod::FilmPrint;
    if (!film_print) {
      Ops::BindDisplayParams(device, plan, *drt);
    }
    // The profile tone stage of a DCP reference: the first operation of the display transform.
    // The film print keeps it out (ResolveProfileToneStage).
    const auto profile_tone = ResolveProfileToneStage(document);
    auto*      profile_tone_buffer =
        profile_tone.has_value()
            ? &UploadProfileToneGpuData(workspace, drt->Id(), PackProfileToneGpuData(*profile_tone),
                                        device.CommandContext())
            : nullptr;
    workspace.Parameters().UploadDirty(device.CommandContext());
    Ops::PrepareNeighborCommands(device, drt->Id(), command_offsets);

    const auto             schedule = MakeDrtPostSchedule(compiled_order, method);
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
        if constexpr (Ops::kImplementsFilmPrint) {
          if (film_print) {
            Ops::DispatchFilmPrint(device, plan, *drt, FilmSceneRawWhite(document), current, dest,
                                   width, height, film_print_grain);
          } else {
            Ops::DispatchDisplayTransform(device, current, dest, drt->Id(), profile_tone_buffer,
                                          width, height);
          }
        } else {
          Ops::DispatchDisplayTransform(device, current, dest, drt->Id(), profile_tone_buffer,
                                        width, height);
        }
      } else {
        const auto& work = works[index - 1];
        if (static_cast<AdjustmentBehavior>(work.params.behavior) ==
            AdjustmentBehavior::FilmGrain) {
          Ops::DispatchFilmGrain(device, current, dest, *film_grain,
                                 MakeDrtPostFilmGrainEncoding(*drt, plan, Ops::kErrorPrefix),
                                 plan.geometry, width, height);
        } else {
          const auto lut = Ops::NeighborLut(device);
          NeighborExecutor<Ops>::Execute(device, current, dest, current, lut, work, 1.0f, nullptr,
                                         width, height);
        }
      }
      current = dest;
    }
    Ops::CheckAfterEncode(device);
    return result;
  }
};

}  // namespace alcedo
