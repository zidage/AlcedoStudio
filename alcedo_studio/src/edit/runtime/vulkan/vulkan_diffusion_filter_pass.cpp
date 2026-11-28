//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_diffusion_filter_pass.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "edit/runtime/vulkan/vulkan_scene_view.hpp"
#include "vulkan/vulkan_diffusion_filter_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels                  = vulkan::diffusion_filter_kernels;

constexpr const char* kErrorPrefix = "ExecuteVulkanDiffusionFilter";

[[noreturn]] void     Fail(const std::string& message) {
  throw std::runtime_error(std::string(kErrorPrefix) + ": " + message);
}

using Texture = VulkanBackend::Texture2D;

auto CheckedInt(std::uint32_t value, const char* label) -> std::int32_t {
  if (value == 0 ||
      value > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
    Fail(std::string("invalid ") + label);
  }
  return static_cast<std::int32_t>(value);
}

void CopyAffine(float (&destination)[6], const Matrix3x3& matrix) {
  for (int index = 0; index < 6; ++index) {
    destination[index] = matrix.m[index];
  }
}

template <class PushConstants, std::size_t Count>
void Dispatch(VulkanRenderDevice& device, const vulkan::VulkanKernelDescriptor& kernel,
              const std::array<VulkanImageBinding, Count>& images, const PushConstants& push,
              std::uint32_t width, std::uint32_t height) {
  RecordKernelDispatch(device.Workspace().Device(), device.CommandContext(), kernel, images,
                       PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

void RequireRgba32f(const Texture& texture, ImageExtent extent, const char* label) {
  if (texture.Format() != TextureFormat::Rgba32f || texture.Width() != extent.width ||
      texture.Height() != extent.height) {
    Fail(std::string("the ") + label + " is not an RGBA32F image of its level");
  }
}

void EncodeDecode(VulkanRenderDevice& device, const VulkanSceneView& source,
                  const VulkanSceneView& output) {
  static const auto            kKernel = kernels::DecodeSceneDescriptor();
  auto&                        backend = device.Workspace().Device();
  kernels::DecodePushConstants push;
  push.source                                    = source.buffer;
  push.destination                               = output.buffer;
  push.width                                     = source.width;
  push.height                                    = source.height;
  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{kernels::kSceneBinding, VulkanSceneImageBinding(backend, source)},
      VulkanImageBinding{kernels::kOutputBinding, VulkanSceneImageBinding(backend, output)}};
  Dispatch(device, kKernel, images, push, source.width, source.height);
}

/**
 * The scatter image of @p layout from the render @p source into @p target (the base extent): the
 * form of `BuildScatter` of the OpenCL pass. With one level the reduction is the scatter image;
 * otherwise the accumulation of level 0 is. The levels are images of the texture pool.
 */
void BuildScatter(VulkanRenderDevice& device, const VulkanSceneView& source,
                  const DiffusionFilterLayout& layout, const DiffusionScatterMapping& mapping,
                  const Texture& target) {
  auto&      workspace = device.Workspace();
  auto&      backend   = workspace.Device();
  const auto count     = layout.level_count;
  if (count == 0 || count > kDiffusionMaxLevels) {
    Fail("invalid scatter level count");
  }
  RequireRgba32f(target, layout.extents[0], "scatter image");

  std::vector<ResourceLease<VulkanBackend>> leases;
  leases.reserve(2U * count);
  auto acquire = [&](ImageExtent extent) -> const Texture* {
    leases.push_back(
        workspace.Textures().Acquire({extent.width, extent.height, TextureFormat::Rgba32f}));
    return &leases.back().Texture();
  };
  std::vector<const Texture*> levels(count, nullptr);
  std::vector<const Texture*> accumulated(count, nullptr);
  for (std::uint32_t index = 0; index < count; ++index) {
    levels[index] = count == 1 ? &target : acquire(layout.extents[index]);
    if (index + 1 < count) {
      accumulated[index] = index == 0 ? &target : acquire(layout.extents[index]);
    }
  }

  const auto base = layout.extents[0];
  {
    static const auto            kKernel = kernels::ReduceBoostDescriptor();
    kernels::ReducePushConstants push;
    push.source     = source.buffer;
    push.src_width  = CheckedInt(source.width, "scene width");
    push.src_height = CheckedInt(source.height, "scene height");
    push.dst_width  = CheckedInt(base.width, "base width");
    push.dst_height = CheckedInt(base.height, "base height");
    push.samples    = CheckedInt(mapping.reduce_samples, "reduce sample count");
    push.gain       = layout.highlight_gain;
    push.low        = layout.highlight_low;
    push.high       = layout.highlight_high;
    CopyAffine(push.base_to_render, mapping.base_to_render);
    const std::array<VulkanImageBinding, 2> images = {
        VulkanImageBinding{kernels::kSceneBinding, VulkanSceneImageBinding(backend, source)},
        VulkanImageBinding{kernels::kOutputBinding, levels[0]}};
    Dispatch(device, kKernel, images, push, base.width, base.height);
  }
  for (std::uint32_t index = 1; index < count; ++index) {
    static const auto                kKernel = kernels::DownsampleDescriptor();
    const auto                       from    = layout.extents[index - 1];
    const auto                       to      = layout.extents[index];
    kernels::DownsamplePushConstants push;
    push.src_width                                 = CheckedInt(from.width, "level width");
    push.src_height                                = CheckedInt(from.height, "level height");
    push.dst_width                                 = CheckedInt(to.width, "level width");
    push.dst_height                                = CheckedInt(to.height, "level height");
    const std::array<VulkanImageBinding, 2> images = {
        VulkanImageBinding{kernels::kOutputBinding, levels[index]},
        VulkanImageBinding{kernels::kCoarseBinding, levels[index - 1]}};
    Dispatch(device, kKernel, images, push, to.width, to.height);
  }

  // Accumulate from the coarsest level. The coarsest level enters with its own weight.
  const Texture* coarse        = levels[count - 1];
  ImageExtent    coarse_extent = layout.extents[count - 1];
  float          coarse_weight = layout.weights[count - 1];
  for (std::uint32_t index = count - 1; index-- > 0;) {
    static const auto              kKernel = kernels::UpsampleAccumulateDescriptor();
    const auto                     extent  = layout.extents[index];
    kernels::UpsamplePushConstants push;
    push.coarse_width  = CheckedInt(coarse_extent.width, "coarse width");
    push.coarse_height = CheckedInt(coarse_extent.height, "coarse height");
    push.width         = CheckedInt(extent.width, "level width");
    push.height        = CheckedInt(extent.height, "level height");
    push.coarse_weight = coarse_weight;
    push.level_weight  = layout.weights[index];
    const std::array<VulkanImageBinding, 3> images = {
        VulkanImageBinding{kernels::kOutputBinding, accumulated[index]},
        VulkanImageBinding{kernels::kCoarseBinding, coarse},
        VulkanImageBinding{kernels::kLevelBinding, levels[index]}};
    Dispatch(device, kKernel, images, push, extent.width, extent.height);
    coarse        = accumulated[index];
    coarse_extent = extent;
    coarse_weight = 1.0F;
  }
}

void EncodeMix(VulkanRenderDevice& device, const VulkanSceneView& source,
               const VulkanSceneView& output, const Texture& scatter,
               const DiffusionFilterLayout& layout, const DiffusionScatterMapping& mapping) {
  static const auto         kKernel = kernels::MixSceneDescriptor();
  auto&                     backend = device.Workspace().Device();
  kernels::MixPushConstants push;
  push.source           = source.buffer;
  push.destination      = output.buffer;
  push.width            = CheckedInt(source.width, "scene width");
  push.height           = CheckedInt(source.height, "scene height");
  push.scatter_width    = CheckedInt(scatter.Width(), "scatter width");
  push.scatter_height   = CheckedInt(scatter.Height(), "scatter height");
  push.scatter_fraction = layout.scatter_fraction;
  push.transmission     = layout.transmission;
  CopyAffine(push.render_to_base, mapping.render_to_base);
  const std::array<VulkanImageBinding, 3> images = {
      VulkanImageBinding{kernels::kSceneBinding, VulkanSceneImageBinding(backend, source)},
      VulkanImageBinding{kernels::kOutputBinding, VulkanSceneImageBinding(backend, output)},
      VulkanImageBinding{kernels::kCoarseBinding, &scatter}};
  Dispatch(device, kKernel, images, push, source.width, source.height);
}

}  // namespace

auto ExecuteVulkanDiffusionFilter(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                  const PipelineDocument& document, const FrameSceneBinding& scene)
    -> FrameSceneBinding {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    Fail("BeginRender has not been called");
  }
  const auto* drt = document.Drt();
  if (drt == nullptr) {
    Fail("missing DRT endpoint");
  }
  const auto output_binding = FrameSceneBinding::WorkImage(DestinationWorkMember(scene));
  const auto source         = VulkanBindScene(device, scene);
  const auto output         = VulkanBindScene(device, output_binding);
  if (output.width != source.width || output.height != source.height) {
    Fail("scene-work extent does not match scene");
  }
  (void)CheckedInt(source.width, "scene width");
  (void)CheckedInt(source.height, "scene height");

  const float strength = drt->Params().DiffusionStrength();
  if (!IsDiffusionFilterActive(strength)) {
    EncodeDecode(device, source, output);
    return output_binding;
  }

  const auto& geometry = plan.geometry;
  const auto  layout   = MakeDiffusionFilterLayout(
      DiffusionCanvasExtent(geometry.full_reference_extent), ResolveDiffusionFilterShape(strength));
  const auto                    mapping      = MakeDiffusionScatterMapping(geometry, layout);
  const auto                    base         = layout.extents[0];

  // The scatter image covers the full frame. A full-edit render builds and publishes it; an ROI
  // render samples the published image, so light outside the viewport still scatters in.
  auto&                         invalidation = workspace.ResultInvalidation();
  const auto                    scatter_id   = DiffusionScatterId(plan.drt.node_id);
  const bool                    persist      = workspace.PersistsResult(scatter_id);
  const bool                    full_edit    = CoversFullEditSpace(geometry);
  const auto                    long_edge    = (std::max)(source.width, source.height);
  ResourceLease<VulkanBackend>* canonical    = nullptr;
  if (persist) {
    const auto needed =
        invalidation.MakeImageRepresentation(scatter_id, base, TextureFormat::Rgba32f,
                                             DiffusionScatterRequiredDetail(full_edit, long_edge));
    canonical =
        workspace.Images().BindValidResult(scatter_id, invalidation.RequiredRevision(scatter_id),
                                           needed, workspace.Device().CompletedSubmission());
  } else {
    ++device.PassStats().result_policy_bypass;
  }
  const auto decision = DecideDiffusionScatter(persist, canonical != nullptr, full_edit, long_edge);

  const Texture*                              scatter = nullptr;
  std::optional<ResourceLease<VulkanBackend>> transient_scatter;
  if (decision.action == DiffusionScatterAction::SampleCanonical) {
    scatter = &canonical->Texture();
    RequireRgba32f(*scatter, base, "published scatter image");
    ++device.PassStats().diffusion_scatter_sample;
  } else {
    const Texture* target = nullptr;
    if (decision.persist_canonical) {
      target =
          &workspace
               .AcquireImageForWrite(scatter_id, {base.width, base.height, TextureFormat::Rgba32f})
               .Texture();
    } else {
      transient_scatter.emplace(
          workspace.Textures().Acquire({base.width, base.height, TextureFormat::Rgba32f}));
      target = &transient_scatter->Texture();
    }
    BuildScatter(device, source, layout, mapping, *target);
    if (decision.persist_canonical) {
      const auto published = invalidation.MakeImageRepresentation(
          scatter_id, base, TextureFormat::Rgba32f, decision.current_long_edge);
      workspace.Images().RecordUnpublished(scatter_id, invalidation.RequiredRevision(scatter_id),
                                           published, device.CommandContext().SubmissionId(),
                                           decision.current_long_edge);
    }
    scatter = target;
    ++device.PassStats().diffusion_scatter_rebuild;
  }

  EncodeMix(device, source, output, *scatter, layout, mapping);
  return output_binding;
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
