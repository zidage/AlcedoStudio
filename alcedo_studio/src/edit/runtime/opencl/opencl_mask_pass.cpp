//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_mask_pass.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

struct MaskAnalyticParams {
  float         render_to_reference[9]{};
  std::uint32_t width               = 0;
  std::uint32_t height              = 0;
  std::uint32_t reference_width     = 0;
  std::uint32_t reference_height    = 0;
  std::uint32_t kind                = 0;
  float         center_x            = 0.0f;
  float         center_y            = 0.0f;
  float         major_radius        = 0.0f;
  float         minor_radius        = 0.0f;
  float         rotation            = 0.0f;
  float         inner_feather       = 0.0f;
  float         outer_feather       = 0.0f;
  std::uint32_t radial_invert       = 0;
  float         origin_x            = 0.0f;
  float         origin_y            = 0.0f;
  float         normal_x            = 0.0f;
  float         normal_y            = 0.0f;
  float         transition_distance = 0.0f;
  float         start_value         = 0.0f;
  float         end_value           = 0.0f;
  float         opacity             = 1.0f;
};

static_assert(sizeof(MaskAnalyticParams) == 120);

auto EnsureOutput(OpenClRenderWorkspace& workspace, const GraphValueId& id, Extent2D extent)
    -> ResourceLease<OpenClBackend>& {
  return workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R8});
}

void SetMem(cl_kernel kernel, cl_uint index, cl_mem value, const char* label) {
  if (value == nullptr) {
    throw std::runtime_error(std::string{"ExecuteOpenClMask: missing "} + label);
  }
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(cl_mem), &value), label);
}

template <typename Params>
void SetParams(cl_kernel kernel, cl_uint index, const Params& params, const char* label) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(params), &params), label);
}

void SetUInt(cl_kernel kernel, cl_uint index, cl_uint value, const char* label) {
  CheckOpenCl(clSetKernelArg(kernel, index, sizeof(value), &value), label);
}

void SetMatrix(float* destination, const Matrix3x3& source) {
  for (int index = 0; index < 9; ++index) {
    destination[index] = source.m[index];
  }
}

void Dispatch2D(OpenClRenderDevice& device, cl_kernel kernel, std::uint32_t width,
                std::uint32_t height, const char* label) {
  if (width == 0 || height == 0) {
    throw std::runtime_error(std::string{"ExecuteOpenClMask: empty "} + label + " dispatch");
  }
  const std::size_t local[2]  = {16, 16};
  const std::size_t global[2] = {((static_cast<std::size_t>(width) + 15) / 16) * 16,
                                 ((static_cast<std::size_t>(height) + 15) / 16) * 16};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 2, nullptr,
                                     global, local, 0, nullptr, &event),
              label);
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

void EncodeAnalytic(OpenClRenderDevice& device, OpenClBackend::Texture2D& output,
                    const MaskModel& mask, const ExecutionPlan& plan) {
  const auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kMaskProgramName, OpenCL::GpuDag::kMaskAnalyticKernelName);
  MaskAnalyticParams params;
  SetMatrix(params.render_to_reference, plan.geometry.render_to_reference);
  params.width               = output.Width();
  params.height              = output.Height();
  params.reference_width     = plan.geometry.full_reference_extent.width;
  params.reference_height    = plan.geometry.full_reference_extent.height;
  params.kind                = AnalyticKindFromMask(mask) == AnalyticMaskKind::Radial ? 0u : 1u;

  const auto radial          = RadialParamsFromMask(mask);
  params.center_x            = radial.center_x;
  params.center_y            = radial.center_y;
  params.major_radius        = radial.major_radius;
  params.minor_radius        = radial.minor_radius;
  params.rotation            = radial.rotation;
  params.inner_feather       = radial.inner_feather;
  params.outer_feather       = radial.outer_feather;
  params.radial_invert       = radial.invert ? 1u : 0u;

  const auto linear_gradient       = LinearGradientParamsFromMask(mask);
  params.origin_x            = linear_gradient.origin_x;
  params.origin_y            = linear_gradient.origin_y;
  params.normal_x            = linear_gradient.normal_x;
  params.normal_y            = linear_gradient.normal_y;
  params.transition_distance = linear_gradient.transition_distance;
  params.start_value         = linear_gradient.start_value;
  params.end_value           = linear_gradient.end_value;
  params.opacity             = mask.opacity;

  SetMem(kernel, 0, output.Native(), "analytic mask output");
  SetParams(kernel, 1, params, "analytic mask parameters");
  Dispatch2D(device, kernel, output.Width(), output.Height(), "analytic mask");
}

void EncodeFillZero(OpenClRenderDevice& device, OpenClBackend::Texture2D& output) {
  const auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kMaskProgramName,
                                                              OpenCL::GpuDag::kMaskFillZeroKernelName);
  SetMem(kernel, 0, output.Native(), "fill zero output");
  SetUInt(kernel, 1, output.Width(), "fill zero width");
  SetUInt(kernel, 2, output.Height(), "fill zero height");
  Dispatch2D(device, kernel, output.Width(), output.Height(), "mask fill zero");
}

void EncodeUnionMax(OpenClRenderDevice& device, const OpenClBackend::Texture2D& lhs,
                    const OpenClBackend::Texture2D& rhs, OpenClBackend::Texture2D& output) {
  const auto kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kMaskProgramName,
                                                              OpenCL::GpuDag::kMaskUnionMaxKernelName);
  SetMem(kernel, 0, lhs.Native(), "union lhs");
  SetMem(kernel, 1, rhs.Native(), "union rhs");
  SetMem(kernel, 2, output.Native(), "union output");
  SetUInt(kernel, 3, output.Width(), "union width");
  SetUInt(kernel, 4, output.Height(), "union height");
  Dispatch2D(device, kernel, output.Width(), output.Height(), "mask union max");
}

}  // namespace

auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                       const CompiledMaskSource& compiled_source) -> OpenClMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteOpenClMask: BeginRender has not been called");
  }
  const auto extent = plan.geometry.render_extent;
  if (extent.Empty() || plan.geometry.full_reference_extent.Empty()) {
    throw std::runtime_error("ExecuteOpenClMask: geometry extents must be positive");
  }

  auto&            workspace = device.Workspace();
  auto&            output    = EnsureOutput(workspace, compiled_source.effective_output, extent);
  OpenClMaskResult result{compiled_source.effective_output};

  const auto& mask_model =
      RequireMaskModel(document, compiled_grade.node_id, compiled_source.mask_id);
  if (std::holds_alternative<RadialMaskSource>(mask_model.source) ||
      std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    EncodeAnalytic(device, output.Texture(), mask_model, plan);
    return result;
  }
  throw std::runtime_error("ExecuteOpenClMask: compiled mask does not match document");
}

auto ExecuteOpenClMaskUnion(OpenClRenderDevice& device, const ExecutionPlan& plan,
                            const PipelineDocument& document,
                            const CompiledGradeNode& compiled_grade) -> OpenClMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteOpenClMaskUnion: BeginRender has not been called");
  }
  const auto& stack     = RequireMaskStack(compiled_grade);
  auto&       workspace = device.Workspace();
  const auto  extent    = plan.geometry.render_extent;
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
    return OpenClMaskResult{stack.union_output};
  }
  if (enabled.size() == 1) {
    (void)workspace.AliasImageFrom(stack.union_output, enabled.front());
    return OpenClMaskResult{stack.union_output};
  }
  auto& output = EnsureOutput(workspace, stack.union_output, extent);
  auto* first  = workspace.Images().Find(enabled.front());
  if (first == nullptr || first->Empty()) {
    throw std::runtime_error("ExecuteOpenClMaskUnion: missing enabled Mask source");
  }
  workspace.Device().CopyTexture2D(first->Texture(), output.Texture(), device.CommandContext());
  const GraphValueId scratch_id{compiled_grade.node_id, PortId{"mask.union.scratch"}};
  auto& scratch = EnsureOutput(workspace, scratch_id, extent);
  for (std::size_t index = 1; index < enabled.size(); ++index) {
    auto* next = workspace.Images().Find(enabled[index]);
    auto* cur  = workspace.Images().Find(stack.union_output);
    if (next == nullptr || cur == nullptr) {
      throw std::runtime_error("ExecuteOpenClMaskUnion: missing enabled Mask source");
    }
    EncodeUnionMax(device, cur->Texture(), next->Texture(), scratch.Texture());
    auto* scratch_image = workspace.Images().Find(scratch_id);
    auto* union_image   = workspace.Images().Find(stack.union_output);
    if (scratch_image == nullptr || union_image == nullptr) {
      throw std::runtime_error("ExecuteOpenClMaskUnion: Union scratch is missing");
    }
    workspace.Device().CopyTexture2D(scratch_image->Texture(), union_image->Texture(),
                                     device.CommandContext());
  }
  return OpenClMaskResult{stack.union_output};
}

auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document, const CompiledGradeNode& compiled_grade)
    -> OpenClMaskResult {
  const auto& stack = RequireMaskStack(compiled_grade);
  for (const auto& source : stack.sources) {
    if (!MaskSourceIsEnabled(document, compiled_grade.node_id, source.mask_id)) {
      continue;
    }
    (void)ExecuteOpenClMask(device, plan, document, compiled_grade, source);
  }
  return ExecuteOpenClMaskUnion(device, plan, document, compiled_grade);
}

auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document) -> OpenClMaskResult {
  const CompiledGradeNode* last = nullptr;
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask_stack.has_value()) {
      last = &grade;
    }
  }
  if (last == nullptr) {
    throw std::runtime_error("ExecuteOpenClMask: plan has no mask");
  }
  OpenClMaskResult result{};
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask_stack.has_value()) {
      result = ExecuteOpenClMask(device, plan, document, grade);
    }
  }
  return result;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
