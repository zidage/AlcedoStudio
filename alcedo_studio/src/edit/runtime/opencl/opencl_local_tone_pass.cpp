//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_local_tone_pass.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

using local_tone_mapping::LlfSample;

constexpr int kMaxLevels = local_tone_mapping::kMaxLevels;

struct PyramidLayout {
  int                         count = 0;
  std::array<int, kMaxLevels> widths{};
  std::array<int, kMaxLevels> heights{};
};

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

auto MakeLayout(std::uint32_t width, std::uint32_t height) -> PyramidLayout {
  PyramidLayout layout;
  layout.count      = local_tone_mapping::ComputeLevelCount(CheckedInt(width, "pyramid width"),
                                                            CheckedInt(height, "pyramid height"),
                                                            local_tone_mapping::kPyramidRadius);
  layout.widths[0]  = static_cast<int>(width);
  layout.heights[0] = static_cast<int>(height);
  for (int level = 1; level < layout.count; ++level) {
    layout.widths[level]  = std::max(1, (layout.widths[level - 1] + 1) / 2);
    layout.heights[level] = std::max(1, (layout.heights[level - 1] + 1) / 2);
  }
  return layout;
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

auto LevelId(const NodeId& grade_id, const char* family) -> GraphValueId {
  return {grade_id, PortId{std::string{"local_tone."} + family + ".0"}};
}

auto LocalUvPlan(std::uint32_t width, std::uint32_t height) -> Matrix3x3 {
  Matrix3x3 matrix;
  matrix.m[0] = 1.0f / static_cast<float>(width);
  matrix.m[4] = 1.0f / static_cast<float>(height);
  return matrix;
}

void CopyMatrix(float* destination, const Matrix3x3& matrix) {
  for (int index = 0; index < 9; ++index) {
    destination[index] = matrix.m[index];
  }
}

void BuildSourcePyramid(OpenClRenderDevice& device, const std::array<Plane, kMaxLevels>& source,
                        const PyramidLayout& layout) {
  for (int level = 1; level < layout.count; ++level) {
    auto kernel = OpenClKernelCache::Instance().GetKernel(
        OpenCL::GpuDag::kLocalToneProgramName, OpenCL::GpuDag::kLocalTonePyramidDownKernelName);
    PyrDownParams params;
    params.src_width  = layout.widths[level - 1];
    params.src_height = layout.heights[level - 1];
    params.dst_width  = layout.widths[level];
    params.dst_height = layout.heights[level];
    SetMem(kernel, 0, source[level - 1].native, "pyramid source");
    SetMem(kernel, 1, source[level].native, "pyramid destination");
    SetParams(kernel, 2, params, "pyramid parameters");
    SetPlaneOffset(kernel, 3, source[level - 1], "pyramid source offset");
    SetPlaneOffset(kernel, 4, source[level], "pyramid destination offset");
    Dispatch2D(device, kernel, static_cast<std::uint32_t>(layout.widths[level]),
               static_cast<std::uint32_t>(layout.heights[level]));
  }
}

void BuildRemapPyramid(OpenClRenderDevice& device, const Plane& source0,
                       const std::array<Plane, kMaxLevels>& levels, const PyramidLayout& layout,
                       const LlfSample& sample, float sigma) {
  auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kLocalToneProgramName,
                                                        OpenCL::GpuDag::kLocalToneRemapKernelName);
  RemapParams params;
  params.width  = layout.widths[0];
  params.height = layout.heights[0];
  params.gamma  = sample.gamma;
  params.target = sample.target;
  params.beta   = sample.beta;
  params.alpha  = sample.alpha;
  params.sigma  = sigma;
  SetMem(kernel, 0, source0.native, "remap source");
  SetMem(kernel, 1, levels[0].native, "remap destination");
  SetParams(kernel, 2, params, "remap parameters");
  SetPlaneOffset(kernel, 3, source0, "remap source offset");
  SetPlaneOffset(kernel, 4, levels[0], "remap destination offset");
  Dispatch2D(device, kernel, static_cast<std::uint32_t>(layout.widths[0]),
             static_cast<std::uint32_t>(layout.heights[0]));
  BuildSourcePyramid(device, levels, layout);
}

void ApplyAdjusted(OpenClRenderDevice& device, const OpenClBackend::Texture2D& input,
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

auto SourceId(const NodeId& grade_id) -> GraphValueId { return LevelId(grade_id, "source"); }

auto ResultId(const NodeId& grade_id) -> GraphValueId { return LevelId(grade_id, "result"); }

}  // namespace

auto ExecuteOpenClLocalTone(OpenClRenderDevice& device, const OpenClBackend::Texture2D& input,
                            OpenClBackend::Texture2D& output, const NodeId& grade_id,
                            float shadows_slider, float highlights_slider,
                            const ResolvedRenderGeometry& geometry, ContentKey source_key,
                            ContentKey result_key) -> OpenClLocalToneResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteOpenClLocalTone: BeginRender has not been called");
  }
  if (input.Native() == nullptr || output.Native() == nullptr) {
    throw std::runtime_error("ExecuteOpenClLocalTone: missing input or output texture");
  }
  if (geometry.full_reference_extent.Empty() || geometry.render_extent.Empty()) {
    throw std::runtime_error("ExecuteOpenClLocalTone: geometry extents must be positive");
  }
  if (output.Width() != input.Width() || output.Height() != input.Height() ||
      output.Format() != TextureFormat::Rgba32f || input.Format() != TextureFormat::Rgba32f) {
    throw std::runtime_error("ExecuteOpenClLocalTone: input and output must be matching RGBA32F");
  }
  if (source_key.Empty() || result_key.Empty()) {
    throw std::runtime_error("ExecuteOpenClLocalTone: content keys must be non-empty");
  }

  const auto width     = input.Width();
  const auto height    = input.Height();
  const auto canonical = local_tone_mapping::ComputeMaskDimensions(
      CheckedInt(geometry.full_reference_extent.width, "full reference width"),
      CheckedInt(geometry.full_reference_extent.height, "full reference height"),
      local_tone_mapping::kReferenceMaskMaxLongEdge);
  const ImageExtent canonical_extent{static_cast<std::uint32_t>(canonical.width),
                                     static_cast<std::uint32_t>(canonical.height)};
  const bool        full_edit = CoversFullEditSpace(geometry);
  const int         current_long_edge =
      std::max(CheckedInt(width, "input width"), CheckedInt(height, "input height"));
  auto&      images    = workspace.Images();
  const auto source_id = SourceId(grade_id);
  const auto result_id = ResultId(grade_id);
  auto*      cached_source =
      images.BindValidResult(source_id, source_key, canonical_extent, TextureFormat::R32f,
                             workspace.Device().CompletedSubmission());
  const auto source_long_edge = images.PublishedAuxiliary(source_id, source_key);
  const bool source_valid =
      cached_source != nullptr && source_long_edge > 0 &&
      source_long_edge <= static_cast<std::uint64_t>((std::numeric_limits<int>::max)());
  auto* cached_result =
      source_valid
          ? images.BindValidResult(result_id, result_key, canonical_extent, TextureFormat::R32f,
                                   workspace.Device().CompletedSubmission())
          : nullptr;
  const bool result_valid = cached_result != nullptr;
  const bool sample_canonical =
      result_valid && !(full_edit && current_long_edge > static_cast<int>(source_long_edge));

  OpenClLocalToneResult tone;
  const auto            mark = workspace.TransientBuffers().used_bytes();
  if (sample_canonical) {
    const auto source_plane =
        AllocateTransientPlane(workspace, PlaneBytes(canonical.width, canonical.height));
    const auto adjusted_plane =
        AllocateTransientPlane(workspace, PlaneBytes(canonical.width, canonical.height));
    workspace.Device().CopyImageToDeviceMemory(cached_source->Texture(), source_plane.ptr,
                                               source_plane.bytes, device.CommandContext());
    workspace.Device().CopyImageToDeviceMemory(cached_result->Texture(), adjusted_plane.ptr,
                                               adjusted_plane.bytes, device.CommandContext());
    const auto sampling =
        MakeLlfSamplingPlan(geometry, Extent2D{canonical_extent.width, canonical_extent.height});
    ApplyAdjusted(device, input, output, source_plane, adjusted_plane, width, height,
                  canonical.width, canonical.height, sampling.render_to_texture_uv);
    tone.reference_resource_id       = cached_source->Texture().ResourceId();
    tone.sampled_canonical_reference = true;
    tone.transient_bytes =
        static_cast<std::uint32_t>(workspace.TransientBuffers().used_bytes() - mark);
    return tone;
  }

  const bool canonical_frame = full_edit;
  const bool reuse_source =
      source_valid && !(full_edit && current_long_edge > static_cast<int>(source_long_edge));
  const auto mask_dims =
      canonical_frame || reuse_source
          ? canonical
          : local_tone_mapping::ComputeMaskDimensions(
                CheckedInt(width, "input width"), CheckedInt(height, "input height"),
                local_tone_mapping::kReferenceMaskMaxLongEdge);
  const auto                    layout = MakeLayout(static_cast<std::uint32_t>(mask_dims.width),
                                                    static_cast<std::uint32_t>(mask_dims.height));

  std::array<Plane, kMaxLevels> source{};
  std::array<Plane, kMaxLevels> remap_a{};
  std::array<Plane, kMaxLevels> remap_b{};
  std::array<Plane, kMaxLevels> result{};
  for (int level = 0; level < layout.count; ++level) {
    const auto bytes = PlaneBytes(layout.widths[level], layout.heights[level]);
    source[level]    = AllocateTransientPlane(workspace, bytes);
    remap_a[level]   = AllocateTransientPlane(workspace, bytes);
    remap_b[level]   = AllocateTransientPlane(workspace, bytes);
    result[level]    = AllocateTransientPlane(workspace, bytes);
  }
  tone.transient_bytes =
      static_cast<std::uint32_t>(workspace.TransientBuffers().used_bytes() - mark);

  if (reuse_source) {
    if (cached_source == nullptr) {
      throw std::runtime_error("ExecuteOpenClLocalTone: canonical source disappeared");
    }
    workspace.Device().CopyImageToDeviceMemory(cached_source->Texture(), source[0].ptr,
                                               source[0].bytes, device.CommandContext());
  } else {
    if (canonical_frame) {
      auto kernel = OpenClKernelCache::Instance().GetKernel(
          OpenCL::GpuDag::kLocalToneProgramName,
          OpenCL::GpuDag::kLocalToneExtractReferenceKernelName);
      ExtractReferenceParams params;
      params.input_width   = CheckedInt(width, "input width");
      params.input_height  = CheckedInt(height, "input height");
      params.output_width  = layout.widths[0];
      params.output_height = layout.heights[0];
      params.full_ref_w    = static_cast<float>(geometry.full_reference_extent.width);
      params.full_ref_h    = static_cast<float>(geometry.full_reference_extent.height);
      CopyMatrix(params.reference_to_render, geometry.reference_to_render);
      SetMem(kernel, 0, input.Native(), "reference extract input");
      SetMem(kernel, 1, source[0].native, "reference extract output");
      SetParams(kernel, 2, params, "reference extract parameters");
      SetPlaneOffset(kernel, 3, source[0], "reference extract output offset");
      Dispatch2D(device, kernel, static_cast<std::uint32_t>(layout.widths[0]),
                 static_cast<std::uint32_t>(layout.heights[0]));
    } else {
      auto kernel = OpenClKernelCache::Instance().GetKernel(
          OpenCL::GpuDag::kLocalToneProgramName, OpenCL::GpuDag::kLocalToneExtractKernelName);
      ExtractParams params;
      params.input_width   = CheckedInt(width, "input width");
      params.input_height  = CheckedInt(height, "input height");
      params.output_width  = layout.widths[0];
      params.output_height = layout.heights[0];
      SetMem(kernel, 0, input.Native(), "local extract input");
      SetMem(kernel, 1, source[0].native, "local extract output");
      SetParams(kernel, 2, params, "local extract parameters");
      SetPlaneOffset(kernel, 3, source[0], "local extract output offset");
      Dispatch2D(device, kernel, static_cast<std::uint32_t>(layout.widths[0]),
                 static_cast<std::uint32_t>(layout.heights[0]));
    }
  }
  BuildSourcePyramid(device, source, layout);

  const float shadow_amount =
      std::clamp(shadows_slider * local_tone_mapping::kHighlightStrengthScale / 80.0f,
                 -local_tone_mapping::kBackendAmountLimit, local_tone_mapping::kBackendAmountLimit);
  const float highlight_amount =
      std::clamp(-highlights_slider * local_tone_mapping::kHighlightStrengthScale / 100.0f,
                 -local_tone_mapping::kBackendAmountLimit, local_tone_mapping::kBackendAmountLimit);
  const float sigma   = local_tone_mapping::SigmaR(shadow_amount, highlight_amount);
  const auto  samples = local_tone_mapping::BuildSamples(shadow_amount, highlight_amount);
  for (int level = 0; level < layout.count; ++level) {
    workspace.Device().FillDeviceMemory(result[level].ptr, result[level].bytes, 0,
                                        device.CommandContext());
  }
  BuildRemapPyramid(device, source[0], remap_a, layout, samples[0], sigma);
  BuildRemapPyramid(device, source[0], remap_b, layout, samples[1], sigma);

  for (std::size_t pair = 0; pair + 1 < samples.size(); ++pair) {
    for (int level = 0; level < layout.count; ++level) {
      const bool top    = level + 1 == layout.count;
      auto       kernel = OpenClKernelCache::Instance().GetKernel(
          OpenCL::GpuDag::kLocalToneProgramName, OpenCL::GpuDag::kLocalToneSelectKernelName);
      SelectParams params;
      params.width         = layout.widths[level];
      params.height        = layout.heights[level];
      params.coarse_width  = top ? 1 : layout.widths[level + 1];
      params.coarse_height = top ? 1 : layout.heights[level + 1];
      params.gamma_lo      = samples[pair].gamma;
      params.gamma_hi      = samples[pair + 1].gamma;
      params.first         = pair == 0 ? 1 : 0;
      params.last          = pair + 2 == samples.size() ? 1 : 0;
      params.top           = top ? 1 : 0;
      SetMem(kernel, 0, source[level].native, "select source");
      SetMem(kernel, 1, remap_a[level].native, "select low");
      SetMem(kernel, 2, (top ? remap_a[level] : remap_a[level + 1]).native, "select low coarse");
      SetMem(kernel, 3, remap_b[level].native, "select high");
      SetMem(kernel, 4, (top ? remap_b[level] : remap_b[level + 1]).native, "select high coarse");
      SetMem(kernel, 5, result[level].native, "select output");
      SetParams(kernel, 6, params, "select parameters");
      SetPlaneOffset(kernel, 7, source[level], "select source offset");
      SetPlaneOffset(kernel, 8, remap_a[level], "select low offset");
      SetPlaneOffset(kernel, 9, top ? remap_a[level] : remap_a[level + 1],
                     "select low coarse offset");
      SetPlaneOffset(kernel, 10, remap_b[level], "select high offset");
      SetPlaneOffset(kernel, 11, top ? remap_b[level] : remap_b[level + 1],
                     "select high coarse offset");
      SetPlaneOffset(kernel, 12, result[level], "select output offset");
      Dispatch2D(device, kernel, static_cast<std::uint32_t>(layout.widths[level]),
                 static_cast<std::uint32_t>(layout.heights[level]));
    }
    if (pair + 2 < samples.size()) {
      std::swap(remap_a, remap_b);
      BuildRemapPyramid(device, source[0], remap_b, layout, samples[pair + 2], sigma);
    }
  }

  for (int level = layout.count - 2; level >= 0; --level) {
    auto kernel = OpenClKernelCache::Instance().GetKernel(
        OpenCL::GpuDag::kLocalToneProgramName, OpenCL::GpuDag::kLocalToneCollapseKernelName);
    CollapseParams params;
    params.width         = layout.widths[level];
    params.height        = layout.heights[level];
    params.coarse_width  = layout.widths[level + 1];
    params.coarse_height = layout.heights[level + 1];
    SetMem(kernel, 0, result[level].native, "collapse laplacian");
    SetMem(kernel, 1, result[level + 1].native, "collapse coarse");
    SetMem(kernel, 2, remap_a[level].native, "collapse output");
    SetParams(kernel, 3, params, "collapse parameters");
    SetPlaneOffset(kernel, 4, result[level], "collapse laplacian offset");
    SetPlaneOffset(kernel, 5, result[level + 1], "collapse coarse offset");
    SetPlaneOffset(kernel, 6, remap_a[level], "collapse output offset");
    Dispatch2D(device, kernel, static_cast<std::uint32_t>(layout.widths[level]),
               static_cast<std::uint32_t>(layout.heights[level]));
    std::swap(result[level], remap_a[level]);
  }

  const Matrix3x3 apply_uv =
      canonical_frame || reuse_source
          ? MakeLlfSamplingPlan(geometry, Extent2D{static_cast<std::uint32_t>(layout.widths[0]),
                                                   static_cast<std::uint32_t>(layout.heights[0])})
                .render_to_texture_uv
          : LocalUvPlan(width, height);
  ApplyAdjusted(device, input, output, source[0], result[0], width, height, layout.widths[0],
                layout.heights[0], apply_uv);

  if (canonical_frame || reuse_source) {
    const ImageExtent extent{static_cast<std::uint32_t>(layout.widths[0]),
                             static_cast<std::uint32_t>(layout.heights[0])};
    if (!reuse_source) {
      (void)workspace.AcquireImageForWrite(source_id,
                                           {extent.width, extent.height, TextureFormat::R32f});
      cached_source = images.Find(source_id);
      if (cached_source == nullptr) {
        throw std::runtime_error("ExecuteOpenClLocalTone: canonical source allocation failed");
      }
    }
    (void)workspace.AcquireImageForWrite(result_id,
                                         {extent.width, extent.height, TextureFormat::R32f});
    cached_result = images.Find(result_id);
    if (cached_source == nullptr || cached_result == nullptr) {
      throw std::runtime_error("ExecuteOpenClLocalTone: canonical result allocation failed");
    }
    if (!reuse_source) {
      workspace.Device().CopyDeviceMemoryToImage(source[0].ptr, cached_source->Texture(),
                                                 device.CommandContext());
      images.RecordUnpublished(source_id, source_key, extent, TextureFormat::R32f,
                               device.CommandContext().SubmissionId(),
                               static_cast<std::uint64_t>(current_long_edge));
    }
    workspace.Device().CopyDeviceMemoryToImage(result[0].ptr, cached_result->Texture(),
                                               device.CommandContext());
    images.RecordUnpublished(result_id, result_key, extent, TextureFormat::R32f,
                             device.CommandContext().SubmissionId(),
                             static_cast<std::uint64_t>(current_long_edge));
    tone.reference_resource_id = cached_source->Texture().ResourceId();
  }
  tone.rebuilt_reference = true;
  return tone;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
