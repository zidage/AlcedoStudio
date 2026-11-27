//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_display_to_ap1_pass.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/develop_raster_input.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/raster_develop_params.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "vulkan/vulkan_display_to_ap1_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels = vulkan::display_to_ap1_kernels;

[[noreturn]] void ThrowDisplayError(const std::string& message) {
  throw std::invalid_argument("EncodeVulkanDisplayToAp1: " + message);
}

void RequireRgba32f(const VulkanBackend::Texture2D& texture, const char* role) {
  if (texture.Empty()) {
    ThrowDisplayError(std::string("empty ") + role + " texture");
  }
  if (texture.Format() != TextureFormat::Rgba32f) {
    ThrowDisplayError(std::string(role) + " texture is " + TextureFormatName(texture.Format()) +
                      "; RGBA32F is required");
  }
}

auto BytesPerPixel(HostPixelFormat format) -> std::size_t {
  switch (format) {
    case HostPixelFormat::U8Rgba:
      return 4;
    case HostPixelFormat::U16Rgba:
      return 8;
    case HostPixelFormat::F32Rgba:
      return 16;
    case HostPixelFormat::U16Cfa:
      break;
  }
  throw std::invalid_argument("EncodeVulkanLinearizeRaster: a CFA plane is not raster input");
}

}  // namespace

void EncodeVulkanLinearizeRaster(VulkanBackend& backend, VulkanCommandContext& command_context,
                                 const VulkanBufferView& source, HostPixelFormat format,
                                 std::uint32_t width, std::uint32_t height,
                                 const VulkanBufferView& parameters, std::size_t parameter_floats,
                                 const VulkanBufferView& destination) {
  constexpr const char*                 kOperation = "EncodeVulkanLinearizeRaster";
  const std::size_t                     pixels     = static_cast<std::size_t>(width) * height;
  kernels::LinearizeRasterPushConstants push;
  push.source = RequireBufferRange(source, pixels * BytesPerPixel(format), 4, kOperation, "source");
  push.parameters =
      RequireBufferRange(parameters, parameter_floats * sizeof(float), 4, kOperation, "parameters");
  push.destination =
      RequireBufferRange(destination, pixels * 4 * sizeof(float), 4, kOperation, "destination");
  push.format = static_cast<std::uint32_t>(format);
  push.width  = width;
  push.height = height;
  RecordKernelDispatch(backend, command_context, kernels::LinearizeRasterDescriptor(), {},
                       PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

void EncodeVulkanDisplayToAp1(VulkanBackend& backend, VulkanCommandContext& command_context,
                              const VulkanBackend::Texture2D& source,
                              VulkanBackend::Texture2D&       destination,
                              const VulkanBackend::Buffer&    parameters,
                              std::uint32_t parameter_offset, std::size_t parameter_floats,
                              DisplayToAp1Output output_kind) {
  RequireRgba32f(source, "source");
  RequireRgba32f(destination, "destination");
  if (source.ResourceId() == destination.ResourceId()) {
    ThrowDisplayError("the source and the destination are one texture");
  }
  if (source.Width() != destination.Width() || source.Height() != destination.Height()) {
    ThrowDisplayError("the source is " + std::to_string(source.Width()) + "x" +
                      std::to_string(source.Height()) + " and the destination is " +
                      std::to_string(destination.Width()) + "x" +
                      std::to_string(destination.Height()) + "; the sizes must be equal");
  }
  const std::size_t bytes = parameter_floats * sizeof(float);
  if (parameters.Empty() || parameter_floats == 0 || parameter_offset % sizeof(float) != 0 ||
      static_cast<std::size_t>(parameter_offset) + bytes > parameters.Bytes()) {
    ThrowDisplayError("a block of " + std::to_string(parameter_floats) + " floats at offset " +
                      std::to_string(parameter_offset) + " is outside the parameter buffer of " +
                      std::to_string(parameters.Bytes()) + " bytes");
  }

  kernels::DisplayToAp1PushConstants push;
  push.parameters                                = parameters.DeviceAddress() + parameter_offset;
  push.width                                     = destination.Width();
  push.height                                    = destination.Height();
  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{kernels::kSourceBinding, &source},
      VulkanImageBinding{kernels::kDestinationBinding, &destination},
  };
  const auto kernel = output_kind == DisplayToAp1Output::LinearAp0 ? kernels::kDisplayToAp0Linear
                                                                   : kernels::kDisplayToAp1;
  RecordKernelDispatch(
      backend, command_context, kernels::DisplayToAp1Descriptor(kernel), images,
      PushConstantBytes(push),
      WorkgroupsCovering(push.width, push.height, kernels::kGroupSize, kernels::kGroupSize));
}

void ExecuteVulkanDisplayToAp1(VulkanRenderDevice& device, const ExecutionPlan& plan,
                               const PreparedRawInput& input, const PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteVulkanDisplayToAp1: BeginRender has not been called");
  }
  const auto raster = RequireRasterInput(document, "ExecuteVulkanDisplayToAp1");
  RequireRasterPixelsMatchDescription(input, raster.source_color_, "ExecuteVulkanDisplayToAp1");
  const auto block = MakeDisplayToAp1ArenaBlock(
      ResolveDisplayToAp1Block(ResolveEffectiveRasterDescription(raster)));
  auto* source = workspace.Images().Find(plan.geometry_output);
  if (source == nullptr || source->Empty()) {
    throw std::runtime_error("ExecuteVulkanDisplayToAp1: missing geometry.scene_source");
  }
  const auto width  = source->Texture().Width();
  const auto height = source->Texture().Height();
  auto&      output =
      workspace.AcquireImageForWrite(plan.develop_output, {width, height, TextureFormat::Rgba32f});
  source = workspace.Images().Find(plan.geometry_output);
  if (source == nullptr) {
    throw std::runtime_error("ExecuteVulkanDisplayToAp1: geometry texture lost during acquire");
  }
  auto&                  arena = workspace.Parameters();
  const ParameterSlotKey key{document.Develop()->Id(), kDevelopDisplayToAp1Slot};
  arena.BindOrWritePackedSlot(key, DirtyFieldMask{DevelopDirty::Input}, block);
  arena.UploadDirty(device.CommandContext());
  EncodeVulkanDisplayToAp1(workspace.Device(), device.CommandContext(), source->Texture(),
                           output.Texture(), arena.DeviceBuffer(), arena.Binding(key).offset,
                           ALCEDO_D2A_PACKED_SIZE, DisplayToAp1Output::AcesccAp1);
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
