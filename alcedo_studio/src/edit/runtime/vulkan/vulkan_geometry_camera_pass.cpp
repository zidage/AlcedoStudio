//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_geometry_camera_pass.hpp"

#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/runtime/camera_color_gpu_params.hpp"
#include "edit/runtime/dng_profile_gpu_data.hpp"
#include "edit/runtime/parameter_arena.hpp"
#include "edit/runtime/parameter_binding.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_buffer_view.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "edit/runtime/vulkan/vulkan_parameter_address.hpp"
#include "vulkan/vulkan_geometry_camera_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels = vulkan::geometry_camera_kernels;

auto AcquireRgba(VulkanRenderWorkspace& workspace, const GraphValueId& id, std::uint32_t width,
                 std::uint32_t height) -> ResourceLease<VulkanBackend>& {
  return workspace.AcquireImageForWrite(id, {width, height, TextureFormat::Rgba32f});
}

auto ImageBindings(const VulkanBackend::Texture2D& source,
                   const VulkanBackend::Texture2D& destination)
    -> std::array<VulkanImageBinding, 2> {
  return {VulkanImageBinding{kernels::kSourceBinding, &source},
          VulkanImageBinding{kernels::kDestinationBinding, &destination}};
}

}  // namespace

void ExecuteVulkanGeometryResample(VulkanRenderDevice& device, const ExecutionPlan& plan) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteVulkanGeometryResample: BeginRender has not been called");
  }
  auto* sensor = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr || sensor->Empty()) {
    throw std::runtime_error("ExecuteVulkanGeometryResample: missing develop.sensor_linear");
  }
  if (!plan.encode_geometry_resample) {
    workspace.AliasImageFrom(plan.geometry_output, plan.sensor_linear_output);
    return;
  }
  auto& destination =
      AcquireRgba(workspace, plan.geometry_output, plan.geometry.render_extent.width,
                  plan.geometry.render_extent.height);
  // The acquire can move the leases of the cache: find the source again.
  sensor = workspace.Images().Find(plan.sensor_linear_output);
  if (sensor == nullptr) {
    throw std::runtime_error("ExecuteVulkanGeometryResample: sensor texture lost during acquire");
  }

  const auto&                            gpu = plan.geometry.gpu_data;
  kernels::GeometryResamplePushConstants push;
  push.m00            = gpu.render_to_decoded[0];
  push.m01            = gpu.render_to_decoded[1];
  push.m02            = gpu.render_to_decoded[2];
  push.m10            = gpu.render_to_decoded[3];
  push.m11            = gpu.render_to_decoded[4];
  push.m12            = gpu.render_to_decoded[5];
  push.decoded_width  = gpu.decoded_width;
  push.decoded_height = gpu.decoded_height;
  push.render_width   = gpu.render_width;
  push.render_height  = gpu.render_height;
  for (std::size_t i = 0; i < 4; ++i) {
    push.border[i] = gpu.border_rgba[i];
  }
  push.use_bicubic  = plan.geometry.filter == TextureFilter::Bicubic ? 1U : 0U;

  const auto images = ImageBindings(sensor->Texture(), destination.Texture());
  RecordKernelDispatch(workspace.Device(), device.CommandContext(),
                       kernels::GeometryResampleDescriptor(), images, PushConstantBytes(push),
                       WorkgroupsCovering(gpu.render_width, gpu.render_height, kernels::kGroupSize,
                                          kernels::kGroupSize));
}

void ExecuteVulkanCameraColor(VulkanRenderDevice& device, const ExecutionPlan& plan,
                              const PipelineDocument& document) {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteVulkanCameraColor: BeginRender has not been called");
  }
  const auto* develop = document.Develop();
  if (develop == nullptr) {
    throw std::runtime_error("ExecuteVulkanCameraColor: missing develop node");
  }
  const auto develop_params = develop->Params().Params();
  const auto resolved       = ResolveDevelopColorTransform(develop_params);
  if (!resolved.ok) {
    throw std::runtime_error(std::string("ExecuteVulkanCameraColor: ") +
                             ColorTransformFailureMessage(develop_params, resolved.error));
  }
  auto* input = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteVulkanCameraColor: missing geometry.scene_source");
  }
  const auto width  = input->Texture().Width();
  const auto height = input->Texture().Height();
  auto&      output = AcquireRgba(workspace, plan.develop_output, width, height);
  input             = workspace.Images().Find(plan.geometry_output);
  if (input == nullptr) {
    throw std::runtime_error("ExecuteVulkanCameraColor: geometry texture lost during acquire");
  }

  CameraColorGpuParams gpu_params;
  for (std::size_t i = 0; i < 9; ++i) {
    gpu_params.camera_to_ap1[i] = resolved.transform.camera_to_ap1[i];
  }
  auto&                  arena = workspace.Parameters();
  const ParameterSlotKey key{develop->Id(), kDevelopCameraColorSlot};
  arena.BindOrWritePackedSlot(key, DirtyFieldMask{DevelopDirty::WhiteBalance}, gpu_params);
  arena.UploadDirty(device.CommandContext());
  const auto table_data = PackDngProfileGpuData(develop_params, resolved.transform);
  auto&      tables =
      UploadDngProfileGpuData(workspace, develop->Id(), table_data, device.CommandContext());

  kernels::CameraColorPushConstants push;
  push.camera_to_ap1 =
      ParameterSlotAddress(arena, key, sizeof(float) * 9, "ExecuteVulkanCameraColor");
  push.dng_profile =
      RequireBufferRange(VulkanBufferView{&tables, 0}, table_data.size() * sizeof(float),
                         sizeof(float), "ExecuteVulkanCameraColor", "DNG profile tables");
  push.width        = width;
  push.height       = height;

  const auto images = ImageBindings(input->Texture(), output.Texture());
  RecordKernelDispatch(workspace.Device(), device.CommandContext(),
                       kernels::CameraColorDescriptor(), images, PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
