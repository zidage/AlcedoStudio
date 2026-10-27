//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

#include "cuda_oklab_math.cuh"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/mask_coverage_texture_cache.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

auto EnsureOutput(CudaRenderWorkspace& workspace, const GraphValueId& id, Extent2D extent)
    -> ResourceLease<CudaBackend>& {
  return workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R8});
}

__device__ auto Transform(const float* matrix, float x, float y) -> float2 {
  return make_float2(matrix[0] * x + matrix[1] * y + matrix[2],
                     matrix[3] * x + matrix[4] * y + matrix[5]);
}

__device__ auto FinishEffectiveCoverage(float value, bool invert, float opacity) -> float {
  if (invert) value = 1.0f - value;
  return fminf(fmaxf(value * opacity, 0.0f), 1.0f);
}

/// Invert, then the range weight, then opacity, then clamp (MaskRangeParams).
__device__ auto FinishRangedCoverage(float value, bool invert, float range_weight, float opacity)
    -> float {
  if (invert) value = 1.0f - value;
  return fminf(fmaxf(value * range_weight * opacity, 0.0f), 1.0f);
}

__device__ auto QuantizeR8(float value) -> std::uint8_t {
  return static_cast<std::uint8_t>(fminf(fmaxf(value * 255.0f + 0.5f, 0.0f), 255.0f));
}

__device__ auto SmoothStep(float edge0, float edge1, float x) -> float {
  const float t = fminf(fmaxf((x - edge0) / (edge1 - edge0), 0.0f), 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

/// Product of the Hue Range and Luminance Range weights of the Grade input pixel @p acescc
/// (ACEScc AP1). The equations are documented at MaskRangeParams.
__device__ auto MaskRangeWeight(const MaskRangeParams& range, const float4& acescc) -> float {
  constexpr float kDegreesPerRadian = 57.295779513082321f;
  const float3 lab = LinearAp1ToOkLab(AcesccToLinearAp1(make_float3(acescc.x, acescc.y, acescc.z)));
  float        weight = 1.0f;
  if (range.hue_restricted != 0u) {
    const float chroma = sqrtf(lab.y * lab.y + lab.z * lab.z);
    float       hue    = atan2f(lab.z, lab.y) * kDegreesPerRadian;
    if (hue < 0.0f) hue += 360.0f;
    const float width  = range.hue_start <= range.hue_end ? range.hue_end - range.hue_start
                                                          : 360.0f - range.hue_start + range.hue_end;
    float       offset = hue - range.hue_start;
    if (offset < 0.0f) offset += 360.0f;
    const float distance = offset <= width ? 0.0f : fminf(offset - width, 360.0f - offset);
    weight *= (1.0f - SmoothStep(0.0f, range.hue_softness, distance)) *
              SmoothStep(0.015f, 0.045f, chroma);
  }
  if (range.luminance_restricted != 0u) {
    const float lightness = fminf(fmaxf(lab.x, 0.0f), 1.0f);
    weight *=
        SmoothStep(range.luminance_low - range.luminance_softness, range.luminance_low, lightness) *
        (1.0f - SmoothStep(range.luminance_high, range.luminance_high + range.luminance_softness,
                           lightness));
  }
  return weight;
}

/// Analytic MaskEvaluate. kRanged multiplies the range weights of @p scene (the Grade input,
/// RGBA32F on the render grid); without ranges the kernel does not read @p scene.
template <bool kRanged>
__global__ void AnalyticMaskKernel(std::uint8_t* output, std::uint32_t width, std::uint32_t height,
                                   Matrix3x3 render_to_reference, Extent2D reference_extent,
                                   AnalyticMaskKind kind, RadialMaskParams radial,
                                   LinearGradientMaskParams linear_gradient, MaskRangeParams range,
                                   const float4* scene) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= width * height) return;
  const auto  x         = index % width;
  const auto  y         = index / width;
  const auto  reference = Transform(render_to_reference.m, x + 0.5f, y + 0.5f);
  const float nx        = reference.x / reference_extent.width;
  const float ny        = reference.y / reference_extent.height;
  float       value     = 0.0f;
  bool        invert    = false;
  float       opacity   = 1.0f;
  if (kind == AnalyticMaskKind::Radial) {
    const float c      = cosf(radial.rotation);
    const float s      = sinf(radial.rotation);
    const float dx     = nx - radial.center_x;
    const float dy     = ny - radial.center_y;
    const float rx     = (c * dx + s * dy) / fmaxf(radial.major_radius, 1.0e-6f);
    const float ry     = (-s * dx + c * dy) / fmaxf(radial.minor_radius, 1.0e-6f);
    const float radius = sqrtf(rx * rx + ry * ry);
    const float inner  = fmaxf(0.0f, 1.0f - radial.inner_feather);
    const float outer  = 1.0f + radial.outer_feather;
    value              = 1.0f - fminf(fmaxf((radius - inner) / fmaxf(outer - inner, 1.0e-6f), 0.0f), 1.0f);
    invert             = radial.invert;
    opacity            = radial.opacity;
  } else {
    const float normal_length = hypotf(linear_gradient.normal_x, linear_gradient.normal_y);
    const float normal_x      = linear_gradient.normal_x / fmaxf(normal_length, 1.0e-6f);
    const float normal_y      = linear_gradient.normal_y / fmaxf(normal_length, 1.0e-6f);
    const float distance      = (nx - linear_gradient.origin_x) * normal_x +
                           (ny - linear_gradient.origin_y) * normal_y;
    const float t = fminf(
        fmaxf(distance / fmaxf(linear_gradient.transition_distance, 1.0e-6f) + 0.5f, 0.0f), 1.0f);
    value   = linear_gradient.start_value + (linear_gradient.end_value - linear_gradient.start_value) * t;
    invert  = linear_gradient.invert;
    opacity = linear_gradient.opacity;
  }
  if constexpr (kRanged) {
    output[index] = QuantizeR8(
        FinishRangedCoverage(value, invert, MaskRangeWeight(range, scene[index]), opacity));
  } else {
    output[index] = QuantizeR8(FinishEffectiveCoverage(value, invert, opacity));
  }
}

/// Raster MaskEvaluate parameters (SM6 R7). The matrix maps render pixels to texture UV.
struct RasterMaskParams {
  float         render_to_texture_uv[9];
  std::uint32_t width;
  std::uint32_t height;
  std::uint32_t texture_width;
  std::uint32_t texture_height;
  std::uint32_t samples;
  std::uint32_t invert;
  float         opacity;
};

/// Bilinear read at texel centers with clamp-to-edge indices; 0 outside UV [0, 1].
__device__ auto SampleRasterMask(const std::uint8_t* texture, const RasterMaskParams& params,
                                 float x, float y) -> float {
  const float* m = params.render_to_texture_uv;
  const float  u = m[0] * x + m[1] * y + m[2];
  const float  v = m[3] * x + m[4] * y + m[5];
  if (!(u >= 0.0f && u <= 1.0f && v >= 0.0f && v <= 1.0f)) return 0.0f;
  const float tx    = u * static_cast<float>(params.texture_width) - 0.5f;
  const float ty    = v * static_cast<float>(params.texture_height) - 0.5f;
  const float x0f   = floorf(tx);
  const float y0f   = floorf(ty);
  const float fx    = tx - x0f;
  const float fy    = ty - y0f;
  const int   max_x = static_cast<int>(params.texture_width) - 1;
  const int   max_y = static_cast<int>(params.texture_height) - 1;
  const int   x0    = min(max(static_cast<int>(x0f), 0), max_x);
  const int   x1    = min(max(static_cast<int>(x0f) + 1, 0), max_x);
  const int   y0    = min(max(static_cast<int>(y0f), 0), max_y);
  const int   y1    = min(max(static_cast<int>(y0f) + 1, 0), max_y);
  const auto  at    = [&](int tx_index, int ty_index) -> float {
    return static_cast<float>(texture[ty_index * params.texture_width + tx_index]) / 255.0f;
  };
  const float top    = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
  const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
  return top + (bottom - top) * fy;
}

/// Source coverage of render pixel (@p x, @p y): the mean of samples x samples bilinear reads.
__device__ auto RasterMaskMean(const std::uint8_t* texture, const RasterMaskParams& params,
                               std::uint32_t x, std::uint32_t y) -> float {
  const float samples = static_cast<float>(params.samples);
  float       sum     = 0.0f;
  for (std::uint32_t j = 0; j < params.samples; ++j) {
    for (std::uint32_t i = 0; i < params.samples; ++i) {
      sum += SampleRasterMask(texture, params, static_cast<float>(x) + (i + 0.5f) / samples,
                              static_cast<float>(y) + (j + 0.5f) / samples);
    }
  }
  return sum / (samples * samples);
}

/// Effective R8 coverage of render pixel (@p x, @p y): RasterMaskMean, then invert and opacity
/// (SM6 R7).
__device__ auto RasterMaskCode(const std::uint8_t* texture, const RasterMaskParams& params,
                               std::uint32_t x, std::uint32_t y) -> std::uint8_t {
  return QuantizeR8(FinishEffectiveCoverage(RasterMaskMean(texture, params, x, y),
                                            params.invert != 0u, params.opacity));
}

/// Raster MaskEvaluate. kRanged multiplies the range weights of @p scene, as AnalyticMaskKernel.
template <bool kRanged>
__global__ void RasterMaskKernel(std::uint8_t* output, const std::uint8_t* texture,
                                 RasterMaskParams params, MaskRangeParams range,
                                 const float4* scene) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= params.width * params.height) return;
  const auto x = index % params.width;
  const auto y = index / params.width;
  if constexpr (kRanged) {
    output[index] =
        QuantizeR8(FinishRangedCoverage(RasterMaskMean(texture, params, x, y), params.invert != 0u,
                                        MaskRangeWeight(range, scene[index]), params.opacity));
  } else {
    output[index] = RasterMaskCode(texture, params, x, y);
  }
}

/// Coverage display (SM8.5): the same R8 value as RasterMaskKernel in red; green and blue 0,
/// alpha 1.
__global__ void RasterMaskDisplayKernel(float4* output, const std::uint8_t* texture,
                                        RasterMaskParams params) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= params.width * params.height) return;
  const auto code = RasterMaskCode(texture, params, index % params.width, index / params.width);
  output[index]   = make_float4(static_cast<float>(code) / 255.0f, 0.0f, 0.0f, 1.0f);
}

/// Probed Mask display (Mask ranges D8): the R8 value of each pixel in red, on the same grid;
/// green and blue 0, alpha 1.
__global__ void MaskValueDisplayKernel(float4* output, const std::uint8_t* mask,
                                       std::uint32_t pixel_count) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  output[index] = make_float4(static_cast<float>(mask[index]) / 255.0f, 0.0f, 0.0f, 1.0f);
}

auto MakeRasterMaskParams(const ResolvedRenderGeometry&         geometry,
                          const MaskCoverageImage<CudaBackend>& coverage, bool invert,
                          float opacity) -> RasterMaskParams {
  const auto sampling = MakeRasterMaskSamplingPlan(geometry, coverage.descriptor.reference_bounds,
                                                   coverage.descriptor.extent);
  RasterMaskParams params{};
  for (int i = 0; i < 9; ++i) params.render_to_texture_uv[i] = sampling.render_to_texture_uv.m[i];
  params.width          = geometry.render_extent.width;
  params.height         = geometry.render_extent.height;
  params.texture_width  = coverage.descriptor.extent.width;
  params.texture_height = coverage.descriptor.extent.height;
  params.samples        = RasterMaskSamplesPerAxis(sampling);
  params.invert         = invert ? 1u : 0u;
  params.opacity        = opacity;
  return params;
}

__global__ void MaskFillZeroKernel(std::uint8_t* output, std::uint32_t pixel_count) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  output[index] = 0;
}

__global__ void MaskUnionMaxKernel(const std::uint8_t* lhs, const std::uint8_t* rhs,
                                   std::uint8_t* output, std::uint32_t pixel_count) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  const auto a = lhs[index];
  const auto b = rhs[index];
  output[index] = a > b ? a : b;
}

/// Grade input pixels for a Mask with a restricted range, or null when the Mask has no range.
auto RangeScenePointer(CudaRenderDevice& device, const MaskModel& mask,
                       const FrameSceneBinding& range_scene, Extent2D extent) -> const float4* {
  if (!MaskHasRestrictedRange(mask)) {
    return nullptr;
  }
  const auto& scene = CudaSceneTexture(device, range_scene);
  if (scene.DevicePointer() == nullptr || scene.Format() != TextureFormat::Rgba32f ||
      scene.Width() != extent.width || scene.Height() != extent.height) {
    throw std::runtime_error(
        "ExecuteCudaMask: the Grade input of a Mask range is not an RGBA32F image of the render "
        "extent");
  }
  return static_cast<const float4*>(scene.DevicePointer());
}

}  // namespace

auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                     const CompiledMaskSource& compiled_source,
                     const FrameSceneBinding&  range_scene) -> CudaMaskResult {
  if (!device.Workspace().IsRendering())
    throw std::runtime_error("ExecuteCudaMask: BeginRender has not been called");
  auto&                   workspace     = device.Workspace();
  auto&                   context       = device.CommandContext();
  const auto              extent        = plan.geometry.render_extent;
  auto&                   output        = EnsureOutput(workspace, compiled_source.effective_output, extent);
  constexpr std::uint32_t block         = 256;
  const auto              render_pixels = extent.width * extent.height;
  CudaMaskResult          result{compiled_source.effective_output};

  const auto& mask_model =
      RequireMaskModel(document, compiled_grade.node_id, compiled_source.mask_id);
  const float4* scene  = RangeScenePointer(device, mask_model, range_scene, extent);
  const auto    range  = MaskRangeParamsFromMask(mask_model);
  const auto    blocks = (render_pixels + block - 1) / block;
  auto*         target = static_cast<std::uint8_t*>(output.Texture().DevicePointer());
  if (std::holds_alternative<RadialMaskSource>(mask_model.source) ||
      std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    const auto kernel = scene == nullptr ? AnalyticMaskKernel<false> : AnalyticMaskKernel<true>;
    kernel<<<blocks, block, 0, context.Stream()>>>(
        target, extent.width, extent.height, plan.geometry.render_to_reference,
        plan.geometry.full_reference_extent, AnalyticKindFromMask(mask_model),
        RadialParamsFromMask(mask_model), LinearGradientParamsFromMask(mask_model), range, scene);
  } else if (const auto* ai = std::get_if<AiSegmentationMaskSource>(&mask_model.source)) {
    const auto coverage = workspace.AcquireAiMaskCoverage(*ai, context);
    const auto params =
        MakeRasterMaskParams(plan.geometry, *coverage, mask_model.invert, mask_model.opacity);
    const auto kernel = scene == nullptr ? RasterMaskKernel<false> : RasterMaskKernel<true>;
    kernel<<<blocks, block, 0, context.Stream()>>>(
        target, static_cast<const std::uint8_t*>(coverage->texture.DevicePointer()), params, range,
        scene);
  } else {
    throw std::runtime_error("ExecuteCudaMask: compiled mask does not match document");
  }
  if (::cudaGetLastError() != cudaSuccess)
    throw std::runtime_error("ExecuteCudaMask: CUDA kernel launch failed");
  return result;
}

auto ExecuteCudaMaskUnion(CudaRenderDevice& device, const ExecutionPlan& plan,
                          const PipelineDocument& document,
                          const CompiledGradeNode& compiled_grade) -> CudaMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteCudaMaskUnion: BeginRender has not been called");
  }
  const auto& stack  = RequireMaskStack(compiled_grade);
  auto&       workspace = device.Workspace();
  const auto  extent = plan.geometry.render_extent;
  constexpr std::uint32_t block = 256;
  const auto render_pixels      = extent.width * extent.height;
  std::vector<GraphValueId> enabled;
  enabled.reserve(stack.sources.size());
  for (const auto& source : stack.sources) {
    if (MaskSourceIsEnabled(document, compiled_grade.node_id, source.mask_id)) {
      enabled.push_back(source.effective_output);
    }
  }
  if (enabled.empty()) {
    auto& output = EnsureOutput(workspace, stack.union_output, extent);
    MaskFillZeroKernel<<<(render_pixels + block - 1) / block, block, 0,
                         device.CommandContext().Stream()>>>(
        static_cast<std::uint8_t*>(output.Texture().DevicePointer()), render_pixels);
    if (::cudaGetLastError() != cudaSuccess) {
      throw std::runtime_error("ExecuteCudaMaskUnion: CUDA kernel launch failed");
    }
    return CudaMaskResult{stack.union_output};
  }
  if (enabled.size() == 1) {
    (void)workspace.AliasImageFrom(stack.union_output, enabled.front());
    return CudaMaskResult{stack.union_output};
  }
  auto& output = EnsureOutput(workspace, stack.union_output, extent);
  auto* first  = workspace.Images().Find(enabled.front());
  if (first == nullptr || first->Empty()) {
    throw std::runtime_error("ExecuteCudaMaskUnion: missing enabled Mask source");
  }
  workspace.Device().CopyTexture2D(first->Texture(), output.Texture(), device.CommandContext());
  for (std::size_t index = 1; index < enabled.size(); ++index) {
    auto* next = workspace.Images().Find(enabled[index]);
    if (next == nullptr || next->Empty()) {
      throw std::runtime_error("ExecuteCudaMaskUnion: missing enabled Mask source");
    }
    MaskUnionMaxKernel<<<(render_pixels + block - 1) / block, block, 0,
                         device.CommandContext().Stream()>>>(
        static_cast<const std::uint8_t*>(output.Texture().DevicePointer()),
        static_cast<const std::uint8_t*>(next->Texture().DevicePointer()),
        static_cast<std::uint8_t*>(output.Texture().DevicePointer()), render_pixels);
  }
  if (::cudaGetLastError() != cudaSuccess) {
    throw std::runtime_error("ExecuteCudaMaskUnion: CUDA kernel launch failed");
  }
  return CudaMaskResult{stack.union_output};
}

auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PipelineDocument& document, const CompiledGradeNode& compiled_grade)
    -> CudaMaskResult {
  const auto& stack = RequireMaskStack(compiled_grade);
  CudaMaskResult sources{};
  for (const auto& source : stack.sources) {
    if (!MaskSourceIsEnabled(document, compiled_grade.node_id, source.mask_id)) {
      continue;
    }
    sources = ExecuteCudaMask(device, plan, document, compiled_grade, source,
                              FrameSceneBinding::CachedImage(source.range_input));
  }
  return ExecuteCudaMaskUnion(device, plan, document, compiled_grade);
}

auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PipelineDocument& document) -> CudaMaskResult {
  const CompiledGradeNode* last = nullptr;
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask_stack.has_value()) {
      last = &grade;
    }
  }
  if (last == nullptr) {
    throw std::runtime_error("ExecuteCudaMask: plan has no mask");
  }
  CudaMaskResult result{};
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask_stack.has_value()) {
      result = ExecuteCudaMask(device, plan, document, grade);
    }
  }
  return result;
}

void RecordMaskCoverageDisplay(CudaRenderDevice&                     device,
                               const MaskCoverageImage<CudaBackend>& coverage,
                               const MaskCoverageDisplayRequest&     request,
                               CudaBackend::Texture2D&               output) {
  const auto extent = request.geometry.render_extent;
  if (output.DevicePointer() == nullptr || output.Format() != TextureFormat::Rgba32f ||
      output.Width() != extent.width || output.Height() != extent.height) {
    throw std::runtime_error(
        "RecordMaskCoverageDisplay: the output is not an RGBA32F texture of the photo frame size");
  }
  if (coverage.texture.DevicePointer() == nullptr) {
    throw std::runtime_error("RecordMaskCoverageDisplay: the coverage texture is missing");
  }
  constexpr std::uint32_t block  = 256;
  const auto              pixels = extent.width * extent.height;
  RasterMaskDisplayKernel<<<(pixels + block - 1) / block, block, 0,
                            device.CommandContext().Stream()>>>(
      static_cast<float4*>(output.DevicePointer()),
      static_cast<const std::uint8_t*>(coverage.texture.DevicePointer()),
      MakeRasterMaskParams(request.geometry, coverage, request.invert, request.opacity));
  if (::cudaGetLastError() != cudaSuccess) {
    throw std::runtime_error("RecordMaskCoverageDisplay: CUDA kernel launch failed");
  }
}

void RecordMaskValueDisplay(CudaRenderDevice& device, const CudaBackend::Texture2D& mask,
                            CudaBackend::Texture2D& output) {
  if (mask.DevicePointer() == nullptr || mask.Format() != TextureFormat::R8) {
    throw std::runtime_error("RecordMaskValueDisplay: the Mask coverage is not an R8 texture");
  }
  if (output.DevicePointer() == nullptr || output.Format() != TextureFormat::Rgba32f ||
      output.Width() != mask.Width() || output.Height() != mask.Height()) {
    throw std::runtime_error(
        "RecordMaskValueDisplay: the output is not an RGBA32F texture of the Mask coverage size");
  }
  constexpr std::uint32_t block  = 256;
  const auto              pixels = mask.Width() * mask.Height();
  MaskValueDisplayKernel<<<(pixels + block - 1) / block, block, 0,
                           device.CommandContext().Stream()>>>(
      static_cast<float4*>(output.DevicePointer()),
      static_cast<const std::uint8_t*>(mask.DevicePointer()), pixels);
  if (::cudaGetLastError() != cudaSuccess) {
    throw std::runtime_error("RecordMaskValueDisplay: CUDA kernel launch failed");
  }
}

}  // namespace alcedo
