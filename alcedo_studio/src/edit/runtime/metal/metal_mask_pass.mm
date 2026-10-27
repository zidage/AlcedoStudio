//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_mask_pass.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "edit/runtime/mask_coverage_texture_cache.hpp"
#include "edit/runtime/metal/metal_scene_work.hpp"
#include "edit/runtime/texture_format.hpp"
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

/// Layout of `MaskRasterParams` in mask.metal (SM6 R7).
struct MaskRasterParams {
  float         render_to_texture_uv[9]{};
  std::uint32_t width          = 0;
  std::uint32_t height         = 0;
  std::uint32_t texture_width  = 0;
  std::uint32_t texture_height = 0;
  std::uint32_t samples        = 1;
  std::uint32_t invert         = 0;
  float         opacity        = 1.0f;
};

static_assert(sizeof(MaskRasterParams) == 64);
// MaskRangeParams (mask_model.hpp) is passed as is: mask.metal declares the same eight 4-byte
// fields.
static_assert(sizeof(MaskRangeParams) == 32);

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

/// Grade input of a Mask with a restricted range, or null when the Mask has no range.
auto RangeSceneTexture(MetalRenderDevice& device, const MaskModel& mask,
                       const FrameSceneBinding& range_scene, Extent2D extent)
    -> const MetalBackend::Texture2D* {
  if (!MaskHasRestrictedRange(mask)) {
    return nullptr;
  }
  const auto& scene = MetalSceneTexture(device, range_scene);
  if (scene.Native() == nullptr || scene.Format() != TextureFormat::Rgba32f ||
      scene.Width() != extent.width || scene.Height() != extent.height) {
    throw std::runtime_error(
        "ExecuteMetalMask: the Grade input of a Mask range is not an RGBA32F texture of the "
        "render extent");
  }
  return &scene;
}

void EncodeAnalytic(MetalRenderDevice& device, MetalBackend::Texture2D& output,
                    const MaskModel& mask, const ExecutionPlan& plan,
                    const MetalBackend::Texture2D* range_scene) {
  auto*              encoder  = Encoder(device);
  auto               pipeline = range_scene == nullptr
                                    ? Pipeline("mask_analytic", "Metal Mask analytic")
                                    : Pipeline("mask_analytic_ranged", "Metal Mask analytic ranged");
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
  if (range_scene != nullptr) {
    const auto range = MaskRangeParamsFromMask(mask);
    encoder->setTexture(static_cast<MTL::Texture*>(range_scene->Native()), 1);
    encoder->setBytes(&range, sizeof(range), 1);
  }
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

/// Raster sampling of @p coverage on the render grid of @p geometry into @p output: R8 for
/// MaskEvaluate, RGBA32F for the coverage display. mask_raster writes (value, 0, 0, 1).
/// @p range_scene and @p range_mask are set only for a MaskEvaluate with a restricted range.
/// @pre A coverage upload, if any, was recorded before (its blit precedes the compute encoder).
void EncodeRasterSampling(MetalRenderDevice&                     device,
                          const MaskCoverageImage<MetalBackend>& coverage,
                          const ResolvedRenderGeometry& geometry, bool invert, float opacity,
                          MetalBackend::Texture2D&       output,
                          const MetalBackend::Texture2D* range_scene = nullptr,
                          const MaskModel*               range_mask  = nullptr) {
  const auto sampling = MakeRasterMaskSamplingPlan(geometry, coverage.descriptor.reference_bounds,
                                                   coverage.descriptor.extent);
  auto*            encoder  = Encoder(device);
  auto             pipeline = range_scene == nullptr
                                  ? Pipeline("mask_raster", "Metal Mask raster")
                                  : Pipeline("mask_raster_ranged", "Metal Mask raster ranged");
  MaskRasterParams params;
  CopyMatrix(params.render_to_texture_uv, sampling.render_to_texture_uv);
  params.width          = output.Width();
  params.height         = output.Height();
  params.texture_width  = coverage.descriptor.extent.width;
  params.texture_height = coverage.descriptor.extent.height;
  params.samples        = RasterMaskSamplesPerAxis(sampling);
  params.invert         = invert ? 1u : 0u;
  params.opacity        = opacity;
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(coverage.texture.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 1);
  encoder->setBytes(&params, sizeof(params), 0);
  if (range_scene != nullptr) {
    const auto range = MaskRangeParamsFromMask(*range_mask);
    encoder->setTexture(static_cast<MTL::Texture*>(range_scene->Native()), 2);
    encoder->setBytes(&range, sizeof(range), 1);
  }
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

void EncodeRaster(MetalRenderDevice& device, MetalBackend::Texture2D& output,
                  const AiSegmentationMaskSource& source, const MaskModel& mask,
                  const ExecutionPlan& plan, const MetalBackend::Texture2D* range_scene) {
  const auto coverage = device.Workspace().AcquireAiMaskCoverage(source, device.CommandContext());
  EncodeRasterSampling(device, *coverage, plan.geometry, mask.invert, mask.opacity, output,
                       range_scene, &mask);
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
      MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_raster", "Metal Mask raster"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_analytic_ranged",
                                          "Metal Mask analytic ranged"});
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_fill_zero", "Metal Mask fill zero"});
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_union_max", "Metal Mask union max"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_value_display",
                                          "Metal Mask value display"});
  // SM8.4: AI Mask Feather and Expand (metal_ai_mask_coverage.mm).
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "ai_mask_morphology",
                                          "Metal AI Mask morphology"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "ai_mask_feather_rows",
                                          "Metal AI Mask Feather rows"});
  pipelines.push_back(MetalPipelineWarmup{
      ALCEDO_METAL_MASK_METALLIB_PATH, "ai_mask_feather_columns", "Metal AI Mask Feather columns"});
#else
  (void)pipelines;
#endif
}

auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                      const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                      const CompiledMaskSource& compiled_source,
                      const FrameSceneBinding&  range_scene) -> MetalMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteMetalMask: BeginRender has not been called");
  }
  auto&           workspace = device.Workspace();
  const auto      extent    = plan.geometry.render_extent;
  auto&           output    = EnsureOutput(workspace, compiled_source.effective_output, extent);
  MetalMaskResult result{compiled_source.effective_output};

  const auto& mask_model =
      RequireMaskModel(document, compiled_grade.node_id, compiled_source.mask_id);
  const auto* scene = RangeSceneTexture(device, mask_model, range_scene, extent);
  if (std::holds_alternative<RadialMaskSource>(mask_model.source) ||
      std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    EncodeAnalytic(device, output.Texture(), mask_model, plan, scene);
    return result;
  }
  if (const auto* ai = std::get_if<AiSegmentationMaskSource>(&mask_model.source)) {
    EncodeRaster(device, output.Texture(), *ai, mask_model, plan, scene);
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
    (void)ExecuteMetalMask(device, plan, document, compiled_grade, source,
                           FrameSceneBinding::CachedImage(source.range_input));
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

void RecordMaskCoverageDisplay(MetalRenderDevice&                     device,
                               const MaskCoverageImage<MetalBackend>& coverage,
                               const MaskCoverageDisplayRequest&      request,
                               MetalBackend::Texture2D&               output) {
  const auto extent = request.geometry.render_extent;
  if (output.Native() == nullptr || output.Format() != TextureFormat::Rgba32f ||
      output.Width() != extent.width || output.Height() != extent.height) {
    throw std::runtime_error(
        "RecordMaskCoverageDisplay: the output is not an RGBA32F texture of the photo frame size");
  }
  EncodeRasterSampling(device, coverage, request.geometry, request.invert, request.opacity, output);
}

void RecordMaskValueDisplay(MetalRenderDevice& device, const MetalBackend::Texture2D& mask,
                            MetalBackend::Texture2D& output) {
  if (mask.Native() == nullptr || mask.Format() != TextureFormat::R8) {
    throw std::runtime_error("RecordMaskValueDisplay: the Mask coverage is not an R8 texture");
  }
  if (output.Native() == nullptr || output.Format() != TextureFormat::Rgba32f ||
      output.Width() != mask.Width() || output.Height() != mask.Height()) {
    throw std::runtime_error(
        "RecordMaskValueDisplay: the output is not an RGBA32F texture of the Mask coverage size");
  }
  auto* encoder  = Encoder(device);
  auto  pipeline = Pipeline("mask_value_display", "Metal Mask value display");
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(mask.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 1);
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

}  // namespace alcedo
