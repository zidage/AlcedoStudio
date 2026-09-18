//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <variant>
#include <vector>

#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"

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

__device__ auto QuantizeR8(float value) -> std::uint8_t {
  return static_cast<std::uint8_t>(fminf(fmaxf(value * 255.0f + 0.5f, 0.0f), 255.0f));
}

__global__ void AnalyticMaskKernel(std::uint8_t* output, std::uint32_t width, std::uint32_t height,
                                   Matrix3x3 render_to_reference, Extent2D reference_extent,
                                   AnalyticMaskKind kind, RadialMaskParams radial,
                                   LinearGradientMaskParams linear_gradient) {
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
  output[index] = QuantizeR8(FinishEffectiveCoverage(value, invert, opacity));
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

}  // namespace

auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                     const CompiledMaskSource& compiled_source) -> CudaMaskResult {
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
  if (std::holds_alternative<RadialMaskSource>(mask_model.source) ||
      std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    AnalyticMaskKernel<<<(render_pixels + block - 1) / block, block, 0, context.Stream()>>>(
        static_cast<std::uint8_t*>(output.Texture().DevicePointer()), extent.width, extent.height,
        plan.geometry.render_to_reference, plan.geometry.full_reference_extent,
        AnalyticKindFromMask(mask_model), RadialParamsFromMask(mask_model),
        LinearGradientParamsFromMask(mask_model));
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
    sources = ExecuteCudaMask(device, plan, document, compiled_grade, source);
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

}  // namespace alcedo
