//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_neighbor_dispatch.hpp"

#include <array>
#include <span>
#include <stdexcept>
#include <string>

#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "edit/runtime/vulkan/vulkan_scene_view.hpp"
#include "vulkan/vulkan_primary_grade_neighbor_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels                  = vulkan::primary_grade_neighbor_kernels;

constexpr const char* kErrorPrefix = "Vulkan neighborhood";

[[noreturn]] void     Fail(const std::string& message) {
  throw std::runtime_error(std::string(kErrorPrefix) + ": " + message);
}

/// The checks of @p params that come before a command: the behavior, the vertical radius that the
/// workgroup tile holds, and the Gaussian taps inside that radius.
void RequireKernelParams(const GradeNeighborParams& params, const char* caller) {
  RequireVulkanNeighborhoodKernelBehavior(params, caller);
  const auto radius = NeighborhoodVerticalRadius(params);
  if (radius > kernels::kMaxRadius) {
    throw std::invalid_argument(std::string(caller) + ": the vertical radius " +
                                std::to_string(radius) + " is above " +
                                std::to_string(kernels::kMaxRadius));
  }
  const bool halation =
      static_cast<AdjustmentBehavior>(params.behavior) == AdjustmentBehavior::Halation;
  if (!halation &&
      (params.tap_count > kGradeNeighborMaxTapCount || params.tap_count > params.radius + 1U)) {
    throw std::invalid_argument(std::string(caller) + ": " + std::to_string(params.tap_count) +
                                " Gaussian taps for the radius " + std::to_string(params.radius));
  }
}

/// The scratch image of the horizontal pass, after a check of its format and size.
auto RequireScratch(const VulkanNeighborOps::HorizontalScratch& scratch, std::uint32_t width,
                    std::uint32_t height) -> const VulkanBackend::Texture2D& {
  if (scratch.Empty()) {
    Fail("the horizontal scratch image is empty");
  }
  const auto& texture = scratch.Texture();
  if (texture.Format() != TextureFormat::Rgba32f || texture.Width() < width ||
      texture.Height() < height) {
    Fail("the horizontal scratch image is not an RGBA32F image of the size");
  }
  return texture;
}

void RequireSize(const VulkanSceneView& view, std::uint32_t width, std::uint32_t height,
                 const char* name) {
  if (view.width != width || view.height != height) {
    Fail(std::string("the ") + name + " differs in size from the neighborhood");
  }
}

}  // namespace

void RequireVulkanNeighborhoodKernelBehavior(const GradeNeighborParams& params,
                                             const char*                caller) {
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  if (behavior == AdjustmentBehavior::FilmGrain) {
    throw std::invalid_argument(std::string(caller) +
                                ": Film Grain has its own pass, not the neighborhood kernels");
  }
  if (behavior != AdjustmentBehavior::Clarity && behavior != AdjustmentBehavior::Sharpen &&
      behavior != AdjustmentBehavior::Halation) {
    throw std::invalid_argument(std::string(caller) + ": behavior " +
                                std::to_string(params.behavior) +
                                " is not Clarity, Sharpen, or Halation");
  }
}

auto VulkanNeighborParamsValueId(const NeighborWork& work) -> GraphValueId {
  return GraphValueId{work.owner, PortId{"runtime.neighbor." + std::to_string(work.command_index)}};
}

auto VulkanNeighborOps::AcquireHorizontalScratch(VulkanRenderDevice& device, std::uint32_t width,
                                                 std::uint32_t height) -> HorizontalScratch {
  return device.Workspace().Textures().Acquire({width, height, TextureFormat::Rgba32f});
}

void VulkanNeighborOps::DispatchHorizontal(VulkanRenderDevice& device, const FrameSceneBinding& src,
                                           HorizontalScratch& scratch, const NeighborWork& work,
                                           std::uint32_t width, std::uint32_t height) {
  RequireKernelParams(work.params, "VulkanNeighborOps::DispatchHorizontal");
  auto&       workspace = device.Workspace();
  auto&       backend   = workspace.Device();
  const auto& blur      = RequireScratch(scratch, width, height);
  const auto  source    = VulkanBindScene(device, src);
  RequireSize(source, width, height, "source");

  // The parameters do not fit in the push constants (300 bytes); the vertical pass reads the same
  // buffer.
  const auto id     = VulkanNeighborParamsValueId(work);
  auto*      params = workspace.Values().Find(id);
  if (params == nullptr || params->Bytes() < sizeof(GradeNeighborParams)) {
    workspace.Values().Store(id, backend.CreateBuffer(sizeof(GradeNeighborParams)));
    params = workspace.Values().Find(id);
    if (params == nullptr) {
      Fail("the parameter buffer store failed");
    }
  }
  backend.UploadBufferRange(*params, 0,
                            std::as_bytes(std::span<const GradeNeighborParams, 1>(&work.params, 1)),
                            device.CommandContext());

  kernels::BlurPushConstants push;
  push.params                                    = params->DeviceAddress();
  push.source                                    = source.buffer;
  push.width                                     = width;
  push.height                                    = height;
  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{kernels::kSourceBinding, VulkanSceneImageBinding(backend, source)},
      VulkanImageBinding{kernels::kBlurBinding, &blur}};
  RecordKernelDispatch(backend, device.CommandContext(), kernels::BlurDescriptor(), images,
                       PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

void VulkanNeighborOps::DispatchVerticalApply(
    VulkanRenderDevice& device, const FrameSceneBinding& src, HorizontalScratch& scratch,
    const FrameSceneBinding& dst, const FrameSceneBinding& original, const LutBinding&,
    const NeighborWork& work, float mix, const GraphValueId* mask_id, std::uint32_t width,
    std::uint32_t height) {
  RequireKernelParams(work.params, "VulkanNeighborOps::DispatchVerticalApply");
  auto&       workspace   = device.Workspace();
  auto&       backend     = workspace.Device();
  const auto& blur        = RequireScratch(scratch, width, height);
  const auto  source      = VulkanBindScene(device, src);
  const auto  destination = VulkanBindScene(device, dst);
  RequireSize(source, width, height, "source");
  RequireSize(destination, width, height, "destination");
  const auto* params = workspace.Values().Find(VulkanNeighborParamsValueId(work));
  if (params == nullptr) {
    Fail("the vertical pass runs without the horizontal pass of its work");
  }

  kernels::ApplyPushConstants push;
  push.params                                = params->DeviceAddress();
  push.source                                = source.buffer;
  push.destination                           = destination.buffer;
  push.width                                 = width;
  push.height                                = height;
  push.grade_mix                             = mix;

  const VulkanBackend::Texture2D* mix_image  = &backend.DummyStorageImage(TextureFormat::Rgba32f);
  const VulkanBackend::Texture2D* mask_image = &backend.DummyStorageImage(TextureFormat::R8);
  // The OpenCL rule (EnqueueOpenClNeighborVerticalScene): a full mix without a mask is no mix.
  if (mix != 1.0F || mask_id != nullptr) {
    const auto mix_source = VulkanBindScene(device, original);
    RequireSize(mix_source, width, height, "mix source");
    push.apply_mix  = 1;
    push.mix_source = mix_source.buffer;
    mix_image       = VulkanSceneImageBinding(backend, mix_source);
    if (mask_id != nullptr) {
      mask_image    = &VulkanRequireSceneMask(device, *mask_id, width, height, kErrorPrefix);
      push.has_mask = 1;
    }
  }

  const std::array<VulkanImageBinding, 5> images = {
      VulkanImageBinding{kernels::kSourceBinding, VulkanSceneImageBinding(backend, source)},
      VulkanImageBinding{kernels::kBlurBinding, &blur},
      VulkanImageBinding{kernels::kDestinationBinding,
                         VulkanSceneImageBinding(backend, destination)},
      VulkanImageBinding{kernels::kMixSourceBinding, mix_image},
      VulkanImageBinding{kernels::kMaskBinding, mask_image}};
  RecordKernelDispatch(backend, device.CommandContext(), kernels::ApplyDescriptor(), images,
                       PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
