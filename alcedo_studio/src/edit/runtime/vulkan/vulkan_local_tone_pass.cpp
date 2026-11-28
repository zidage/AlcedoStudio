//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_VULKAN

#include "edit/runtime/vulkan/vulkan_local_tone_pass.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>

#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/local_tone_executor.hpp"
#include "edit/runtime/local_tone_mapping.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/vulkan/vulkan_kernel_dispatch.hpp"
#include "edit/runtime/vulkan/vulkan_scene_view.hpp"
#include "vulkan/vulkan_local_tone_kernels.hpp"

namespace alcedo {
namespace {

namespace kernels                      = vulkan::local_tone_kernels;

constexpr const char*   kErrorPrefix   = "ExecuteVulkanLocalTone";
/// Texels of one row of the zero fill: a plane of up to 2048 x 2048 texels needs at most 4096
/// rows, inside the workgroup count limit of every device.
constexpr std::uint32_t kFillRowTexels = 1024;

[[noreturn]] void       Fail(const std::string& message) {
  throw std::runtime_error(std::string(kErrorPrefix) + ": " + message);
}

/// One R32F plane: a range of the transient arena, read by device address.
struct Plane {
  VkDeviceAddress address = 0;
  std::size_t     bytes   = 0;
};

auto CheckedInt(std::uint32_t value, const char* label) -> std::int32_t {
  if (value == 0 ||
      value > static_cast<std::uint32_t>((std::numeric_limits<std::int32_t>::max)())) {
    Fail(std::string("invalid ") + label);
  }
  return static_cast<std::int32_t>(value);
}

auto TexelBytes(std::uint32_t width, std::uint32_t height) -> std::size_t {
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(float);
}

/// The plane @p plane after a check that it holds @p width x @p height texels.
auto RequirePlane(const Plane& plane, std::uint32_t width, std::uint32_t height, const char* label)
    -> VkDeviceAddress {
  if (plane.address == 0 || plane.bytes < TexelBytes(width, height)) {
    Fail(std::string("the ") + label + " plane is smaller than its level");
  }
  return plane.address;
}

void CopyTransform(float (&destination)[6], const Matrix3x3& matrix) {
  for (int index = 0; index < 6; ++index) {
    destination[index] = matrix.m[index];
  }
}

auto AllocateTransientPlane(VulkanRenderWorkspace& workspace, std::size_t bytes) -> Plane {
  void* pointer = workspace.TransientBuffers().Allocate(bytes);
  if (pointer == nullptr) {
    Fail("transient allocation failed");
  }
  return Plane{static_cast<VkDeviceAddress>(reinterpret_cast<std::uintptr_t>(pointer)), bytes};
}

template <class PushConstants>
void Dispatch(VulkanRenderDevice& device, const vulkan::VulkanKernelDescriptor& kernel,
              std::span<const VulkanImageBinding> images, const PushConstants& push,
              std::uint32_t width, std::uint32_t height) {
  if (width == 0 || height == 0) {
    Fail("empty dispatch extent");
  }
  RecordKernelDispatch(device.Workspace().Device(), device.CommandContext(), kernel, images,
                       PushConstantBytes(push),
                       WorkgroupsCovering(width, height, kernels::kGroupSize, kernels::kGroupSize));
}

/// The R32F image @p image into @p plane (the OpenCL `CopyImageToDeviceMemory`).
void CopyImageToPlane(VulkanRenderDevice& device, const VulkanBackend::Texture2D& image,
                      const Plane& plane) {
  if (image.Format() != TextureFormat::R32f) {
    Fail("a canonical plane image is not R32F");
  }
  static const auto           kKernel = kernels::CopyImageToPlaneDescriptor();
  kernels::PlanePushConstants push;
  push.plane  = RequirePlane(plane, image.Width(), image.Height(), "canonical copy");
  push.width  = CheckedInt(image.Width(), "canonical width");
  push.height = CheckedInt(image.Height(), "canonical height");
  const std::array<VulkanImageBinding, 1> images = {
      VulkanImageBinding{kernels::kPlaneBinding, &image}};
  Dispatch(device, kKernel, images, push, image.Width(), image.Height());
}

/// @p plane into the R32F image @p image (the OpenCL `CopyDeviceMemoryToImage`).
void CopyPlaneToImage(VulkanRenderDevice& device, const Plane& plane,
                      const VulkanBackend::Texture2D& image) {
  if (image.Format() != TextureFormat::R32f) {
    Fail("a canonical plane image is not R32F");
  }
  static const auto           kKernel = kernels::CopyPlaneToImageDescriptor();
  kernels::PlanePushConstants push;
  push.plane  = RequirePlane(plane, image.Width(), image.Height(), "canonical copy");
  push.width  = CheckedInt(image.Width(), "canonical width");
  push.height = CheckedInt(image.Height(), "canonical height");
  const std::array<VulkanImageBinding, 1> images = {
      VulkanImageBinding{kernels::kPlaneBinding, &image}};
  Dispatch(device, kKernel, images, push, image.Width(), image.Height());
}

auto CanonicalNeeded(RuntimeInvalidationState& invalidation, const GraphValueId& id,
                     const ResolvedRenderGeometry& geometry, int current_long_edge)
    -> ResultRepresentation {
  const auto canonical = local_tone_mapping::ComputeMaskDimensions(
      static_cast<int>(geometry.full_reference_extent.width),
      static_cast<int>(geometry.full_reference_extent.height),
      local_tone_mapping::kReferenceMaskMaxLongEdge);
  const ImageExtent extent{static_cast<std::uint32_t>(canonical.width),
                           static_cast<std::uint32_t>(canonical.height)};
  return invalidation.MakeImageRepresentation(id, extent, TextureFormat::R32f,
                                              static_cast<std::uint32_t>(current_long_edge));
}

auto BindCanonicalImage(VulkanRenderDevice& device, const GraphValueId& id,
                        const ResultRepresentation& needed) -> ResourceLease<VulkanBackend>* {
  return device.Workspace().Images().BindValidResult(
      id, device.Workspace().ResultInvalidation().RequiredRevision(id), needed,
      device.Workspace().Device().CompletedSubmission());
}

/// The Vulkan operations of `LocalToneExecutor`: the form of `OpenClLocalToneOps`.
struct VulkanLocalToneOps {
  using Device                              = VulkanRenderDevice;
  using Texture                             = VulkanBackend::Texture2D;
  using ScratchPlane                        = Plane;

  static constexpr const char* kErrorPrefix = "ExecuteVulkanLocalTone";

  static auto BindingWidth(VulkanRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return VulkanBindScene(device, binding).width;
  }

  static auto BindingHeight(VulkanRenderDevice& device, const FrameSceneBinding& binding)
      -> std::uint32_t {
    return VulkanBindScene(device, binding).height;
  }

  static auto TransientBytes(VulkanRenderDevice& device) -> std::size_t {
    return device.Workspace().TransientBuffers().used_bytes();
  }

  static auto LookupCanonical(VulkanRenderDevice& device, const GraphValueId& source_id,
                              const GraphValueId& result_id, int current_long_edge,
                              const ResolvedRenderGeometry& geometry) -> LocalToneCanonicalLookup {
    auto&      invalidation = device.Workspace().ResultInvalidation();
    const auto source_needed =
        CanonicalNeeded(invalidation, source_id, geometry, current_long_edge);
    const auto result_needed =
        CanonicalNeeded(invalidation, result_id, geometry, current_long_edge);
    auto*                    source = BindCanonicalImage(device, source_id, source_needed);
    LocalToneCanonicalLookup lookup;
    if (source == nullptr) {
      return lookup;
    }
    const auto long_edge    = device.Workspace().Images().PublishedAuxiliary(source_id);
    lookup.source_valid     = long_edge > 0;
    lookup.source_long_edge = static_cast<int>(long_edge);
    lookup.extent           = source_needed.extent;
    lookup.result_valid =
        lookup.source_valid && BindCanonicalImage(device, result_id, result_needed) != nullptr;
    return lookup;
  }

  static void ApplyCanonicalSampleAndMix(VulkanRenderDevice&      device,
                                         const FrameSceneBinding& original,
                                         const FrameSceneBinding& working,
                                         const FrameSceneBinding& adjusted, const NodeId& grade_id,
                                         float mix, const GraphValueId* mask_id,
                                         const LocalToneDecision& decision, std::uint32_t width,
                                         std::uint32_t height) {
    const auto source_id    = LocalToneSourceId(grade_id);
    const auto result_id    = LocalToneResultId(grade_id);
    auto&      invalidation = device.Workspace().ResultInvalidation();
    const auto needed       = invalidation.MakeImageRepresentation(
        source_id, decision.mask_extent, TextureFormat::R32f,
        static_cast<std::uint32_t>(decision.current_long_edge));
    const auto result_needed = invalidation.MakeImageRepresentation(
        result_id, decision.mask_extent, TextureFormat::R32f,
        static_cast<std::uint32_t>(decision.current_long_edge));
    auto* source = BindCanonicalImage(device, source_id, needed);
    auto* result = BindCanonicalImage(device, result_id, result_needed);
    if (source == nullptr || result == nullptr) {
      Fail("canonical sample lost published planes");
    }
    const auto bytes        = TexelBytes(decision.mask_extent.width, decision.mask_extent.height);
    const auto source_plane = AllocateTransientPlane(device.Workspace(), bytes);
    const auto result_plane = AllocateTransientPlane(device.Workspace(), bytes);
    CopyImageToPlane(device, source->Texture(), source_plane);
    CopyImageToPlane(device, result->Texture(), result_plane);
    ApplyAdjustedAndMix(device, original, working, adjusted, source_plane, result_plane, mix,
                        mask_id, width, height, static_cast<int>(decision.mask_extent.width),
                        static_cast<int>(decision.mask_extent.height), decision.apply_uv);
  }

  static auto CanonicalResourceId(VulkanRenderDevice& device, const GraphValueId& source_id)
      -> std::uint64_t {
    auto* image = device.Workspace().Images().Find(source_id);
    return image == nullptr ? 0 : image->Texture().ResourceId();
  }

  static auto BindCanonicalSourcePlane(VulkanRenderDevice& device, const GraphValueId& source_id,
                                       std::size_t bytes) -> Plane {
    auto* image = device.Workspace().Images().Find(source_id);
    if (image == nullptr || image->Empty()) {
      Fail("canonical source disappeared");
    }
    auto plane = AllocateTransientPlane(device.Workspace(), bytes);
    CopyImageToPlane(device, image->Texture(), plane);
    return plane;
  }

  static auto AllocateScratchPlane(VulkanRenderDevice& device, std::size_t bytes) -> Plane {
    return AllocateTransientPlane(device.Workspace(), bytes);
  }

  static void ExtractReference(VulkanRenderDevice& device, const FrameSceneBinding& input,
                               Plane dest, std::uint32_t width, std::uint32_t height,
                               const LocalToneDecision&      decision,
                               const ResolvedRenderGeometry& geometry) {
    static const auto                      kKernel = kernels::ExtractReferenceDescriptor();
    const auto                             source  = RequireInput(device, input, width, height);
    const auto                             out_w   = static_cast<std::uint32_t>(decision.widths[0]);
    const auto                             out_h = static_cast<std::uint32_t>(decision.heights[0]);
    kernels::ExtractReferencePushConstants push;
    push.source        = source.buffer;
    push.output        = RequirePlane(dest, out_w, out_h, "reference extract");
    push.input_width   = CheckedInt(width, "input width");
    push.input_height  = CheckedInt(height, "input height");
    push.output_width  = CheckedInt(out_w, "plane width");
    push.output_height = CheckedInt(out_h, "plane height");
    push.full_ref_w    = static_cast<float>(geometry.full_reference_extent.width);
    push.full_ref_h    = static_cast<float>(geometry.full_reference_extent.height);
    CopyTransform(push.reference_to_render, geometry.reference_to_render);
    const std::array<VulkanImageBinding, 1> images = {VulkanImageBinding{
        kernels::kSceneBinding, VulkanSceneImageBinding(device.Workspace().Device(), source)}};
    Dispatch(device, kKernel, images, push, out_w, out_h);
  }

  static void Extract(VulkanRenderDevice& device, const FrameSceneBinding& input, Plane dest,
                      std::uint32_t width, std::uint32_t height,
                      const LocalToneDecision& decision) {
    static const auto             kKernel = kernels::ExtractDescriptor();
    const auto                    source  = RequireInput(device, input, width, height);
    const auto                    out_w   = static_cast<std::uint32_t>(decision.widths[0]);
    const auto                    out_h   = static_cast<std::uint32_t>(decision.heights[0]);
    kernels::ExtractPushConstants push;
    push.source                                    = source.buffer;
    push.output                                    = RequirePlane(dest, out_w, out_h, "extract");
    push.input_width                               = CheckedInt(width, "input width");
    push.input_height                              = CheckedInt(height, "input height");
    push.output_width                              = CheckedInt(out_w, "plane width");
    push.output_height                             = CheckedInt(out_h, "plane height");
    const std::array<VulkanImageBinding, 1> images = {VulkanImageBinding{
        kernels::kSceneBinding, VulkanSceneImageBinding(device.Workspace().Device(), source)}};
    Dispatch(device, kKernel, images, push, out_w, out_h);
  }

  static void PyramidDown(VulkanRenderDevice& device, Plane src, Plane dst,
                          const LocalToneDecision& decision, int level) {
    static const auto kKernel = kernels::PyramidDownDescriptor();
    const auto        src_w   = static_cast<std::uint32_t>(decision.widths[level - 1]);
    const auto        src_h   = static_cast<std::uint32_t>(decision.heights[level - 1]);
    const auto        dst_w   = static_cast<std::uint32_t>(decision.widths[level]);
    const auto        dst_h   = static_cast<std::uint32_t>(decision.heights[level]);
    kernels::PyramidDownPushConstants push;
    push.source     = RequirePlane(src, src_w, src_h, "pyramid source");
    push.output     = RequirePlane(dst, dst_w, dst_h, "pyramid destination");
    push.src_width  = CheckedInt(src_w, "pyramid source width");
    push.src_height = CheckedInt(src_h, "pyramid source height");
    push.dst_width  = CheckedInt(dst_w, "pyramid width");
    push.dst_height = CheckedInt(dst_h, "pyramid height");
    Dispatch(device, kKernel, {}, push, dst_w, dst_h);
  }

  static void FillZero(VulkanRenderDevice& device, Plane plane) {
    static const auto kKernel = kernels::FillZeroDescriptor();
    if (plane.address == 0 || plane.bytes % sizeof(float) != 0 ||
        plane.bytes / sizeof(float) > (std::numeric_limits<std::uint32_t>::max)()) {
      Fail("invalid plane for the zero fill");
    }
    const auto                 count = static_cast<std::uint32_t>(plane.bytes / sizeof(float));
    kernels::FillPushConstants push;
    push.plane      = plane.address;
    push.count      = count;
    push.row_texels = kFillRowTexels;
    Dispatch(device, kKernel, {}, push, kFillRowTexels,
             (count + kFillRowTexels - 1) / kFillRowTexels);
  }

  static void Remap(VulkanRenderDevice& device, Plane src, Plane dst,
                    const LocalToneDecision& decision, const local_tone_mapping::LlfSample& sample,
                    float sigma) {
    static const auto           kKernel = kernels::RemapDescriptor();
    const auto                  width   = static_cast<std::uint32_t>(decision.widths[0]);
    const auto                  height  = static_cast<std::uint32_t>(decision.heights[0]);
    kernels::RemapPushConstants push;
    push.source = RequirePlane(src, width, height, "remap source");
    push.output = RequirePlane(dst, width, height, "remap destination");
    push.width  = CheckedInt(width, "remap width");
    push.height = CheckedInt(height, "remap height");
    push.gamma  = sample.gamma;
    push.target = sample.target;
    push.beta   = sample.beta;
    push.alpha  = sample.alpha;
    push.sigma  = sigma;
    Dispatch(device, kKernel, {}, push, width, height);
  }

  static void Select(VulkanRenderDevice& device, Plane source, Plane lo, Plane lo_coarse, Plane hi,
                     Plane hi_coarse, Plane output, const LocalToneDecision& decision, int level,
                     const local_tone_mapping::LlfSample& lo_sample,
                     const local_tone_mapping::LlfSample& hi_sample, bool first, bool last,
                     bool top) {
    static const auto kKernel  = kernels::SelectDescriptor();
    const auto        width    = static_cast<std::uint32_t>(decision.widths[level]);
    const auto        height   = static_cast<std::uint32_t>(decision.heights[level]);
    const auto        coarse_w = top ? 1U : static_cast<std::uint32_t>(decision.widths[level + 1]);
    const auto        coarse_h = top ? 1U : static_cast<std::uint32_t>(decision.heights[level + 1]);
    kernels::SelectPushConstants push;
    push.source        = RequirePlane(source, width, height, "select source");
    push.lo            = RequirePlane(lo, width, height, "select low");
    push.lo_coarse     = RequirePlane(lo_coarse, coarse_w, coarse_h, "select low coarse");
    push.hi            = RequirePlane(hi, width, height, "select high");
    push.hi_coarse     = RequirePlane(hi_coarse, coarse_w, coarse_h, "select high coarse");
    push.output        = RequirePlane(output, width, height, "select output");
    push.width         = CheckedInt(width, "select width");
    push.height        = CheckedInt(height, "select height");
    push.coarse_width  = CheckedInt(coarse_w, "select coarse width");
    push.coarse_height = CheckedInt(coarse_h, "select coarse height");
    push.gamma_lo      = lo_sample.gamma;
    push.gamma_hi      = hi_sample.gamma;
    push.first         = first ? 1 : 0;
    push.last          = last ? 1 : 0;
    push.top           = top ? 1 : 0;
    Dispatch(device, kKernel, {}, push, width, height);
  }

  static void Collapse(VulkanRenderDevice& device, Plane lap, Plane coarse, Plane output,
                       const LocalToneDecision& decision, int level) {
    static const auto kKernel  = kernels::CollapseDescriptor();
    const auto        width    = static_cast<std::uint32_t>(decision.widths[level]);
    const auto        height   = static_cast<std::uint32_t>(decision.heights[level]);
    const auto        coarse_w = static_cast<std::uint32_t>(decision.widths[level + 1]);
    const auto        coarse_h = static_cast<std::uint32_t>(decision.heights[level + 1]);
    kernels::CollapsePushConstants push;
    push.laplacian     = RequirePlane(lap, width, height, "collapse laplacian");
    push.coarse        = RequirePlane(coarse, coarse_w, coarse_h, "collapse coarse");
    push.output        = RequirePlane(output, width, height, "collapse output");
    push.width         = CheckedInt(width, "collapse width");
    push.height        = CheckedInt(height, "collapse height");
    push.coarse_width  = CheckedInt(coarse_w, "collapse coarse width");
    push.coarse_height = CheckedInt(coarse_h, "collapse coarse height");
    Dispatch(device, kKernel, {}, push, width, height);
  }

  static void ApplyAdjustedAndMix(VulkanRenderDevice& device, const FrameSceneBinding& original,
                                  const FrameSceneBinding& working,
                                  const FrameSceneBinding& adjusted, Plane reference, Plane tone,
                                  float mix, const GraphValueId* mask_id, std::uint32_t width,
                                  std::uint32_t height, int plane_width, int plane_height,
                                  const Matrix3x3& apply_uv) {
    if (!working.IsWorkImage() || adjusted != working) {
      Fail("apply requires one in-place scene work member");
    }
    static const auto kKernel = kernels::ApplyDescriptor();
    auto&             backend = device.Workspace().Device();
    const auto        target  = VulkanBindScene(device, working);
    if (target.width != width || target.height != height) {
      Fail("the working scene image differs in size");
    }
    const auto                  plane_w = static_cast<std::uint32_t>(plane_width);
    const auto                  plane_h = static_cast<std::uint32_t>(plane_height);
    kernels::ApplyPushConstants push;
    push.working         = target.buffer;
    push.reference       = RequirePlane(reference, plane_w, plane_h, "apply reference");
    push.adjusted        = RequirePlane(tone, plane_w, plane_h, "apply adjusted");
    push.width           = CheckedInt(width, "apply width");
    push.height          = CheckedInt(height, "apply height");
    push.adjusted_width  = CheckedInt(plane_w, "apply plane width");
    push.adjusted_height = CheckedInt(plane_h, "apply plane height");
    push.grade_mix       = mix;
    CopyTransform(push.render_to_uv, apply_uv);

    const VulkanBackend::Texture2D* mix_image  = &backend.DummyStorageImage(TextureFormat::Rgba32f);
    const VulkanBackend::Texture2D* mask_image = &backend.DummyStorageImage(TextureFormat::R8);
    if (mix != 1.0F || mask_id != nullptr) {
      const auto source = VulkanBindScene(device, original);
      if (source.width != width || source.height != height) {
        Fail("the mix source differs in size");
      }
      push.apply_mix  = 1;
      push.mix_source = source.buffer;
      mix_image       = VulkanSceneImageBinding(backend, source);
      if (mask_id != nullptr) {
        mask_image    = &VulkanRequireSceneMask(device, *mask_id, width, height, kErrorPrefix);
        push.has_mask = 1;
      }
    }
    const std::array<VulkanImageBinding, 2> images = {
        VulkanImageBinding{kernels::kSceneBinding, mix_image},
        VulkanImageBinding{kernels::kMaskBinding, mask_image}};
    Dispatch(device, kKernel, images, push, width, height);
  }

  static void PersistCanonical(VulkanRenderDevice& device, Plane plane, const GraphValueId& id,
                               const LocalToneDecision& decision, int current_long_edge) {
    auto&             workspace    = device.Workspace();
    auto&             invalidation = workspace.ResultInvalidation();
    const ImageExtent extent{decision.mask_extent.width, decision.mask_extent.height};
    auto&             image =
        workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R32f});
    CopyPlaneToImage(device, plane, image.Texture());
    const auto needed = invalidation.MakeImageRepresentation(
        id, extent, TextureFormat::R32f, static_cast<std::uint32_t>(current_long_edge));
    workspace.Images().RecordUnpublished(id, invalidation.RequiredRevision(id), needed,
                                         device.CommandContext().SubmissionId(),
                                         static_cast<std::uint64_t>(current_long_edge));
  }

  static void PersistCanonicalSource(VulkanRenderDevice& device, Plane plane,
                                     const GraphValueId&      source_id,
                                     const LocalToneDecision& decision, int current_long_edge) {
    PersistCanonical(device, plane, source_id, decision, current_long_edge);
  }

  static void PersistCanonicalResult(VulkanRenderDevice& device, Plane plane,
                                     const GraphValueId&      result_id,
                                     const LocalToneDecision& decision, int current_long_edge) {
    PersistCanonical(device, plane, result_id, decision, current_long_edge);
  }

 private:
  /// The scene view of the extract input after a check of its size.
  static auto RequireInput(VulkanRenderDevice& device, const FrameSceneBinding& input,
                           std::uint32_t width, std::uint32_t height) -> VulkanSceneView {
    const auto view = VulkanBindScene(device, input);
    if (view.width != width || view.height != height) {
      Fail("the extract input differs in size");
    }
    return view;
  }
};

}  // namespace

auto ExecuteVulkanLocalTone(VulkanRenderDevice& device, const FrameSceneBinding& adjusted,
                            const FrameSceneBinding& working, const FrameSceneBinding& original,
                            const NodeId& grade_id, float shadows_slider, float highlights_slider,
                            const ResolvedRenderGeometry& geometry, float mix,
                            const GraphValueId* mask_id) -> VulkanLocalToneResult {
  const auto executed = LocalToneExecutor<VulkanLocalToneOps>::Execute(
      device, adjusted, working, original, grade_id, shadows_slider, highlights_slider, geometry,
      mix, mask_id);
  VulkanLocalToneResult tone;
  tone.reference_resource_id       = executed.reference_resource_id;
  tone.rebuilt_reference           = executed.rebuilt_reference;
  tone.sampled_canonical_reference = executed.sampled_canonical_reference;
  tone.transient_bytes             = executed.transient_bytes;
  tone.trace                       = executed.trace;
  return tone;
}

}  // namespace alcedo

#endif  // HAVE_VULKAN
