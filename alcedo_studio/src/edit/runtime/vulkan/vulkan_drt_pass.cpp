//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_drt_pass.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

#include "edit/graph/drt_node_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/runtime/adjustment_runtime.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/drt_post_executor.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/film_grain_plan.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_drt_params.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "edit/runtime/vulkan/vulkan_neighbor_dispatch.hpp"
#include "edit/runtime/vulkan/vulkan_parameter_address.hpp"
#include "edit/runtime/vulkan/vulkan_scene_view.hpp"
#include "vulkan/vulkan_drt_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels                     = vulkan::drt_kernels;

constexpr const char*   kErrorPrefix  = "ExecuteVulkanDrt";
constexpr std::uint32_t kDrtDirtyBits = static_cast<std::uint32_t>(DrtDirty::All);

[[noreturn]] void       Fail(const std::string& message) {
  throw std::runtime_error(std::string(kErrorPrefix) + ": " + message);
}

auto DisplayParamsKey(const NodeId& drt_id) -> ParameterSlotKey {
  return ParameterSlotKey{drt_id, AdjustmentInstanceId{"drt.output"}};
}

/**
 * The display transform of @p scene into @p display: `drt_display_scene_rgba32f` with the
 * parameter slot of the DRT. Each side is an RGBA32F image or a scene-work buffer.
 */
void DispatchDisplay(VulkanRenderDevice& device, const FrameSceneBinding& scene,
                     const FrameSceneBinding& display, const NodeId& drt_id, std::uint32_t width,
                     std::uint32_t height) {
  static const auto kKernel     = kernels::DisplaySceneDescriptor();
  auto&             backend     = device.Workspace().Device();
  const auto        source      = VulkanBindScene(device, scene);
  const auto        destination = VulkanBindScene(device, display);
  if (source.width != width || source.height != height || destination.width != width ||
      destination.height != height) {
    Fail("the display transform images differ in size");
  }
  kernels::DisplayPushConstants push;
  push.source      = source.buffer;
  push.destination = destination.buffer;
  push.parameters  = ParameterSlotAddress(device.Workspace().Parameters(), DisplayParamsKey(drt_id),
                                          sizeof(VulkanDrtParamBlock), kErrorPrefix);
  push.width       = width;
  push.height      = height;
  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{kernels::kSourceBinding, VulkanSceneImageBinding(backend, source)},
      VulkanImageBinding{kernels::kDestinationBinding,
                         VulkanSceneImageBinding(backend, destination)}};
  RecordKernelDispatch(backend, device.CommandContext(), kKernel, images, PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

/// The Vulkan operations of `DrtPostExecutor`: the form of `OpenClDrtOps`. The neighborhood
/// operations (Clarity, Sharpen, Halation) come from `VulkanNeighborOps` (phase VK9).
struct VulkanDrtOps : VulkanNeighborOps {
  using Texture                                     = VulkanBackend::Texture2D;

  static constexpr const char* kErrorPrefix         = "ExecuteVulkanDrt";
  /// The film print kernels are phase VK13; `ExecuteVulkanDrt` names the phase before the shared
  /// executor runs.
  static constexpr bool        kImplementsFilmPrint = false;

  /// The Film Grain of the finish is phase VK14; `ExecuteVulkanDrt` refuses it before any command.
  [[noreturn]] static void     DispatchFilmGrain(VulkanRenderDevice&, const FrameSceneBinding&,
                                                 const FrameSceneBinding&, const FilmGrainPlan&,
                                                 FilmGrainDisplayEncoding,
                                                 const ResolvedRenderGeometry&, std::uint32_t,
                                                 std::uint32_t) {
    Fail("the Film Grain of the DRT finish is phase VK14 of the Vulkan backend");
  }

  /// The neighborhood parameters travel in the node value buffer of each command
  /// (`VulkanNeighborOps::DispatchHorizontal`), not in an arena slot.
  static void RefreshNeighborhoodAdjustment(VulkanRenderDevice&, const IOperatorModel&,
                                            const ParameterSlotKey&, AdjustmentBehavior) {}

  static void PrepareNeighborCommands(VulkanRenderDevice&, const NodeId&,
                                      std::span<const std::uint32_t>) {}

  static auto NeighborLut(VulkanRenderDevice& device) -> LutBinding {
    return device.Workspace().Device().DummyLut();
  }

  static auto BindingWidth(VulkanRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return VulkanBindScene(device, binding).width;
  }

  static auto BindingHeight(VulkanRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return VulkanBindScene(device, binding).height;
  }

  static void AcquireDisplayOutput(VulkanRenderDevice& device, const GraphValueId& id,
                                   std::uint32_t width, std::uint32_t height) {
    (void)device.Workspace().AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
  }

  /**
   * Pack the display transform when the slot is missing, the DRT revision changed, or this frame
   * overrides the output color. An override is recorded without a revision, so the next plain
   * frame packs again.
   */
  static void BindDisplayParams(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                const DrtNodeModel& drt) {
    auto&      arena      = device.Workspace().Parameters();
    const auto key        = DisplayParamsKey(drt.Id());
    const bool overridden = plan.output_color_override.has_value();
    const auto revision   = drt.Params().Revision();
    if (!overridden && arena.Contains(key) && arena.AppliedRevision(key) == revision) {
      return;
    }
    const auto block = PackVulkanDrtParams(
        DrtOutputResolver::ResolveNode(drt, plan.output_color_override, kErrorPrefix));
    arena.BindOrWritePackedSlot(key, DirtyFieldMask{kDrtDirtyBits}, block,
                                overridden ? kNoParameterRevision : revision);
  }

  static void DispatchDisplayTransform(VulkanRenderDevice& device, const FrameSceneBinding& scene,
                                       const FrameSceneBinding& display, const NodeId& drt_id,
                                       std::uint32_t width, std::uint32_t height) {
    DispatchDisplay(device, scene, display, drt_id, width, height);
  }

  static void CheckAfterEncode(VulkanRenderDevice&) {}
};

/// The film print (phase VK13) and an enabled Film Grain of the finish (phase VK14) fail here,
/// before the shared executor records a command. An output encoding that does not resolve keeps
/// its own error first (the order of `DrtPostExecutor`).
void RequireImplementedDrt(const ExecutionPlan& plan, const DrtNodeModel& drt) {
  const auto method = drt.Params().Method();
  if (method == DrtMethod::FilmPrint) {
    (void)DrtOutputResolver::ResolveNode(drt, plan.output_color_override, kErrorPrefix);
    Fail("the film print is phase VK13 of the Vulkan backend");
  }
  for (const auto& compiled : plan.drt.post_adjustments) {
    const auto* model    = drt.FindAdjustment(compiled.instance_id);
    const auto  behavior = TryResolveAdjustmentBehavior(compiled.type);
    if (model == nullptr || !behavior.has_value() || *behavior != AdjustmentBehavior::FilmGrain) {
      continue;  // the shared executor reports a mismatch
    }
    const auto neighbor = MakeGradeNeighborParams(*model, *behavior, plan.geometry);
    if (DrtPostNeighborhoodRuns(method, neighbor)) {
      Fail("the Film Grain of the DRT finish is phase VK14 of the Vulkan backend");
    }
  }
}

}  // namespace

auto ExecuteVulkanDrt(VulkanRenderDevice& device, const ExecutionPlan& plan,
                      const PipelineDocument& document, const FrameSceneBinding& scene)
    -> VulkanDrtResult {
  if (!device.Workspace().IsRendering()) {
    Fail("BeginRender has not been called");
  }
  const auto* drt = document.Drt();
  if (drt == nullptr) {
    Fail("missing DRT endpoint");
  }
  RequireImplementedDrt(plan, *drt);
  const auto executed = DrtPostExecutor<VulkanDrtOps>::Execute(device, plan, document, scene);
  return {executed.output, executed.display_post, executed.post_neighborhood_count, executed.trace};
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
