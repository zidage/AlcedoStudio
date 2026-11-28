//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase VK11 (#1036): MaskEvaluate, MaskUnion, and the coverage displays of a Color Grade Mask on
// Vulkan, the form of opencl_mask_pass.cpp. The kernels are the group mask (shader/mask.slang).

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_mask_pass.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "edit/runtime/vulkan/vulkan_scene_view.hpp"
#include "vulkan/vulkan_mask_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels = vulkan::mask_kernels;

static_assert(sizeof(MaskRangeParams) == sizeof(kernels::AnalyticPushConstants::range));
static_assert(sizeof(MaskRangeParams) == sizeof(kernels::RasterPushConstants::range));

auto EnsureOutput(VulkanRenderWorkspace& workspace, const GraphValueId& id, Extent2D extent)
    -> ResourceLease<VulkanBackend>& {
  return workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R8});
}

void CopyTransform(float (&destination)[6], const Matrix3x3& matrix) {
  for (int index = 0; index < 6; ++index) {
    destination[index] = matrix.m[index];
  }
}

/// The bytes of @p range into the push constant words (mask.slang declares the same fields).
void CopyRange(std::uint32_t (&destination)[9], const MaskRangeParams& range) {
  std::memcpy(destination, &range, sizeof(range));
}

template <class PushConstants>
void Dispatch(VulkanRenderDevice& device, const vulkan::VulkanKernelDescriptor& kernel,
              std::span<const VulkanImageBinding> images, const PushConstants& push,
              std::uint32_t width, std::uint32_t height, const char* error_prefix) {
  if (width == 0 || height == 0) {
    throw std::runtime_error(std::string(error_prefix) + ": empty dispatch extent");
  }
  RecordKernelDispatch(device.Workspace().Device(), device.CommandContext(), kernel, images,
                       PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

/// The Grade input of a Mask with a restricted range, or empty when the Mask has no range.
auto RangeSceneView(VulkanRenderDevice& device, const MaskModel& mask,
                    const FrameSceneBinding& range_scene, Extent2D extent, const char* error_prefix)
    -> std::optional<VulkanSceneView> {
  if (!MaskHasRestrictedRange(mask)) {
    return std::nullopt;
  }
  const auto not_of_the_extent = [&] {
    return std::runtime_error(std::string(error_prefix) +
                              ": the Grade input of a Mask range is not an image of the render "
                              "extent");
  };
  auto& workspace = device.Workspace();
  if (range_scene.IsWorkImage()) {
    const auto& member = workspace.SceneWork().Member(range_scene.member);
    if (member.Empty() || member.Width() != extent.width || member.Height() != extent.height) {
      throw not_of_the_extent();
    }
  } else {
    const auto* lease = workspace.Images().Find(range_scene.graph_id);
    if (lease == nullptr || lease->Empty() || lease->Texture().Width() != extent.width ||
        lease->Texture().Height() != extent.height) {
      throw not_of_the_extent();
    }
    if (lease->Texture().Format() != TextureFormat::Rgba32f) {
      throw std::runtime_error(std::string(error_prefix) +
                               ": the Grade input of a Mask range is not an RGBA32F image");
    }
  }
  return VulkanBindScene(device, range_scene);
}

/// The scene image binding and the push constant address of the ranges of one dispatch.
struct RangeBinding {
  const VulkanBackend::Texture2D* image = nullptr;
  VkDeviceAddress                 scene = 0;
  MaskRangeParams                 range{};
};

auto MakeRangeBinding(VulkanRenderDevice& device, const MaskModel& mask,
                      const std::optional<VulkanSceneView>& scene) -> RangeBinding {
  RangeBinding binding;
  binding.image = &device.Workspace().Device().DummyStorageImage(TextureFormat::Rgba32f);
  if (scene.has_value()) {
    binding.image = VulkanSceneImageBinding(device.Workspace().Device(), *scene);
    binding.scene = scene->buffer;
    binding.range = MaskRangeParamsFromMask(mask);
  }
  return binding;
}

void EncodeAnalytic(VulkanRenderDevice& device, VulkanBackend::Texture2D& output,
                    const MaskModel& mask, const ExecutionPlan& plan, const RangeBinding& range) {
  static const auto              kKernel = kernels::AnalyticDescriptor();
  kernels::AnalyticPushConstants push;
  CopyTransform(push.render_to_reference, plan.geometry.render_to_reference);
  push.width            = output.Width();
  push.height           = output.Height();
  push.reference_width  = plan.geometry.full_reference_extent.width;
  push.reference_height = plan.geometry.full_reference_extent.height;
  push.kind             = AnalyticKindFromMask(mask) == AnalyticMaskKind::Radial ? 0U : 1U;
  const auto radial     = RadialParamsFromMask(mask);
  push.invert           = radial.invert ? 1U : 0U;
  push.opacity          = mask.opacity;
  if (push.kind == 0U) {
    using Shape                                        = kernels::RadialShape;
    push.shape[static_cast<int>(Shape::kCenterX)]      = radial.center_x;
    push.shape[static_cast<int>(Shape::kCenterY)]      = radial.center_y;
    push.shape[static_cast<int>(Shape::kMajorRadius)]  = radial.major_radius;
    push.shape[static_cast<int>(Shape::kMinorRadius)]  = radial.minor_radius;
    push.shape[static_cast<int>(Shape::kRotation)]     = radial.rotation;
    push.shape[static_cast<int>(Shape::kInnerFeather)] = radial.inner_feather;
    push.shape[static_cast<int>(Shape::kOuterFeather)] = radial.outer_feather;
  } else {
    using Shape                                              = kernels::LinearShape;
    const auto gradient                                      = LinearGradientParamsFromMask(mask);
    push.shape[static_cast<int>(Shape::kOriginX)]            = gradient.origin_x;
    push.shape[static_cast<int>(Shape::kOriginY)]            = gradient.origin_y;
    push.shape[static_cast<int>(Shape::kNormalX)]            = gradient.normal_x;
    push.shape[static_cast<int>(Shape::kNormalY)]            = gradient.normal_y;
    push.shape[static_cast<int>(Shape::kTransitionDistance)] = gradient.transition_distance;
    push.shape[static_cast<int>(Shape::kStartValue)]         = gradient.start_value;
    push.shape[static_cast<int>(Shape::kEndValue)]           = gradient.end_value;
  }
  push.scene = range.scene;
  CopyRange(push.range, range.range);
  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{kernels::kSceneBinding, range.image},
      VulkanImageBinding{kernels::kOutputR8Binding, &output}};
  Dispatch(device, kKernel, images, push, output.Width(), output.Height(), "ExecuteVulkanMask");
}

/// The raster sampling of @p coverage on the render grid of @p geometry into @p output: R8 for
/// MaskEvaluate, RGBA32F (value, 0, 0, 1) for a coverage display.
void EncodeRasterSampling(VulkanRenderDevice&                     device,
                          const MaskCoverageImage<VulkanBackend>& coverage,
                          const ResolvedRenderGeometry& geometry, bool invert, float opacity,
                          VulkanBackend::Texture2D& output, const RangeBinding& range,
                          const char* error_prefix) {
  const auto sampling = MakeRasterMaskSamplingPlan(geometry, coverage.descriptor.reference_bounds,
                                                   coverage.descriptor.extent);
  const bool display  = output.Format() == TextureFormat::Rgba32f;
  static const auto            kRaster        = kernels::RasterDescriptor();
  static const auto            kRasterDisplay = kernels::RasterDisplayDescriptor();
  kernels::RasterPushConstants push;
  CopyTransform(push.render_to_texture_uv, sampling.render_to_texture_uv);
  push.width          = output.Width();
  push.height         = output.Height();
  push.texture_width  = coverage.descriptor.extent.width;
  push.texture_height = coverage.descriptor.extent.height;
  push.samples        = RasterMaskSamplesPerAxis(sampling);
  push.invert         = invert ? 1U : 0U;
  push.opacity        = opacity;
  push.scene          = range.scene;
  CopyRange(push.range, range.range);
  const std::array<VulkanImageBinding, 3> images = {
      VulkanImageBinding{kernels::kSceneBinding, range.image},
      VulkanImageBinding{kernels::kSourceBinding, &coverage.texture},
      VulkanImageBinding{display ? kernels::kOutputRgbaBinding : kernels::kOutputR8Binding,
                         &output}};
  Dispatch(device, display ? kRasterDisplay : kRaster, images, push, output.Width(),
           output.Height(), error_prefix);
}

void EncodeFillZero(VulkanRenderDevice& device, VulkanBackend::Texture2D& output) {
  static const auto                       kKernel = kernels::FillZeroDescriptor();
  kernels::ExtentPushConstants            push{output.Width(), output.Height()};
  const std::array<VulkanImageBinding, 1> images = {
      VulkanImageBinding{kernels::kOutputR8Binding, &output}};
  Dispatch(device, kKernel, images, push, output.Width(), output.Height(),
           "ExecuteVulkanMaskUnion");
}

void EncodeUnionMax(VulkanRenderDevice& device, const VulkanBackend::Texture2D& source,
                    VulkanBackend::Texture2D& output) {
  static const auto                       kKernel = kernels::UnionMaxDescriptor();
  kernels::ExtentPushConstants            push{output.Width(), output.Height()};
  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{kernels::kSourceBinding, &source},
      VulkanImageBinding{kernels::kOutputR8Binding, &output}};
  Dispatch(device, kKernel, images, push, output.Width(), output.Height(),
           "ExecuteVulkanMaskUnion");
}

void RequireDisplayOutput(const VulkanBackend::Texture2D& output, Extent2D extent,
                          const char* message) {
  if (output.Empty() || output.Format() != TextureFormat::Rgba32f ||
      output.Width() != extent.width || output.Height() != extent.height) {
    throw std::runtime_error(message);
  }
}

}  // namespace

auto ExecuteVulkanMask(VulkanRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                       const CompiledMaskSource& compiled_source,
                       const FrameSceneBinding&  range_scene) -> VulkanMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteVulkanMask: BeginRender has not been called");
  }
  const auto extent = plan.geometry.render_extent;
  if (extent.Empty() || plan.geometry.full_reference_extent.Empty()) {
    throw std::runtime_error("ExecuteVulkanMask: geometry extents must be positive");
  }
  const auto& mask_model =
      RequireMaskModel(document, compiled_grade.node_id, compiled_source.mask_id);
  const auto* ai = std::get_if<AiSegmentationMaskSource>(&mask_model.source);
  if (ai == nullptr && !std::holds_alternative<RadialMaskSource>(mask_model.source) &&
      !std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    throw std::runtime_error("ExecuteVulkanMask: compiled mask does not match document");
  }
  const auto scene = RangeSceneView(device, mask_model, range_scene, extent, "ExecuteVulkanMask");
  const auto range = MakeRangeBinding(device, mask_model, scene);

  auto&      workspace = device.Workspace();
  auto&      output    = EnsureOutput(workspace, compiled_source.effective_output, extent);
  if (ai == nullptr) {
    EncodeAnalytic(device, output.Texture(), mask_model, plan, range);
  } else {
    const auto coverage = workspace.AcquireAiMaskCoverage(*ai, device.CommandContext());
    EncodeRasterSampling(device, *coverage, plan.geometry, mask_model.invert, mask_model.opacity,
                         output.Texture(), range, "ExecuteVulkanMask");
  }
  return VulkanMaskResult{compiled_source.effective_output};
}

auto ExecuteVulkanMaskUnion(VulkanRenderDevice& device, const ExecutionPlan& plan,
                            const PipelineDocument&  document,
                            const CompiledGradeNode& compiled_grade) -> VulkanMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteVulkanMaskUnion: BeginRender has not been called");
  }
  const auto&               stack     = RequireMaskStack(compiled_grade);
  auto&                     workspace = device.Workspace();
  const auto                extent    = plan.geometry.render_extent;
  std::vector<GraphValueId> enabled;
  enabled.reserve(stack.sources.size());
  for (const auto& source : stack.sources) {
    if (MaskSourceIsEnabled(document, compiled_grade.node_id, source.mask_id)) {
      enabled.push_back(source.effective_output);
    }
  }
  if (enabled.empty()) {
    auto& output = EnsureOutput(workspace, stack.union_output, extent);
    EncodeFillZero(device, output.Texture());
    return VulkanMaskResult{stack.union_output};
  }
  if (enabled.size() == 1) {
    (void)workspace.AliasImageFrom(stack.union_output, enabled.front());
    return VulkanMaskResult{stack.union_output};
  }
  // Every source must exist before the first command: a failure records nothing.
  std::vector<const VulkanBackend::Texture2D*> sources;
  sources.reserve(enabled.size());
  for (const auto& id : enabled) {
    const auto* image = workspace.Images().Find(id);
    if (image == nullptr || image->Empty()) {
      throw std::runtime_error("ExecuteVulkanMaskUnion: missing enabled Mask source");
    }
    sources.push_back(&image->Texture());
  }
  auto& output = EnsureOutput(workspace, stack.union_output, extent);
  workspace.Device().CopyTexture2D(*sources.front(), output.Texture(), device.CommandContext());
  for (std::size_t index = 1; index < sources.size(); ++index) {
    EncodeUnionMax(device, *sources[index], output.Texture());
  }
  return VulkanMaskResult{stack.union_output};
}

auto ExecuteVulkanMask(VulkanRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document, const CompiledGradeNode& compiled_grade)
    -> VulkanMaskResult {
  const auto& stack = RequireMaskStack(compiled_grade);
  for (const auto& source : stack.sources) {
    if (!MaskSourceIsEnabled(document, compiled_grade.node_id, source.mask_id)) {
      continue;
    }
    (void)ExecuteVulkanMask(device, plan, document, compiled_grade, source,
                            FrameSceneBinding::CachedImage(source.range_input));
  }
  return ExecuteVulkanMaskUnion(device, plan, document, compiled_grade);
}

auto ExecuteVulkanMask(VulkanRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document) -> VulkanMaskResult {
  bool             any = false;
  VulkanMaskResult result{};
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask_stack.has_value()) {
      any    = true;
      result = ExecuteVulkanMask(device, plan, document, grade);
    }
  }
  if (!any) {
    throw std::runtime_error("ExecuteVulkanMask: plan has no mask");
  }
  return result;
}

void RecordMaskCoverageDisplay(VulkanRenderDevice&                     device,
                               const MaskCoverageImage<VulkanBackend>& coverage,
                               const MaskCoverageDisplayRequest&       request,
                               VulkanBackend::Texture2D&               output) {
  RequireDisplayOutput(
      output, request.geometry.render_extent,
      "RecordMaskCoverageDisplay: the output is not an RGBA32F image of the photo frame size");
  RangeBinding no_range;
  no_range.image = &device.Workspace().Device().DummyStorageImage(TextureFormat::Rgba32f);
  EncodeRasterSampling(device, coverage, request.geometry, request.invert, request.opacity, output,
                       no_range, "RecordMaskCoverageDisplay");
}

void RecordMaskCandidateRangeDisplay(VulkanRenderDevice& device, const ExecutionPlan& plan,
                                     const MaskCoverageImage<VulkanBackend>& coverage,
                                     const MaskModel& mask, const FrameSceneBinding& range_scene,
                                     VulkanBackend::Texture2D& output) {
  const auto extent = plan.geometry.render_extent;
  RequireDisplayOutput(
      output, extent,
      "RecordMaskCandidateRangeDisplay: the output is not an RGBA32F image of the render extent");
  const auto scene =
      RangeSceneView(device, mask, range_scene, extent, "RecordMaskCandidateRangeDisplay");
  EncodeRasterSampling(device, coverage, plan.geometry, mask.invert, mask.opacity, output,
                       MakeRangeBinding(device, mask, scene), "RecordMaskCandidateRangeDisplay");
}

void RecordMaskValueDisplay(VulkanRenderDevice& device, const VulkanBackend::Texture2D& mask,
                            VulkanBackend::Texture2D& output) {
  if (mask.Empty() || mask.Format() != TextureFormat::R8) {
    throw std::runtime_error("RecordMaskValueDisplay: the Mask coverage is not an R8 image");
  }
  RequireDisplayOutput(
      output, Extent2D{mask.Width(), mask.Height()},
      "RecordMaskValueDisplay: the output is not an RGBA32F image of the Mask coverage size");
  static const auto                       kKernel = kernels::ValueDisplayDescriptor();
  kernels::ExtentPushConstants            push{output.Width(), output.Height()};
  const std::array<VulkanImageBinding, 2> images = {
      VulkanImageBinding{kernels::kSourceBinding, &mask},
      VulkanImageBinding{kernels::kOutputRgbaBinding, &output}};
  Dispatch(device, kKernel, images, push, output.Width(), output.Height(),
           "RecordMaskValueDisplay");
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
