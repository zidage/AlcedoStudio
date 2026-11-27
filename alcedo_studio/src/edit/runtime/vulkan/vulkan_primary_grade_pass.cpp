//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_primary_grade_pass.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/grade_executor.hpp"
#include "edit/runtime/grade_lut.hpp"
#include "edit/runtime/neighbor_executor.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "edit/runtime/vulkan/vulkan_scene_view.hpp"
#include "vulkan/vulkan_primary_grade_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels                  = vulkan::primary_grade_kernels;

constexpr const char* kErrorPrefix = "ExecuteVulkanPrimaryGrade";

/// The result of the Local Laplacian stage that `GradeExecutor` reads (phase VK10 fills it).
struct VulkanLocalToneResult {
  std::uint64_t reference_resource_id       = 0;
  bool          rebuilt_reference           = false;
  bool          sampled_canonical_reference = false;
  std::uint32_t transient_bytes             = 0;
};

auto EnsureBuffer(VulkanRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> VulkanBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) {
    return *existing;
  }
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  auto* stored = workspace.Values().Find(id);
  if (stored == nullptr) {
    throw std::runtime_error("ExecuteVulkanPrimaryGrade: command buffer store failed");
  }
  return *stored;
}

/// The image of a scene view for its binding: the cached image, or the dummy RGBA32F image when
/// the kernel reads the buffer.
auto SceneImage(VulkanBackend& backend, const VulkanSceneView& view)
    -> const VulkanBackend::Texture2D* {
  return view.image != nullptr ? view.image : &backend.DummyStorageImage(TextureFormat::Rgba32f);
}

/// The R8 mask of the Grade, after a check of its format and size.
auto RequireMask(VulkanRenderDevice& device, const GraphValueId& mask_id, std::uint32_t width,
                 std::uint32_t height) -> const VulkanBackend::Texture2D& {
  auto* mask = device.Workspace().Images().Find(mask_id);
  if (mask == nullptr || mask->Empty()) {
    throw std::runtime_error("ExecuteVulkanPrimaryGrade: compiled mask output is missing");
  }
  const auto& texture = mask->Texture();
  if (texture.Format() != TextureFormat::R8 || texture.Width() < width ||
      texture.Height() < height) {
    throw std::runtime_error(
        "ExecuteVulkanPrimaryGrade: compiled mask output is not an R8 image "
        "of the Grade size");
  }
  return texture;
}

/**
 * The point kernel: @p command_count records from @p command_offset of @p commands on @p src
 * into the scene-work member @p dst, then, with @p mix_source, the mix with it (times the mask of
 * @p mask_id when that is not null).
 */
void EncodePointwise(VulkanRenderDevice& device, const FrameSceneBinding& src,
                     const FrameSceneBinding& dst, const FrameSceneBinding* mix_source,
                     VkDeviceAddress commands, std::uint32_t command_offset,
                     std::uint32_t command_count, const VulkanLutBinding& lut, float mix,
                     const GraphValueId* mask_id, std::uint32_t width, std::uint32_t height) {
  auto&      backend     = device.Workspace().Device();
  const auto source      = VulkanBindScene(device, src);
  const auto destination = VulkanBindScene(device, dst);
  if (!destination.IsBuffer()) {
    throw std::runtime_error(
        "ExecuteVulkanPrimaryGrade: the destination is not a scene-work "
        "member");
  }
  if (source.width != width || source.height != height || destination.width != width ||
      destination.height != height) {
    throw std::runtime_error("ExecuteVulkanPrimaryGrade: the scene images differ in size");
  }
  if (lut.address == 0) {
    throw std::runtime_error("ExecuteVulkanPrimaryGrade: LUT resource is missing");
  }

  kernels::PointwisePushConstants push;
  push.parameter_base = device.Workspace().Parameters().DeviceBuffer().DeviceAddress();
  push.commands       = commands;
  push.lut            = lut.address;
  push.source         = source.buffer;
  push.destination    = destination.buffer;
  push.command_offset = command_offset;
  push.command_count  = command_count;
  push.lut_edge       = lut.edge_size;
  push.width          = width;
  push.height         = height;
  push.grade_mix      = mix;

  const VulkanBackend::Texture2D* mix_image  = &backend.DummyStorageImage(TextureFormat::Rgba32f);
  const VulkanBackend::Texture2D* mask_image = &backend.DummyStorageImage(TextureFormat::R8);
  if (mix_source != nullptr) {
    const auto original = VulkanBindScene(device, *mix_source);
    if (original.width != width || original.height != height) {
      throw std::runtime_error("ExecuteVulkanPrimaryGrade: the mix source differs in size");
    }
    push.apply_mix  = 1;
    push.mix_source = original.buffer;
    mix_image       = SceneImage(backend, original);
    if (mask_id != nullptr) {
      mask_image    = &RequireMask(device, *mask_id, width, height);
      push.has_mask = 1;
    }
  }

  const std::array<VulkanImageBinding, 3> images = {
      VulkanImageBinding{kernels::kSourceBinding, SceneImage(backend, source)},
      VulkanImageBinding{kernels::kMixSourceBinding, mix_image},
      VulkanImageBinding{kernels::kMaskBinding, mask_image}};
  RecordKernelDispatch(backend, device.CommandContext(), kernels::PointwiseSceneDescriptor(),
                       images, PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

[[noreturn]] void ThrowStageOfALaterPhase(const char* stage, const char* phase) {
  throw std::runtime_error(std::string(kErrorPrefix) + ": the Vulkan kernels of the " + stage +
                           " are not in this build (plan phase " + phase + ", #1036)");
}

/// The Vulkan operations of `GradeExecutor` and `NeighborExecutor`.
struct VulkanGradeOps {
  using Device                              = VulkanRenderDevice;
  using Backend                             = VulkanBackend;
  using Texture                             = VulkanBackend::Texture2D;
  using HorizontalScratch                   = ResourceLease<VulkanBackend>;
  using LutBinding                          = VulkanLutBinding;

  static constexpr const char* kErrorPrefix = "ExecuteVulkanPrimaryGrade";

  static auto BindingWidth(VulkanRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return VulkanBindScene(device, binding).width;
  }

  static auto BindingHeight(VulkanRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return VulkanBindScene(device, binding).height;
  }

  /// Uploads @p fused_offsets into the node value `runtime.order` of the Grade when their topology
  /// differs from the last upload (the OpenCL rule). Returns the uploaded bytes.
  static auto UploadFusedCommands(VulkanRenderDevice& device, const NodeId& grade_id,
                                  const std::vector<std::uint32_t>& fused_offsets)
      -> std::uint32_t {
    if (fused_offsets.empty()) {
      return 0;
    }
    auto&              workspace = device.Workspace();
    const GraphValueId command_id{grade_id, PortId{"runtime.order"}};
    const auto         bytes = fused_offsets.size() * sizeof(fused_offsets[0]);
    ContentHash        topology;
    topology.MixText(grade_id.Value());
    topology.MixU32(static_cast<std::uint32_t>(fused_offsets.size()));
    for (const auto offset : fused_offsets) {
      topology.MixU32(offset);
    }
    const auto  topology_hash = topology.Key().hash;
    const auto* existing      = workspace.Values().Find(command_id);
    const bool  needs_upload  = existing == nullptr || existing->Bytes() < bytes ||
                              workspace.Device().GradeCommandTopologyHash() != topology_hash;
    auto& buffer =
        EnsureBuffer(workspace, command_id, std::max<std::size_t>(bytes, sizeof(std::uint32_t)));
    if (!needs_upload) {
      return 0;
    }
    workspace.Device().UploadBufferRange(buffer, 0, std::as_bytes(std::span(fused_offsets)),
                                         device.CommandContext());
    workspace.Device().SetGradeCommandTopologyHash(topology_hash);
    return static_cast<std::uint32_t>(bytes);
  }

  static auto LoadLut(VulkanRenderDevice& device, const ColorGradeNodeModel& grade)
      -> VulkanLutBinding {
    auto&      backend = device.Workspace().Device();
    const auto packed  = TryPackGradeLut(grade, device.Workspace().LutResources());
    if (packed == nullptr) {
      return backend.DummyLut();
    }
    return backend.AcquireLut(packed->key, packed->rgba, packed->edge, device.CommandContext());
  }

  static auto LutResourceId(const VulkanLutBinding& lut) -> std::uint64_t {
    return lut.resource_id;
  }

  static auto CommandAddress(VulkanRenderDevice& device, const NodeId& grade_id,
                             std::uint32_t command_count) -> VkDeviceAddress {
    auto* commands =
        device.Workspace().Values().Find(GraphValueId{grade_id, PortId{"runtime.order"}});
    if (commands != nullptr) {
      return commands->DeviceAddress();
    }
    if (command_count != 0U) {
      throw std::runtime_error("ExecuteVulkanPrimaryGrade: missing fused command buffer");
    }
    // A mix without commands (GradeExecutor, issue #900) reads no command; the parameter buffer
    // stands in for the command address, as in OpenCL.
    return device.Workspace().Parameters().DeviceBuffer().DeviceAddress();
  }

  static void DispatchPointwise(VulkanRenderDevice& device, const FrameSceneBinding& src,
                                const FrameSceneBinding& dst, const VulkanLutBinding& lut,
                                const NodeId& grade_id, std::uint32_t command_start,
                                std::uint32_t command_count, std::uint32_t width,
                                std::uint32_t height) {
    const auto commands =
        device.Workspace().Values().Find(GraphValueId{grade_id, PortId{"runtime.order"}});
    if (commands == nullptr) {
      throw std::runtime_error("ExecuteVulkanPrimaryGrade: missing fused command buffer");
    }
    EncodePointwise(device, src, dst, nullptr, commands->DeviceAddress(), command_start,
                    command_count, lut, 1.0F, nullptr, width, height);
  }

  static void DispatchPointwiseWithMix(VulkanRenderDevice& device, const FrameSceneBinding& src,
                                       const FrameSceneBinding& dst,
                                       const FrameSceneBinding& original,
                                       const VulkanLutBinding& lut, const NodeId& grade_id,
                                       std::uint32_t command_start, std::uint32_t command_count,
                                       float mix, const GraphValueId* mask_id, std::uint32_t width,
                                       std::uint32_t height) {
    EncodePointwise(device, src, dst, &original, CommandAddress(device, grade_id, command_count),
                    command_start, command_count, lut, mix, mask_id, width, height);
  }

  static void DispatchCurveInputHistogram(VulkanRenderDevice& device, const FrameSceneBinding& src,
                                          const VulkanBackend::Buffer& commands,
                                          std::uint32_t                command_count,
                                          VulkanBackend::Buffer& counts, std::uint32_t width,
                                          std::uint32_t height, std::uint32_t step) {
    const auto sampled_width = CurveInputHistogramSampledLength(width, step);
    const auto sample_count  = static_cast<std::uint32_t>(
        static_cast<std::uint64_t>(sampled_width) * CurveInputHistogramSampledLength(height, step));
    if (sample_count == 0U) {
      return;
    }
    auto&      backend = device.Workspace().Device();
    const auto source  = VulkanBindScene(device, src);
    const auto groups  = std::min<std::uint32_t>(
        (sample_count + kernels::kHistogramGroupSize - 1) / kernels::kHistogramGroupSize,
        kernels::kHistogramMaxGroups);

    kernels::CurveInputHistogramPushConstants push;
    push.parameter_base   = device.Workspace().Parameters().DeviceBuffer().DeviceAddress();
    push.commands         = commands.DeviceAddress();
    push.source           = source.buffer;
    push.counts           = counts.DeviceAddress();
    push.width            = width;
    push.step             = step;
    push.sampled_width    = sampled_width;
    push.sample_count     = sample_count;
    push.command_count    = command_count;
    push.invocation_count = groups * kernels::kHistogramGroupSize;

    const std::array<VulkanImageBinding, 1> images = {
        VulkanImageBinding{kernels::kSourceBinding, SceneImage(backend, source)}};
    RecordKernelDispatch(backend, device.CommandContext(), kernels::CurveInputHistogramDescriptor(),
                         images, PushConstantBytes(push), VulkanWorkgroupCount{groups, 1, 1});
  }

  static auto AcquireHorizontalScratch(VulkanRenderDevice&, std::uint32_t, std::uint32_t)
      -> HorizontalScratch {
    ThrowStageOfALaterPhase("Grade neighborhood stage", "VK9");
  }

  static void DispatchHorizontal(VulkanRenderDevice&, const FrameSceneBinding&, HorizontalScratch&,
                                 const NeighborWork&, std::uint32_t, std::uint32_t) {
    ThrowStageOfALaterPhase("Grade neighborhood stage", "VK9");
  }

  static void DispatchVerticalApply(VulkanRenderDevice&, const FrameSceneBinding&,
                                    HorizontalScratch&, const FrameSceneBinding&,
                                    const FrameSceneBinding&, const LutBinding&,
                                    const NeighborWork&, float, const GraphValueId*, std::uint32_t,
                                    std::uint32_t) {
    ThrowStageOfALaterPhase("Grade neighborhood stage", "VK9");
  }

  static auto ExecuteLocalTone(VulkanRenderDevice&, const FrameSceneBinding&,
                               const FrameSceneBinding&, const FrameSceneBinding&, const NodeId&,
                               float, float, const ResolvedRenderGeometry&, float,
                               const GraphValueId*) -> VulkanLocalToneResult {
    ThrowStageOfALaterPhase("Local Laplacian stage (Shadows, Highlights)", "VK10");
  }

  static void CheckAfterEncode(VulkanRenderDevice&) {}
};

}  // namespace

auto ExecuteVulkanPrimaryGrade(VulkanRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& prepared, const PipelineDocument& document,
                               const CompiledGradeNode& compiled_grade,
                               const FrameSceneBinding& scene, CurveInputHistogramRecord* histogram)
    -> VulkanPrimaryGradeResult {
  const auto executed = GradeExecutor<VulkanGradeOps>::Execute(device, plan, prepared, document,
                                                               compiled_grade, scene, histogram);
  VulkanPrimaryGradeResult result;
  result.output                                 = executed.output;
  result.output_binding                         = executed.output_binding;
  result.pointwise_dispatch_count               = executed.pointwise_dispatch_count;
  result.detail_pass_count                      = executed.detail_pass_count;
  result.local_tone_pass_count                  = executed.local_tone_pass_count;
  result.local_tone_transient_bytes             = executed.local_tone_transient_bytes;
  result.command_upload_bytes                   = executed.command_upload_bytes;
  result.lut_resource_id                        = executed.lut_resource_id;
  result.local_tone_reference_resource_id       = executed.local_tone_reference_resource_id;
  result.local_tone_rebuilt_reference           = executed.local_tone_rebuilt_reference;
  result.local_tone_sampled_canonical_reference = executed.local_tone_sampled_canonical_reference;
  return result;
}

auto ExecuteVulkanPrimaryGrade(VulkanRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& prepared, const PipelineDocument& document,
                               CurveInputHistogramRecord* histogram) -> VulkanPrimaryGradeResult {
  if (plan.grade_nodes.empty()) {
    throw std::runtime_error("ExecuteVulkanPrimaryGrade: plan has no Color Grade");
  }
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteVulkanPrimaryGrade: BeginRender has not been called");
  }
  device.Workspace().PrepareResultValidity(plan, document, prepared);
  const ImageExtent extent{plan.geometry.render_extent.width, plan.geometry.render_extent.height};
  device.Workspace().EnsureSceneWorkImages(extent);
  FrameSceneBinding        scene = FrameSceneBinding::CachedImage(plan.develop_output);
  VulkanPrimaryGradeResult last{};
  for (const auto& compiled_grade : plan.grade_nodes) {
    auto* grade_histogram =
        histogram != nullptr && histogram->grade_id == compiled_grade.node_id ? histogram : nullptr;
    last  = ExecuteVulkanPrimaryGrade(device, plan, prepared, document, compiled_grade, scene,
                                      grade_histogram);
    scene = last.output_binding;
  }
  return last;
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
