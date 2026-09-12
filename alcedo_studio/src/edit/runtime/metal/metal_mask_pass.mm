//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_mask_pass.hpp"

#include <algorithm>
#include <stdexcept>
#include <variant>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

struct alignas(16) MaskAnalyticParams {
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

auto Pipeline(const char* function, const char* label) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_MASK_METALLIB_PATH
  throw std::runtime_error("Metal mask metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(ALCEDO_METAL_MASK_METALLIB_PATH,
                                                                  function, label);
#endif
}

auto Encoder(MetalRenderDevice& device) -> MTL::ComputeCommandEncoder* {
  auto* encoder = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalMask: compute encoder is missing");
  }
  return encoder;
}

void Dispatch2D(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                std::uint32_t width, std::uint32_t height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(MTL::Size{width, height, 1}, MTL::Size{thread_width, thread_height, 1});
}

void CopyMatrix(float* dst, const Matrix3x3& matrix) {
  for (int i = 0; i < 9; ++i) {
    dst[i] = matrix.m[i];
  }
}

auto EnsureOutput(MetalRenderWorkspace& workspace, const GraphValueId& id, Extent2D extent)
    -> ResourceLease<MetalBackend>& {
  return workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R8});
}

void EncodeAnalytic(MetalRenderDevice& device, MetalBackend::Texture2D& output,
                    const MaskModel& mask, const ExecutionPlan& plan) {
  auto*              encoder  = Encoder(device);
  auto               pipeline = Pipeline("mask_analytic", "Metal Mask analytic");
  MaskAnalyticParams params;
  CopyMatrix(params.render_to_reference, plan.geometry.render_to_reference);
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
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 0);
  encoder->setBytes(&params, sizeof(params), 0);
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

void EncodeFillZero(MetalRenderDevice& device, MetalBackend::Texture2D& output) {
  auto* encoder  = Encoder(device);
  auto  pipeline = Pipeline("mask_fill_zero", "Metal Mask fill zero");
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 0);
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

void EncodeUnionMax(MetalRenderDevice& device, const MetalBackend::Texture2D& lhs,
                    const MetalBackend::Texture2D& rhs, MetalBackend::Texture2D& output) {
  auto* encoder  = Encoder(device);
  auto  pipeline = Pipeline("mask_union_max", "Metal Mask union max");
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(lhs.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(rhs.Native()), 1);
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 2);
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

}  // namespace

void AppendMetalMaskWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_MASK_METALLIB_PATH
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_analytic", "Metal Mask analytic"});
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_fill_zero", "Metal Mask fill zero"});
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_union_max", "Metal Mask union max"});
#else
  (void)pipelines;
#endif
}

auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                      const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                      const CompiledMaskSource& compiled_source) -> MetalMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteMetalMask: BeginRender has not been called");
  }
  auto&           workspace = device.Workspace();
  const auto      extent    = plan.geometry.render_extent;
  auto&           output    = EnsureOutput(workspace, compiled_source.effective_output, extent);
  MetalMaskResult result{compiled_source.effective_output};

  const auto& mask_model =
      RequireMaskModel(document, compiled_grade.node_id, compiled_source.mask_id);
  if (std::holds_alternative<RadialMaskSource>(mask_model.source) ||
      std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    EncodeAnalytic(device, output.Texture(), mask_model, plan);
    return result;
  }
  throw std::runtime_error("ExecuteMetalMask: compiled mask does not match document");
}

auto ExecuteMetalMaskUnion(MetalRenderDevice& device, const ExecutionPlan& plan,
                           const PipelineDocument& document,
                           const CompiledGradeNode& compiled_grade) -> MetalMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteMetalMaskUnion: BeginRender has not been called");
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
    return MetalMaskResult{stack.union_output};
  }
  if (enabled.size() == 1) {
    (void)workspace.AliasImageFrom(stack.union_output, enabled.front());
    return MetalMaskResult{stack.union_output};
  }
  auto& output = EnsureOutput(workspace, stack.union_output, extent);
  auto* first  = workspace.Images().Find(enabled.front());
  if (first == nullptr || first->Empty()) {
    throw std::runtime_error("ExecuteMetalMaskUnion: missing enabled Mask source");
  }
  workspace.Device().CopyTexture2D(first->Texture(), output.Texture(), device.CommandContext());
  const GraphValueId scratch_id{compiled_grade.node_id, PortId{"mask.union.scratch"}};
  auto& scratch = EnsureOutput(workspace, scratch_id, extent);
  for (std::size_t index = 1; index < enabled.size(); ++index) {
    auto* next = workspace.Images().Find(enabled[index]);
    auto* cur  = workspace.Images().Find(stack.union_output);
    if (next == nullptr || cur == nullptr) {
      throw std::runtime_error("ExecuteMetalMaskUnion: missing enabled Mask source");
    }
    EncodeUnionMax(device, cur->Texture(), next->Texture(), scratch.Texture());
    auto* scratch_image = workspace.Images().Find(scratch_id);
    auto* union_image   = workspace.Images().Find(stack.union_output);
    if (scratch_image == nullptr || union_image == nullptr) {
      throw std::runtime_error("ExecuteMetalMaskUnion: Union scratch is missing");
    }
    workspace.Device().CopyTexture2D(scratch_image->Texture(), union_image->Texture(),
                                     device.CommandContext());
  }
  return MetalMaskResult{stack.union_output};
}

auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                      const PipelineDocument& document, const CompiledGradeNode& compiled_grade)
    -> MetalMaskResult {
  const auto& stack = RequireMaskStack(compiled_grade);
  for (const auto& source : stack.sources) {
    if (!MaskSourceIsEnabled(document, compiled_grade.node_id, source.mask_id)) {
      continue;
    }
    (void)ExecuteMetalMask(device, plan, document, compiled_grade, source);
  }
  return ExecuteMetalMaskUnion(device, plan, document, compiled_grade);
}

auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                      const PipelineDocument& document) -> MetalMaskResult {
  const CompiledGradeNode* last = nullptr;
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask_stack.has_value()) {
      last = &grade;
    }
  }
  if (last == nullptr) {
    throw std::runtime_error("ExecuteMetalMask: plan has no mask");
  }
  MetalMaskResult result{};
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask_stack.has_value()) {
      result = ExecuteMetalMask(device, plan, document, grade);
    }
  }
  return result;
}

}  // namespace alcedo
