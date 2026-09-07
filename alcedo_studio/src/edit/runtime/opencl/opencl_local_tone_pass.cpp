//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_local_tone_pass.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/local_tone_executor.hpp"
#include "edit/runtime/local_tone_plan.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

using local_tone_mapping::LlfSample;

struct Plane {
  void*         ptr          = nullptr;
  cl_mem        native       = nullptr;
  std::uint32_t offset_bytes = 0;
  std::size_t   bytes        = 0;
};

struct ExtractParams {
  std::int32_t input_width   = 0;
  std::int32_t input_height  = 0;
  std::int32_t output_width  = 0;
  std::int32_t output_height = 0;
};

struct ExtractReferenceParams {
  std::int32_t input_width   = 0;
  std::int32_t input_height  = 0;
  std::int32_t output_width  = 0;
  std::int32_t output_height = 0;
  float        full_ref_w    = 0.0f;
  float        full_ref_h    = 0.0f;
  float        pad0          = 0.0f;
  float        pad1          = 0.0f;
  float        reference_to_render[12]{};
};

struct RemapParams {
  std::int32_t width  = 0;
  std::int32_t height = 0;
  float        gamma  = 0.0f;
  float        target = 0.0f;
  float        beta   = 1.0f;
  float        alpha  = 1.0f;
  float        sigma  = 0.0f;
  std::int32_t pad    = 0;
};

struct PyrDownParams {
  std::int32_t src_width  = 0;
  std::int32_t src_height = 0;
  std::int32_t dst_width  = 0;
  std::int32_t dst_height = 0;
};

struct SelectParams {
  std::int32_t width         = 0;
  std::int32_t height        = 0;
  std::int32_t coarse_width  = 0;
  std::int32_t coarse_height = 0;
  float        gamma_lo      = 0.0f;
  float        gamma_hi      = 0.0f;
  std::int32_t first         = 0;
  std::int32_t last          = 0;
  std::int32_t top           = 0;
  std::int32_t pad0          = 0;
  std::int32_t pad1          = 0;
  std::int32_t pad2          = 0;
};

struct CollapseParams {
  std::int32_t width         = 0;
  std::int32_t height        = 0;
  std::int32_t coarse_width  = 0;
  std::int32_t coarse_height = 0;
};

struct ApplyParams {
  std::int32_t width           = 0;
  std::int32_t height          = 0;
  std::int32_t adjusted_width  = 0;
  std::int32_t adjusted_height = 0;
  float        render_to_uv[12]{};
};

auto CheckedInt(std::uint32_t value, const char* label) -> int {
  if (value == 0 || value > static_cast<std::uint32_t>((std::numeric_limits<int>::max)())) {
    throw std::runtime_error(std::string{"ExecuteOpenClLocalTone: invalid "} + label);
  }
  return static_cast<int>(value);
}

auto PlaneBytes(int width, int height) -> std::size_t {
  return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * sizeof(float);
}

auto OffsetFloats(const Plane& plane) -> cl_uint {
  if (plane.native == nullptr || plane.offset_bytes % sizeof(float) != 0 ||
      plane.offset_bytes / sizeof(float) > (std::numeric_limits<cl_uint>::max)()) {
    throw std::runtime_error("ExecuteOpenClLocalTone: invalid transient plane offset");
  }
  return static_cast<cl_uint>(plane.offset_bytes / sizeof(float));
}

void SetMem(cl_kernel kernel, cl_uint index, cl_mem value, const char* label) {
  if (value == nullptr) {
    throw std::runtime_error(std::string{"ExecuteOpenClLocalTone: missing "} + label);
  }
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(cl_mem), &value), label);
}

void SetUInt(cl_kernel kernel, cl_uint index, cl_uint value, const char* label) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(cl_uint), &value), label);
}

template <typename Params>
void SetParams(cl_kernel kernel, cl_uint index, const Params& params, const char* label) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(params), &params), label);
}

void SetPlaneOffset(cl_kernel kernel, cl_uint index, const Plane& plane, const char* label) {
  SetUInt(kernel, index, OffsetFloats(plane), label);
}

void Dispatch2D(OpenClRenderDevice& device, cl_kernel kernel, std::uint32_t width,
                std::uint32_t height) {
  if (width == 0 || height == 0) {
    throw std::runtime_error("ExecuteOpenClLocalTone: empty dispatch extent");
  }
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 2, nullptr,
                                     global, local, 0, nullptr, &event),
              "OpenCL Local Tone enqueue");
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

auto AllocateTransientPlane(OpenClRenderWorkspace& workspace, std::size_t bytes) -> Plane {
  void* ptr = workspace.TransientBuffers().Allocate(bytes);
  if (ptr == nullptr) {
    throw std::runtime_error("ExecuteOpenClLocalTone: transient allocation failed");
  }
  const auto resolved = workspace.Device().ResolveDeviceMemory(ptr, bytes);
  return Plane{ptr, resolved.first, resolved.second, bytes};
}

void CopyMatrix(float* destination, const Matrix3x3& matrix) {
  for (int index = 0; index < 9; ++index) {
    destination[index] = matrix.m[index];
  }
}

void EnqueueLlfApply(OpenClRenderDevice& device, const OpenClBackend::Texture2D& input,
                   OpenClBackend::Texture2D& output, const Plane& reference, const Plane& adjusted,
                   std::uint32_t width, std::uint32_t height, int adjusted_width,
                   int adjusted_height, const Matrix3x3& render_to_uv) {
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kLocalToneProgramName,
                                                        OpenCL::GpuDag::kLocalToneApplyKernelName);
  ApplyParams params;
  params.width           = CheckedInt(width, "apply width");
  params.height          = CheckedInt(height, "apply height");
  params.adjusted_width  = adjusted_width;
  params.adjusted_height = adjusted_height;
  CopyMatrix(params.render_to_uv, render_to_uv);
  const auto input_mem  = input.Native();
  const auto output_mem = output.Native();
  SetMem(kernel, 0, input_mem, "apply input");
  SetMem(kernel, 1, output_mem, "apply output");
  SetMem(kernel, 2, reference.native, "apply reference");
  SetMem(kernel, 3, adjusted.native, "apply adjusted");
  SetParams(kernel, 4, params, "apply parameters");
  SetPlaneOffset(kernel, 5, reference, "apply reference offset");
  SetPlaneOffset(kernel, 6, adjusted, "apply adjusted offset");
  Dispatch2D(device, kernel, width, height);
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

auto BindCanonicalImage(OpenClRenderDevice& device, const GraphValueId& id,
                        const ResultRepresentation& needed) -> ResourceLease<OpenClBackend>* {
  return device.Workspace().Images().BindValidResult(
      id, device.Workspace().ResultInvalidation().RequiredRevision(id), needed,
      device.Workspace().Device().CompletedSubmission());
}

struct OpenClLocalToneOps {
  using Device       = OpenClRenderDevice;
  using Texture      = OpenClBackend::Texture2D;
  using ScratchPlane = Plane;

  static constexpr const char* kErrorPrefix = "ExecuteOpenClLocalTone";

  static auto TextureWidth(const Texture& texture) -> std::uint32_t { return texture.Width(); }
  static auto TextureHeight(const Texture& texture) -> std::uint32_t { return texture.Height(); }
  static auto TransientBytes(OpenClRenderDevice& device) -> std::size_t {
    return device.Workspace().TransientBuffers().used_bytes();
  }

  static auto LookupCanonical(OpenClRenderDevice& device, const GraphValueId& source_id,
                              const GraphValueId& result_id, int current_long_edge,
                              const ResolvedRenderGeometry& geometry) -> LocalToneCanonicalLookup {
    auto&      invalidation  = device.Workspace().ResultInvalidation();
    const auto source_needed = CanonicalNeeded(invalidation, source_id, geometry, current_long_edge);
    const auto result_needed = CanonicalNeeded(invalidation, result_id, geometry, current_long_edge);
    auto*      source        = BindCanonicalImage(device, source_id, source_needed);
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

  static void ApplyCanonicalSample(OpenClRenderDevice& device, const Texture& input,
                                   Texture& output, const GraphValueId& source_id,
                                   const GraphValueId& result_id, const LocalToneDecision& decision,
                                   std::uint32_t width, std::uint32_t height) {
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
      throw std::runtime_error("ExecuteOpenClLocalTone: canonical sample lost published planes");
    }
    auto&      workspace    = device.Workspace();
    const auto bytes        = PlaneBytes(static_cast<int>(decision.mask_extent.width),
                                         static_cast<int>(decision.mask_extent.height));
    const auto source_plane = AllocateTransientPlane(workspace, bytes);
    const auto result_plane = AllocateTransientPlane(workspace, bytes);
    workspace.Device().CopyImageToDeviceMemory(source->Texture(), source_plane.ptr,
                                               source_plane.bytes, device.CommandContext());
    workspace.Device().CopyImageToDeviceMemory(result->Texture(), result_plane.ptr,
                                               result_plane.bytes, device.CommandContext());
    EnqueueLlfApply(device, input, output, source_plane, result_plane, width, height,
                    static_cast<int>(decision.mask_extent.width),
                    static_cast<int>(decision.mask_extent.height), decision.apply_uv);
  }

  static auto CanonicalResourceId(OpenClRenderDevice& device, const GraphValueId& source_id)
      -> std::uint64_t {
    auto* image = device.Workspace().Images().Find(source_id);
    return image == nullptr ? 0 : image->Texture().ResourceId();
  }

  static auto BindCanonicalSourcePlane(OpenClRenderDevice& device, const GraphValueId& source_id,
                                       std::size_t bytes) -> Plane {
    auto* image = device.Workspace().Images().Find(source_id);
    if (image == nullptr) {
      throw std::runtime_error("ExecuteOpenClLocalTone: canonical source disappeared");
    }
    auto plane = AllocateTransientPlane(device.Workspace(), bytes);
    device.Workspace().Device().CopyImageToDeviceMemory(image->Texture(), plane.ptr, plane.bytes,
                                                        device.CommandContext());
    return plane;
  }

  static auto AllocateScratchPlane(OpenClRenderDevice& device, std::size_t bytes) -> Plane {
    return AllocateTransientPlane(device.Workspace(), bytes);
  }

  static void ExtractReference(OpenClRenderDevice& device, const Texture& input, Plane dest,
                               std::uint32_t width, std::uint32_t height,
                               const LocalToneDecision& decision,
                               const ResolvedRenderGeometry& geometry) {
    auto kernel = OpenClKernelCache::Instance().GetKernel(
        OpenCL::GpuDag::kLocalToneProgramName, OpenCL::GpuDag::kLocalToneExtractReferenceKernelName);
    ExtractReferenceParams params;
    params.input_width   = CheckedInt(width, "input width");
    params.input_height  = CheckedInt(height, "input height");
    params.output_width  = decision.widths[0];
    params.output_height = decision.heights[0];
    params.full_ref_w    = static_cast<float>(geometry.full_reference_extent.width);
    params.full_ref_h    = static_cast<float>(geometry.full_reference_extent.height);
    CopyMatrix(params.reference_to_render, geometry.reference_to_render);
    SetMem(kernel, 0, input.Native(), "reference extract input");
    SetMem(kernel, 1, dest.native, "reference extract output");
    SetParams(kernel, 2, params, "reference extract parameters");
    SetPlaneOffset(kernel, 3, dest, "reference extract output offset");
    Dispatch2D(device, kernel, static_cast<std::uint32_t>(decision.widths[0]),
               static_cast<std::uint32_t>(decision.heights[0]));
  }

  static void Extract(OpenClRenderDevice& device, const Texture& input, Plane dest,
                      std::uint32_t width, std::uint32_t height,
                      const LocalToneDecision& decision) {
    auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kLocalToneProgramName,
                                                        OpenCL::GpuDag::kLocalToneExtractKernelName);
    ExtractParams params;
    params.input_width   = CheckedInt(width, "input width");
    params.input_height  = CheckedInt(height, "input height");
    params.output_width  = decision.widths[0];
    params.output_height = decision.heights[0];
    SetMem(kernel, 0, input.Native(), "local extract input");
    SetMem(kernel, 1, dest.native, "local extract output");
    SetParams(kernel, 2, params, "local extract parameters");
    SetPlaneOffset(kernel, 3, dest, "local extract output offset");
    Dispatch2D(device, kernel, static_cast<std::uint32_t>(decision.widths[0]),
               static_cast<std::uint32_t>(decision.heights[0]));
  }

  static void PyramidDown(OpenClRenderDevice& device, Plane src, Plane dst,
                          const LocalToneDecision& decision, int level) {
    auto kernel = OpenClKernelCache::Instance().GetKernel(
        OpenCL::GpuDag::kLocalToneProgramName, OpenCL::GpuDag::kLocalTonePyramidDownKernelName);
    PyrDownParams params;
    params.src_width  = decision.widths[level - 1];
    params.src_height = decision.heights[level - 1];
    params.dst_width  = decision.widths[level];
    params.dst_height = decision.heights[level];
    SetMem(kernel, 0, src.native, "pyramid source");
    SetMem(kernel, 1, dst.native, "pyramid destination");
    SetParams(kernel, 2, params, "pyramid parameters");
    SetPlaneOffset(kernel, 3, src, "pyramid source offset");
    SetPlaneOffset(kernel, 4, dst, "pyramid destination offset");
    Dispatch2D(device, kernel, static_cast<std::uint32_t>(decision.widths[level]),
               static_cast<std::uint32_t>(decision.heights[level]));
  }

  static void FillZero(OpenClRenderDevice& device, Plane plane) {
    device.Workspace().Device().FillDeviceMemory(plane.ptr, plane.bytes, 0,
                                                device.CommandContext());
  }

  static void Remap(OpenClRenderDevice& device, Plane src, Plane dst,
                    const LocalToneDecision& decision, const local_tone_mapping::LlfSample& sample,
                    float sigma) {
    auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kLocalToneProgramName,
                                                        OpenCL::GpuDag::kLocalToneRemapKernelName);
    RemapParams params;
    params.width  = decision.widths[0];
    params.height = decision.heights[0];
    params.gamma  = sample.gamma;
    params.target = sample.target;
    params.beta   = sample.beta;
    params.alpha  = sample.alpha;
    params.sigma  = sigma;
    SetMem(kernel, 0, src.native, "remap source");
    SetMem(kernel, 1, dst.native, "remap destination");
    SetParams(kernel, 2, params, "remap parameters");
    SetPlaneOffset(kernel, 3, src, "remap source offset");
    SetPlaneOffset(kernel, 4, dst, "remap destination offset");
    Dispatch2D(device, kernel, static_cast<std::uint32_t>(decision.widths[0]),
               static_cast<std::uint32_t>(decision.heights[0]));
  }

  static void Select(OpenClRenderDevice& device, Plane source, Plane lo, Plane lo_coarse, Plane hi,
                     Plane hi_coarse, Plane output, const LocalToneDecision& decision, int level,
                     const local_tone_mapping::LlfSample& lo_sample,
                     const local_tone_mapping::LlfSample& hi_sample, bool first, bool last,
                     bool top) {
    auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kLocalToneProgramName,
                                                        OpenCL::GpuDag::kLocalToneSelectKernelName);
    SelectParams params;
    params.width         = decision.widths[level];
    params.height        = decision.heights[level];
    params.coarse_width  = top ? 1 : decision.widths[level + 1];
    params.coarse_height = top ? 1 : decision.heights[level + 1];
    params.gamma_lo      = lo_sample.gamma;
    params.gamma_hi      = hi_sample.gamma;
    params.first         = first ? 1 : 0;
    params.last          = last ? 1 : 0;
    params.top           = top ? 1 : 0;
    SetMem(kernel, 0, source.native, "select source");
    SetMem(kernel, 1, lo.native, "select low");
    SetMem(kernel, 2, lo_coarse.native, "select low coarse");
    SetMem(kernel, 3, hi.native, "select high");
    SetMem(kernel, 4, hi_coarse.native, "select high coarse");
    SetMem(kernel, 5, output.native, "select output");
    SetParams(kernel, 6, params, "select parameters");
    SetPlaneOffset(kernel, 7, source, "select source offset");
    SetPlaneOffset(kernel, 8, lo, "select low offset");
    SetPlaneOffset(kernel, 9, lo_coarse, "select low coarse offset");
    SetPlaneOffset(kernel, 10, hi, "select high offset");
    SetPlaneOffset(kernel, 11, hi_coarse, "select high coarse offset");
    SetPlaneOffset(kernel, 12, output, "select output offset");
    Dispatch2D(device, kernel, static_cast<std::uint32_t>(decision.widths[level]),
               static_cast<std::uint32_t>(decision.heights[level]));
  }

  static void Collapse(OpenClRenderDevice& device, Plane lap, Plane coarse, Plane output,
                       const LocalToneDecision& decision, int level) {
    auto kernel = OpenClKernelCache::Instance().GetKernel(
        OpenCL::GpuDag::kLocalToneProgramName, OpenCL::GpuDag::kLocalToneCollapseKernelName);
    CollapseParams params;
    params.width         = decision.widths[level];
    params.height        = decision.heights[level];
    params.coarse_width  = decision.widths[level + 1];
    params.coarse_height = decision.heights[level + 1];
    SetMem(kernel, 0, lap.native, "collapse laplacian");
    SetMem(kernel, 1, coarse.native, "collapse coarse");
    SetMem(kernel, 2, output.native, "collapse output");
    SetParams(kernel, 3, params, "collapse parameters");
    SetPlaneOffset(kernel, 4, lap, "collapse laplacian offset");
    SetPlaneOffset(kernel, 5, coarse, "collapse coarse offset");
    SetPlaneOffset(kernel, 6, output, "collapse output offset");
    Dispatch2D(device, kernel, static_cast<std::uint32_t>(decision.widths[level]),
               static_cast<std::uint32_t>(decision.heights[level]));
  }

  static void ApplyAdjusted(OpenClRenderDevice& device, const Texture& input, Texture& output,
                            Plane reference, Plane adjusted, std::uint32_t width,
                            std::uint32_t height, const LocalToneDecision& decision) {
    EnqueueLlfApply(device, input, output, reference, adjusted, width, height, decision.widths[0],
                    decision.heights[0], decision.apply_uv);
  }

  static void PersistCanonical(OpenClRenderDevice& device, Plane plane, const GraphValueId& id,
                               const LocalToneDecision& decision, int current_long_edge) {
    auto&       workspace    = device.Workspace();
    auto&       invalidation = workspace.ResultInvalidation();
    const ImageExtent extent{decision.mask_extent.width, decision.mask_extent.height};
    auto& image =
        workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R32f});
    workspace.Device().CopyDeviceMemoryToImage(plane.ptr, image.Texture(),
                                               device.CommandContext());
    const auto needed = invalidation.MakeImageRepresentation(
        id, extent, TextureFormat::R32f, static_cast<std::uint32_t>(current_long_edge));
    workspace.Images().RecordUnpublished(id, invalidation.RequiredRevision(id), needed,
                                         device.CommandContext().SubmissionId(),
                                         static_cast<std::uint64_t>(current_long_edge));
  }

  static void PersistCanonicalSource(OpenClRenderDevice& device, Plane plane,
                                     const GraphValueId& source_id,
                                     const LocalToneDecision& decision, int current_long_edge) {
    PersistCanonical(device, plane, source_id, decision, current_long_edge);
  }

  static void PersistCanonicalResult(OpenClRenderDevice& device, Plane plane,
                                     const GraphValueId& result_id,
                                     const LocalToneDecision& decision, int current_long_edge) {
    PersistCanonical(device, plane, result_id, decision, current_long_edge);
  }
};

}  // namespace

auto ExecuteOpenClLocalTone(OpenClRenderDevice& device, const OpenClBackend::Texture2D& input,
                            OpenClBackend::Texture2D& output, const NodeId& grade_id,
                            float shadows_slider, float highlights_slider,
                            const ResolvedRenderGeometry& geometry) -> OpenClLocalToneResult {
  if (input.Native() == nullptr || output.Native() == nullptr) {
    throw std::runtime_error("ExecuteOpenClLocalTone: missing input or output texture");
  }
  if (output.Format() != TextureFormat::Rgba32f || input.Format() != TextureFormat::Rgba32f) {
    throw std::runtime_error("ExecuteOpenClLocalTone: input and output must be matching RGBA32F");
  }
  const auto executed = LocalToneExecutor<OpenClLocalToneOps>::Execute(
      device, input, output, grade_id, shadows_slider, highlights_slider, geometry);
  OpenClLocalToneResult tone;
  tone.reference_resource_id       = executed.reference_resource_id;
  tone.rebuilt_reference           = executed.rebuilt_reference;
  tone.sampled_canonical_reference = executed.sampled_canonical_reference;
  tone.transient_bytes             = executed.transient_bytes;
  return tone;
}


}  // namespace alcedo

#endif  // HAVE_OPENCL
