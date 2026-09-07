//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_local_tone_pass.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal/compute_pipeline_cache.hpp"

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
  void*         ptr    = nullptr;
  MTL::Buffer*  native = nullptr;
  std::uint32_t offset = 0;
  std::size_t   bytes  = 0;
};

struct CanonicalLlfMeta {
  std::uint64_t source_key       = 0;
  std::uint64_t result_key       = 0;
  std::int32_t  mask_width       = 0;
  std::int32_t  mask_height      = 0;
  std::int32_t  source_long_edge = 0;
  std::int32_t  canonical        = 0;
};

struct alignas(16) ExtractParams {
  std::int32_t input_width   = 0;
  std::int32_t input_height  = 0;
  std::int32_t output_width  = 0;
  std::int32_t output_height = 0;
};

struct alignas(16) ExtractReferenceParams {
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

struct alignas(16) RemapParams {
  std::int32_t width  = 0;
  std::int32_t height = 0;
  float        gamma  = 0.0f;
  float        target = 0.0f;
  float        beta   = 1.0f;
  float        alpha  = 1.0f;
  float        sigma  = 0.0f;
  std::int32_t pad    = 0;
};

struct alignas(16) PyrDownParams {
  std::int32_t src_width  = 0;
  std::int32_t src_height = 0;
  std::int32_t dst_width  = 0;
  std::int32_t dst_height = 0;
};

struct alignas(16) SelectParams {
  std::int32_t width         = 0;
  std::int32_t height        = 0;
  std::int32_t coarse_width  = 0;
  std::int32_t coarse_height = 0;
  float        gamma_lo      = 0.0f;
  float        gamma_hi      = 0.0f;
  std::int32_t first         = 0;
  std::int32_t last          = 0;
  std::int32_t top           = 0;
  std::int32_t pad[3]        = {};
};

struct alignas(16) CollapseParams {
  std::int32_t width         = 0;
  std::int32_t height        = 0;
  std::int32_t coarse_width  = 0;
  std::int32_t coarse_height = 0;
};

struct alignas(16) ApplyParams {
  std::int32_t width           = 0;
  std::int32_t height          = 0;
  std::int32_t adjusted_width  = 0;
  std::int32_t adjusted_height = 0;
  float        render_to_uv[12]{};
};

auto MakeLayout(int width, int height) -> PyramidLayout {
  PyramidLayout layout;
  layout.count =
      local_tone_mapping::ComputeLevelCount(width, height, local_tone_mapping::kPyramidRadius);
  layout.widths[0]  = width;
  layout.heights[0] = height;
  for (int level = 1; level < layout.count; ++level) {
    layout.widths[level]  = std::max(1, (layout.widths[level - 1] + 1) / 2);
    layout.heights[level] = std::max(1, (layout.heights[level - 1] + 1) / 2);
  }
  return layout;
}

auto LevelId(const NodeId& grade_id, const char* family, int level) -> GraphValueId {
  return {grade_id, PortId{std::string{"local_tone."} + family + "." + std::to_string(level)}};
}

auto MetaId(const NodeId& grade_id) -> GraphValueId {
  return {grade_id, PortId{"local_tone.canonical.meta"}};
}

auto Pipeline(const char* function, const char* label) -> NS::SharedPtr<MTL::ComputePipelineState> {
#ifndef ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH
  throw std::runtime_error("Metal local tone metallib path is not configured.");
#else
  return metal::ComputePipelineCache::Instance().GetPipelineState(
      ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH, function, label);
#endif
}

void DispatchThreads(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState* pipeline,
                     int width, int height) {
  const auto thread_width = std::max<NS::UInteger>(1, pipeline->threadExecutionWidth());
  const auto thread_height =
      std::max<NS::UInteger>(1, pipeline->maxTotalThreadsPerThreadgroup() / thread_width);
  encoder->dispatchThreads(
      MTL::Size{static_cast<NS::UInteger>(width), static_cast<NS::UInteger>(height), 1},
      MTL::Size{thread_width, thread_height, 1});
}

auto Encoder(MetalRenderDevice& device) -> MTL::ComputeCommandEncoder* {
  auto* encoder = static_cast<MTL::ComputeCommandEncoder*>(
      device.Workspace().Device().EnsureComputeCommandEncoder(device.CommandContext()));
  if (encoder == nullptr) {
    throw std::runtime_error("ExecuteMetalLocalTone: compute encoder is missing");
  }
  return encoder;
}

void BindPlane(MTL::ComputeCommandEncoder* encoder, const Plane& plane, std::uint32_t index) {
  encoder->setBuffer(plane.native, plane.offset, index);
}

auto AllocateTransientPlane(MetalRenderWorkspace& workspace, std::size_t bytes) -> Plane {
  auto& buffer = workspace.Device().AcquireRecordedWorkScratchBuffer(bytes);
  Plane plane;
  plane.ptr    = buffer.DevicePointer();
  plane.native = static_cast<MTL::Buffer*>(buffer.Native());
  plane.offset = 0;
  plane.bytes  = buffer.Bytes();
  return plane;
}

auto PlaneFromBuffer(MetalBackend::Buffer& buffer) -> Plane {
  Plane plane;
  plane.ptr    = buffer.DevicePointer();
  plane.native = static_cast<MTL::Buffer*>(buffer.Native());
  plane.offset = 0;
  plane.bytes  = buffer.Bytes();
  return plane;
}

auto EnsureValueBuffer(MetalRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> MetalBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) {
    return *existing;
  }
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  auto* stored = workspace.Values().Find(id);
  if (stored == nullptr) {
    throw std::runtime_error("ExecuteMetalLocalTone: failed to store canonical buffer");
  }
  return *stored;
}

auto ReadMeta(MetalRenderWorkspace& workspace, const NodeId& grade_id) -> CanonicalLlfMeta {
  auto* buffer = workspace.Values().Find(MetaId(grade_id));
  if (buffer == nullptr || buffer->Bytes() < sizeof(CanonicalLlfMeta) ||
      buffer->DevicePointer() == nullptr) {
    return {};
  }
  CanonicalLlfMeta meta;
  std::memcpy(&meta, buffer->DevicePointer(), sizeof(meta));
  return meta;
}

void WriteMeta(MetalRenderDevice& device, const NodeId& grade_id, const CanonicalLlfMeta& meta) {
  auto& buffer = EnsureValueBuffer(device.Workspace(), MetaId(grade_id), sizeof(CanonicalLlfMeta));
  device.Workspace().Device().UploadBufferRange(
      buffer, 0,
      std::span<const std::byte>(reinterpret_cast<const std::byte*>(&meta), sizeof(meta)),
      device.CommandContext());
}

auto LocalUvPlan(std::uint32_t width, std::uint32_t height) -> Matrix3x3 {
  Matrix3x3 matrix;
  matrix.m[0] = 1.0f / static_cast<float>(width);
  matrix.m[4] = 1.0f / static_cast<float>(height);
  return matrix;
}

void CopyMatrix(float* dst, const Matrix3x3& matrix) {
  for (int i = 0; i < 9; ++i) {
    dst[i] = matrix.m[i];
  }
}

void BuildSourcePyramid(MetalRenderDevice& device, std::array<Plane, kMaxLevels>& source,
                        const PyramidLayout& layout) {
  for (int level = 1; level < layout.count; ++level) {
    auto*         encoder  = Encoder(device);
    auto          pipeline = Pipeline("local_tone_pyr_down", "Metal LLF pyr down");
    PyrDownParams params;
    params.src_width  = layout.widths[level - 1];
    params.src_height = layout.heights[level - 1];
    params.dst_width  = layout.widths[level];
    params.dst_height = layout.heights[level];
    encoder->setComputePipelineState(pipeline.get());
    BindPlane(encoder, source[level - 1], 0);
    BindPlane(encoder, source[level], 1);
    encoder->setBytes(&params, sizeof(params), 2);
    DispatchThreads(encoder, pipeline.get(), layout.widths[level], layout.heights[level]);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  }
}

void BuildRemapPyramid(MetalRenderDevice& device, const Plane& source0,
                       std::array<Plane, kMaxLevels>& levels, const PyramidLayout& layout,
                       const LlfSample& sample, float sigma) {
  auto*       encoder  = Encoder(device);
  auto        pipeline = Pipeline("local_tone_remap", "Metal LLF remap");
  RemapParams params;
  params.width  = layout.widths[0];
  params.height = layout.heights[0];
  params.gamma  = sample.gamma;
  params.target = sample.target;
  params.beta   = sample.beta;
  params.alpha  = sample.alpha;
  params.sigma  = sigma;
  encoder->setComputePipelineState(pipeline.get());
  BindPlane(encoder, source0, 0);
  BindPlane(encoder, levels[0], 1);
  encoder->setBytes(&params, sizeof(params), 2);
  DispatchThreads(encoder, pipeline.get(), layout.widths[0], layout.heights[0]);
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  BuildSourcePyramid(device, levels, layout);
}

void ApplyAdjusted(MetalRenderDevice& device, const MetalBackend::Texture2D& input,
                   MetalBackend::Texture2D& output, const Plane& reference, const Plane& adjusted,
                   std::uint32_t width, std::uint32_t height, int adjusted_width,
                   int adjusted_height, const Matrix3x3& render_to_uv) {
  auto*       encoder  = Encoder(device);
  auto        pipeline = Pipeline("local_tone_apply", "Metal LLF apply");
  ApplyParams params;
  params.width           = static_cast<std::int32_t>(width);
  params.height          = static_cast<std::int32_t>(height);
  params.adjusted_width  = adjusted_width;
  params.adjusted_height = adjusted_height;
  CopyMatrix(params.render_to_uv, render_to_uv);
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(input.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 1);
  BindPlane(encoder, reference, 0);
  BindPlane(encoder, adjusted, 1);
  encoder->setBytes(&params, sizeof(params), 2);
  DispatchThreads(encoder, pipeline.get(), static_cast<int>(width), static_cast<int>(height));
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

}  // namespace

void AppendMetalLocalToneWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH,
                                          "local_tone_extract", "Metal LLF extract"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH,
                                          "local_tone_extract_reference",
                                          "Metal LLF extract reference"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH,
                                          "local_tone_pyr_down", "Metal LLF pyr down"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH, "local_tone_remap",
                                          "Metal LLF remap"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH,
                                          "local_tone_select", "Metal LLF select"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH,
                                          "local_tone_collapse", "Metal LLF collapse"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_LOCAL_TONE_METALLIB_PATH, "local_tone_apply",
                                          "Metal LLF apply"});
#else
  (void)pipelines;
#endif
}

auto ExecuteMetalLocalTone(MetalRenderDevice& device, const MetalBackend::Texture2D& input,
                           MetalBackend::Texture2D& output, const NodeId& grade_id,
                           float shadows_slider, float highlights_slider,
                           const ResolvedRenderGeometry& geometry) -> MetalLocalToneResult {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteMetalLocalTone: BeginRender has not been called");
  }
  if (input.Native() == nullptr || output.Native() == nullptr) {
    throw std::runtime_error("ExecuteMetalLocalTone: missing input or output texture");
  }
  if (geometry.full_reference_extent.Empty() || geometry.render_extent.Empty()) {
    throw std::runtime_error("ExecuteMetalLocalTone: geometry extents must be positive");
  }

  const auto width  = input.Width();
  const auto height = input.Height();
  if (output.Width() != width || output.Height() != height) {
    throw std::runtime_error("ExecuteMetalLocalTone: output extent does not match input");
  }

  const auto canonical_dims = local_tone_mapping::ComputeMaskDimensions(
      static_cast<int>(geometry.full_reference_extent.width),
      static_cast<int>(geometry.full_reference_extent.height),
      local_tone_mapping::kReferenceMaskMaxLongEdge);
  const bool full_edit         = CoversFullEditSpace(geometry);
  const int  current_long_edge = std::max(static_cast<int>(width), static_cast<int>(height));
  const auto canonical_bytes   = static_cast<std::size_t>(canonical_dims.width) *
                               static_cast<std::size_t>(canonical_dims.height) * sizeof(float);
  const auto meta          = ReadMeta(workspace, grade_id);
  auto&      invalidation  = workspace.ResultInvalidation();
  const auto source_id     = LocalToneSourceId(grade_id);
  const auto result_id     = LocalToneResultId(grade_id);
  const bool persist_llf   = workspace.PersistsResult(source_id);
  auto*      cached_source = persist_llf ? workspace.Values().Find(source_id) : nullptr;
  auto*      cached_result = persist_llf ? workspace.Values().Find(result_id) : nullptr;
  if (!persist_llf) {
    device.PassStats().result_policy_bypass += 2;
  }
  const ImageExtent canonical_extent{static_cast<std::uint32_t>(canonical_dims.width),
                                     static_cast<std::uint32_t>(canonical_dims.height)};
  const auto source_needed = invalidation.MakeImageRepresentation(
      source_id, canonical_extent, TextureFormat::R32f,
      static_cast<std::uint32_t>(current_long_edge));
  const auto result_needed = invalidation.MakeImageRepresentation(
      result_id, canonical_extent, TextureFormat::R32f,
      static_cast<std::uint32_t>(current_long_edge));
  const bool source_valid  = meta.canonical != 0 &&
                            invalidation.IsSatisfied(source_id, source_needed) &&
                            meta.mask_width == canonical_dims.width &&
                            meta.mask_height == canonical_dims.height && cached_source != nullptr &&
                            cached_source->Bytes() >= canonical_bytes &&
                            cached_source->Native() != nullptr;
  const bool result_valid = source_valid && invalidation.IsSatisfied(result_id, result_needed) &&
                            cached_result != nullptr && cached_result->Bytes() >= canonical_bytes &&
                            cached_result->Native() != nullptr;
  const bool sample_canonical =
      result_valid && !(full_edit && current_long_edge > meta.source_long_edge);

  MetalLocalToneResult tone;
  if (sample_canonical) {
    const auto sampling =
        MakeLlfSamplingPlan(geometry, Extent2D{static_cast<std::uint32_t>(meta.mask_width),
                                               static_cast<std::uint32_t>(meta.mask_height)});
    ApplyAdjusted(device, input, output, PlaneFromBuffer(*cached_source),
                  PlaneFromBuffer(*cached_result), width, height, meta.mask_width, meta.mask_height,
                  sampling.render_to_texture_uv);
    tone.reference_resource_id       = cached_source->ResourceId();
    tone.sampled_canonical_reference = true;
    return tone;
  }

  const bool build_canonical = full_edit;
  const bool reuse_source =
      source_valid && !(full_edit && current_long_edge > meta.source_long_edge);
  const auto mask_dims = build_canonical || reuse_source
                             ? canonical_dims
                             : local_tone_mapping::ComputeMaskDimensions(
                                   static_cast<int>(width), static_cast<int>(height),
                                   local_tone_mapping::kReferenceMaskMaxLongEdge);
  const auto layout = MakeLayout(mask_dims.width, mask_dims.height);

  std::array<Plane, kMaxLevels> source{};
  std::array<Plane, kMaxLevels> remap_a{};
  std::array<Plane, kMaxLevels> remap_b{};
  std::array<Plane, kMaxLevels> result{};
  const auto mark = workspace.Device().RecordedWorkScratchBufferBytes();
  for (int level = 0; level < layout.count; ++level) {
    const auto bytes =
        static_cast<std::size_t>(layout.widths[level]) * layout.heights[level] * sizeof(float);
    if (level == 0 && reuse_source) {
      source[level] = PlaneFromBuffer(*cached_source);
    } else {
      source[level] = AllocateTransientPlane(workspace, bytes);
    }
    remap_a[level] = AllocateTransientPlane(workspace, bytes);
    remap_b[level] = AllocateTransientPlane(workspace, bytes);
    result[level]  = AllocateTransientPlane(workspace, bytes);
  }
  tone.transient_bytes =
      static_cast<std::uint32_t>(workspace.Device().RecordedWorkScratchBufferBytes() - mark);

  if (!reuse_source) {
    auto* encoder = Encoder(device);
    if (build_canonical) {
      auto pipeline = Pipeline("local_tone_extract_reference", "Metal LLF extract reference");
      ExtractReferenceParams params;
      params.input_width   = static_cast<std::int32_t>(width);
      params.input_height  = static_cast<std::int32_t>(height);
      params.output_width  = layout.widths[0];
      params.output_height = layout.heights[0];
      params.full_ref_w    = static_cast<float>(geometry.full_reference_extent.width);
      params.full_ref_h    = static_cast<float>(geometry.full_reference_extent.height);
      CopyMatrix(params.reference_to_render, geometry.reference_to_render);
      encoder->setComputePipelineState(pipeline.get());
      encoder->setTexture(static_cast<MTL::Texture*>(input.Native()), 0);
      BindPlane(encoder, source[0], 0);
      encoder->setBytes(&params, sizeof(params), 1);
      DispatchThreads(encoder, pipeline.get(), layout.widths[0], layout.heights[0]);
    } else {
      auto          pipeline = Pipeline("local_tone_extract", "Metal LLF extract");
      ExtractParams params;
      params.input_width   = static_cast<std::int32_t>(width);
      params.input_height  = static_cast<std::int32_t>(height);
      params.output_width  = layout.widths[0];
      params.output_height = layout.heights[0];
      encoder->setComputePipelineState(pipeline.get());
      encoder->setTexture(static_cast<MTL::Texture*>(input.Native()), 0);
      BindPlane(encoder, source[0], 0);
      encoder->setBytes(&params, sizeof(params), 1);
      DispatchThreads(encoder, pipeline.get(), layout.widths[0], layout.heights[0]);
    }
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
    BuildSourcePyramid(device, source, layout);
  } else if (layout.count > 1) {
    BuildSourcePyramid(device, source, layout);
  }

  const float shadow_amount    = std::clamp(shadows_slider * 1.5f / 80.0f, -1.5f, 1.5f);
  const float highlight_amount = std::clamp(-highlights_slider * 1.5f / 100.0f, -1.5f, 1.5f);
  const float sigma            = local_tone_mapping::SigmaR(shadow_amount, highlight_amount);
  const auto  samples          = local_tone_mapping::BuildSamples(shadow_amount, highlight_amount);
  for (int level = 0; level < layout.count; ++level) {
    workspace.Device().FillDeviceMemory(result[level].ptr, result[level].bytes, 0,
                                        device.CommandContext());
  }
  BuildRemapPyramid(device, source[0], remap_a, layout, samples[0], sigma);
  BuildRemapPyramid(device, source[0], remap_b, layout, samples[1], sigma);
  for (std::size_t pair = 0; pair + 1 < samples.size(); ++pair) {
    for (int level = 0; level < layout.count; ++level) {
      const bool   top      = level + 1 == layout.count;
      auto*        encoder  = Encoder(device);
      auto         pipeline = Pipeline("local_tone_select", "Metal LLF select");
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
      encoder->setComputePipelineState(pipeline.get());
      BindPlane(encoder, source[level], 0);
      BindPlane(encoder, remap_a[level], 1);
      BindPlane(encoder, top ? remap_a[level] : remap_a[level + 1], 2);
      BindPlane(encoder, remap_b[level], 3);
      BindPlane(encoder, top ? remap_b[level] : remap_b[level + 1], 4);
      BindPlane(encoder, result[level], 5);
      encoder->setBytes(&params, sizeof(params), 6);
      DispatchThreads(encoder, pipeline.get(), layout.widths[level], layout.heights[level]);
      device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
    }
    if (pair + 2 < samples.size()) {
      std::swap(remap_a, remap_b);
      BuildRemapPyramid(device, source[0], remap_b, layout, samples[pair + 2], sigma);
    }
  }
  for (int level = layout.count - 2; level >= 0; --level) {
    auto*          encoder  = Encoder(device);
    auto           pipeline = Pipeline("local_tone_collapse", "Metal LLF collapse");
    CollapseParams params;
    params.width         = layout.widths[level];
    params.height        = layout.heights[level];
    params.coarse_width  = layout.widths[level + 1];
    params.coarse_height = layout.heights[level + 1];
    encoder->setComputePipelineState(pipeline.get());
    BindPlane(encoder, result[level], 0);
    BindPlane(encoder, result[level + 1], 1);
    BindPlane(encoder, remap_a[level], 2);
    encoder->setBytes(&params, sizeof(params), 3);
    DispatchThreads(encoder, pipeline.get(), layout.widths[level], layout.heights[level]);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
    std::swap(result[level], remap_a[level]);
  }

  const Matrix3x3 apply_uv =
      build_canonical || reuse_source
          ? MakeLlfSamplingPlan(geometry, Extent2D{static_cast<std::uint32_t>(layout.widths[0]),
                                                   static_cast<std::uint32_t>(layout.heights[0])})
                .render_to_texture_uv
          : LocalUvPlan(width, height);
  ApplyAdjusted(device, input, output, source[0], result[0], width, height, layout.widths[0],
                layout.heights[0], apply_uv);

  if (persist_llf && (build_canonical || reuse_source)) {
    const auto bytes =
        static_cast<std::size_t>(layout.widths[0]) * layout.heights[0] * sizeof(float);
    auto& source_buffer = EnsureValueBuffer(workspace, LevelId(grade_id, "source", 0), bytes);
    auto& result_buffer = EnsureValueBuffer(workspace, LevelId(grade_id, "result", 0), bytes);
    if (!reuse_source) {
      workspace.Device().CopyDeviceMemoryToBuffer(source[0].ptr, source_buffer, 0, bytes,
                                                  device.CommandContext());
    }
    workspace.Device().CopyDeviceMemoryToBuffer(result[0].ptr, result_buffer, 0, bytes,
                                                device.CommandContext());
    CanonicalLlfMeta published;
    published.source_key       = invalidation.RequiredRevision(source_id);
    published.result_key       = invalidation.RequiredRevision(result_id);
    published.mask_width       = layout.widths[0];
    published.mask_height      = layout.heights[0];
    published.source_long_edge = current_long_edge;
    published.canonical        = 1;
    WriteMeta(device, grade_id, published);
    if (!reuse_source) {
      invalidation.MarkCompleted(source_id, source_needed);
    }
    invalidation.MarkCompleted(result_id, result_needed);
    tone.reference_resource_id = source_buffer.ResourceId();
  } else if (persist_llf && workspace.Values().Find(MetaId(grade_id)) != nullptr) {
    WriteMeta(device, grade_id, CanonicalLlfMeta{});
    tone.reference_resource_id = 0;
  }
  tone.rebuilt_reference = true;
  return tone;
}

}  // namespace alcedo
