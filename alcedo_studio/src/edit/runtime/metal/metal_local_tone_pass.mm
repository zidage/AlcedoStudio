//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_local_tone_pass.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/local_tone_executor.hpp"
#include "edit/runtime/local_tone_plan.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

using local_tone_mapping::LlfSample;

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

void CopyMatrix(float* dst, const Matrix3x3& matrix) {
  for (int i = 0; i < 9; ++i) {
    dst[i] = matrix.m[i];
  }
}

void EnqueueLlfApply(MetalRenderDevice& device, const MetalBackend::Texture2D& input,
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

struct MetalLocalToneOps {
  using Device       = MetalRenderDevice;
  using Texture      = MetalBackend::Texture2D;
  using ScratchPlane = Plane;

  static constexpr const char* kErrorPrefix = "ExecuteMetalLocalTone";

  static auto TextureWidth(const Texture& texture) -> std::uint32_t { return texture.Width(); }
  static auto TextureHeight(const Texture& texture) -> std::uint32_t { return texture.Height(); }
  static auto TransientBytes(MetalRenderDevice& device) -> std::size_t {
    return device.Workspace().Device().RecordedWorkScratchBufferBytes();
  }

  static auto LookupCanonical(MetalRenderDevice& device, const GraphValueId& source_id,
                              const GraphValueId& result_id, int current_long_edge,
                              const ResolvedRenderGeometry& geometry) -> LocalToneCanonicalLookup {
    auto&      workspace    = device.Workspace();
    auto&      invalidation = workspace.ResultInvalidation();
    const auto meta         = ReadMeta(workspace, source_id.producer);
    const auto canonical    = local_tone_mapping::ComputeMaskDimensions(
        static_cast<int>(geometry.full_reference_extent.width),
        static_cast<int>(geometry.full_reference_extent.height),
        local_tone_mapping::kReferenceMaskMaxLongEdge);
    const ImageExtent canonical_extent{static_cast<std::uint32_t>(canonical.width),
                                       static_cast<std::uint32_t>(canonical.height)};
    const auto canonical_bytes = static_cast<std::size_t>(canonical.width) *
                                 static_cast<std::size_t>(canonical.height) * sizeof(float);
    const auto source_needed = invalidation.MakeImageRepresentation(
        source_id, canonical_extent, TextureFormat::R32f,
        static_cast<std::uint32_t>(current_long_edge));
    const auto result_needed = invalidation.MakeImageRepresentation(
        result_id, canonical_extent, TextureFormat::R32f,
        static_cast<std::uint32_t>(current_long_edge));
    auto* cached_source = workspace.Values().Find(source_id);
    auto* cached_result = workspace.Values().Find(result_id);
    LocalToneCanonicalLookup lookup;
    lookup.source_valid =
        meta.canonical != 0 && invalidation.IsSatisfied(source_id, source_needed) &&
        meta.mask_width == canonical.width && meta.mask_height == canonical.height &&
        cached_source != nullptr && cached_source->Bytes() >= canonical_bytes &&
        cached_source->Native() != nullptr;
    lookup.source_long_edge = meta.source_long_edge;
    lookup.extent           = {static_cast<std::uint32_t>(meta.mask_width),
                               static_cast<std::uint32_t>(meta.mask_height)};
    lookup.result_valid =
        lookup.source_valid && invalidation.IsSatisfied(result_id, result_needed) &&
        cached_result != nullptr && cached_result->Bytes() >= canonical_bytes &&
        cached_result->Native() != nullptr;
    return lookup;
  }

  static void ApplyCanonicalSample(MetalRenderDevice& device, const Texture& input, Texture& output,
                                   const GraphValueId& source_id, const GraphValueId& result_id,
                                   const LocalToneDecision& decision, std::uint32_t width,
                                   std::uint32_t height) {
    auto* source = device.Workspace().Values().Find(source_id);
    auto* result = device.Workspace().Values().Find(result_id);
    if (source == nullptr || result == nullptr) {
      throw std::runtime_error("ExecuteMetalLocalTone: canonical sample lost published planes");
    }
    EnqueueLlfApply(device, input, output, PlaneFromBuffer(*source), PlaneFromBuffer(*result), width,
                    height, static_cast<int>(decision.mask_extent.width),
                    static_cast<int>(decision.mask_extent.height), decision.apply_uv);
  }

  static auto CanonicalResourceId(MetalRenderDevice& device, const GraphValueId& source_id)
      -> std::uint64_t {
    auto* buffer = device.Workspace().Values().Find(source_id);
    return buffer == nullptr ? 0 : buffer->ResourceId();
  }

  static auto BindCanonicalSourcePlane(MetalRenderDevice& device, const GraphValueId& source_id,
                                       std::size_t) -> Plane {
    auto* buffer = device.Workspace().Values().Find(source_id);
    if (buffer == nullptr || buffer->Native() == nullptr) {
      throw std::runtime_error("ExecuteMetalLocalTone: canonical source disappeared");
    }
    return PlaneFromBuffer(*buffer);
  }

  static auto AllocateScratchPlane(MetalRenderDevice& device, std::size_t bytes) -> Plane {
    return AllocateTransientPlane(device.Workspace(), bytes);
  }

  static void ExtractReference(MetalRenderDevice& device, const Texture& input, Plane dest,
                               std::uint32_t width, std::uint32_t height,
                               const LocalToneDecision& decision,
                               const ResolvedRenderGeometry& geometry) {
    auto* encoder  = Encoder(device);
    auto  pipeline = Pipeline("local_tone_extract_reference", "Metal LLF extract reference");
    ExtractReferenceParams params;
    params.input_width   = static_cast<std::int32_t>(width);
    params.input_height  = static_cast<std::int32_t>(height);
    params.output_width  = decision.widths[0];
    params.output_height = decision.heights[0];
    params.full_ref_w    = static_cast<float>(geometry.full_reference_extent.width);
    params.full_ref_h    = static_cast<float>(geometry.full_reference_extent.height);
    CopyMatrix(params.reference_to_render, geometry.reference_to_render);
    encoder->setComputePipelineState(pipeline.get());
    encoder->setTexture(static_cast<MTL::Texture*>(input.Native()), 0);
    BindPlane(encoder, dest, 0);
    encoder->setBytes(&params, sizeof(params), 1);
    DispatchThreads(encoder, pipeline.get(), decision.widths[0], decision.heights[0]);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  }

  static void Extract(MetalRenderDevice& device, const Texture& input, Plane dest,
                      std::uint32_t width, std::uint32_t height, const LocalToneDecision& decision) {
    auto* encoder  = Encoder(device);
    auto  pipeline = Pipeline("local_tone_extract", "Metal LLF extract");
    ExtractParams params;
    params.input_width   = static_cast<std::int32_t>(width);
    params.input_height  = static_cast<std::int32_t>(height);
    params.output_width  = decision.widths[0];
    params.output_height = decision.heights[0];
    encoder->setComputePipelineState(pipeline.get());
    encoder->setTexture(static_cast<MTL::Texture*>(input.Native()), 0);
    BindPlane(encoder, dest, 0);
    encoder->setBytes(&params, sizeof(params), 1);
    DispatchThreads(encoder, pipeline.get(), decision.widths[0], decision.heights[0]);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  }

  static void PyramidDown(MetalRenderDevice& device, Plane src, Plane dst,
                          const LocalToneDecision& decision, int level) {
    auto*         encoder  = Encoder(device);
    auto          pipeline = Pipeline("local_tone_pyr_down", "Metal LLF pyr down");
    PyrDownParams params;
    params.src_width  = decision.widths[level - 1];
    params.src_height = decision.heights[level - 1];
    params.dst_width  = decision.widths[level];
    params.dst_height = decision.heights[level];
    encoder->setComputePipelineState(pipeline.get());
    BindPlane(encoder, src, 0);
    BindPlane(encoder, dst, 1);
    encoder->setBytes(&params, sizeof(params), 2);
    DispatchThreads(encoder, pipeline.get(), decision.widths[level], decision.heights[level]);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  }

  static void FillZero(MetalRenderDevice& device, Plane plane) {
    device.Workspace().Device().FillDeviceMemory(plane.ptr, plane.bytes, 0,
                                                device.CommandContext());
  }

  static void Remap(MetalRenderDevice& device, Plane src, Plane dst,
                    const LocalToneDecision& decision, const local_tone_mapping::LlfSample& sample,
                    float sigma) {
    auto*       encoder  = Encoder(device);
    auto        pipeline = Pipeline("local_tone_remap", "Metal LLF remap");
    RemapParams params;
    params.width  = decision.widths[0];
    params.height = decision.heights[0];
    params.gamma  = sample.gamma;
    params.target = sample.target;
    params.beta   = sample.beta;
    params.alpha  = sample.alpha;
    params.sigma  = sigma;
    encoder->setComputePipelineState(pipeline.get());
    BindPlane(encoder, src, 0);
    BindPlane(encoder, dst, 1);
    encoder->setBytes(&params, sizeof(params), 2);
    DispatchThreads(encoder, pipeline.get(), decision.widths[0], decision.heights[0]);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  }

  static void Select(MetalRenderDevice& device, Plane source, Plane lo, Plane lo_coarse, Plane hi,
                     Plane hi_coarse, Plane output, const LocalToneDecision& decision, int level,
                     const local_tone_mapping::LlfSample& lo_sample,
                     const local_tone_mapping::LlfSample& hi_sample, bool first, bool last,
                     bool top) {
    auto*        encoder  = Encoder(device);
    auto         pipeline = Pipeline("local_tone_select", "Metal LLF select");
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
    encoder->setComputePipelineState(pipeline.get());
    BindPlane(encoder, source, 0);
    BindPlane(encoder, lo, 1);
    BindPlane(encoder, lo_coarse, 2);
    BindPlane(encoder, hi, 3);
    BindPlane(encoder, hi_coarse, 4);
    BindPlane(encoder, output, 5);
    encoder->setBytes(&params, sizeof(params), 6);
    DispatchThreads(encoder, pipeline.get(), decision.widths[level], decision.heights[level]);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  }

  static void Collapse(MetalRenderDevice& device, Plane lap, Plane coarse, Plane output,
                       const LocalToneDecision& decision, int level) {
    auto*          encoder  = Encoder(device);
    auto           pipeline = Pipeline("local_tone_collapse", "Metal LLF collapse");
    CollapseParams params;
    params.width         = decision.widths[level];
    params.height        = decision.heights[level];
    params.coarse_width  = decision.widths[level + 1];
    params.coarse_height = decision.heights[level + 1];
    encoder->setComputePipelineState(pipeline.get());
    BindPlane(encoder, lap, 0);
    BindPlane(encoder, coarse, 1);
    BindPlane(encoder, output, 2);
    encoder->setBytes(&params, sizeof(params), 3);
    DispatchThreads(encoder, pipeline.get(), decision.widths[level], decision.heights[level]);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  }

  static void ApplyAdjusted(MetalRenderDevice& device, const Texture& input, Texture& output,
                            Plane reference, Plane adjusted, std::uint32_t width,
                            std::uint32_t height, const LocalToneDecision& decision) {
    EnqueueLlfApply(device, input, output, reference, adjusted, width, height, decision.widths[0],
                    decision.heights[0], decision.apply_uv);
  }

  static void PersistCanonicalSource(MetalRenderDevice& device, Plane plane,
                                     const GraphValueId& source_id,
                                     const LocalToneDecision& decision, int current_long_edge) {
    auto& workspace = device.Workspace();
    const auto bytes = static_cast<std::size_t>(decision.mask_extent.width) *
                       decision.mask_extent.height * sizeof(float);
    auto& buffer = EnsureValueBuffer(workspace, source_id, bytes);
    workspace.Device().CopyDeviceMemoryToBuffer(plane.ptr, buffer, 0, bytes,
                                                device.CommandContext());
    const auto needed = workspace.ResultInvalidation().MakeImageRepresentation(
        source_id, decision.mask_extent, TextureFormat::R32f,
        static_cast<std::uint32_t>(current_long_edge));
    workspace.ResultInvalidation().MarkCompleted(source_id, needed);
  }

  static void PersistCanonicalResult(MetalRenderDevice& device, Plane plane,
                                     const GraphValueId& result_id,
                                     const LocalToneDecision& decision, int current_long_edge) {
    auto& workspace = device.Workspace();
    const auto bytes = static_cast<std::size_t>(decision.mask_extent.width) *
                       decision.mask_extent.height * sizeof(float);
    auto& buffer = EnsureValueBuffer(workspace, result_id, bytes);
    workspace.Device().CopyDeviceMemoryToBuffer(plane.ptr, buffer, 0, bytes,
                                                device.CommandContext());
    CanonicalLlfMeta published;
    published.source_key       = workspace.ResultInvalidation().RequiredRevision(
        GraphValueId{result_id.producer, PortId{"local_tone.source.0"}});
    published.result_key       = workspace.ResultInvalidation().RequiredRevision(result_id);
    published.mask_width       = static_cast<std::int32_t>(decision.mask_extent.width);
    published.mask_height      = static_cast<std::int32_t>(decision.mask_extent.height);
    published.source_long_edge = current_long_edge;
    published.canonical        = 1;
    WriteMeta(device, result_id.producer, published);
    const auto needed = workspace.ResultInvalidation().MakeImageRepresentation(
        result_id, decision.mask_extent, TextureFormat::R32f,
        static_cast<std::uint32_t>(current_long_edge));
    workspace.ResultInvalidation().MarkCompleted(result_id, needed);
  }
};

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
  if (input.Native() == nullptr || output.Native() == nullptr) {
    throw std::runtime_error("ExecuteMetalLocalTone: missing input or output texture");
  }
  const auto executed = LocalToneExecutor<MetalLocalToneOps>::Execute(
      device, input, output, grade_id, shadows_slider, highlights_slider, geometry);
  MetalLocalToneResult tone;
  tone.reference_resource_id       = executed.reference_resource_id;
  tone.rebuilt_reference           = executed.rebuilt_reference;
  tone.sampled_canonical_reference = executed.sampled_canonical_reference;
  tone.transient_bytes             = executed.transient_bytes;
  return tone;
}


}  // namespace alcedo
