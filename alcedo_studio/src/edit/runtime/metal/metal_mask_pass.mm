//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/metal/metal_mask_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include <alcedo/metal/Metal.hpp>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/content_key.hpp"
#include "metal/compute_pipeline_cache.hpp"

namespace alcedo {
namespace {

constexpr std::size_t kAlign = 256;

struct Plane {
  void*         ptr    = nullptr;
  MTL::Buffer*  native = nullptr;
  std::uint32_t offset = 0;
  std::size_t   bytes  = 0;
};

struct SignedDistanceMeta {
  std::uint64_t content_hash = 0;
  std::uint32_t width        = 0;
  std::uint32_t height       = 0;
};

struct alignas(16) MaskSampleParams {
  float         render_to_uv[9]{};
  std::uint32_t source_width  = 0;
  std::uint32_t source_height = 0;
  std::uint32_t output_width  = 0;
  std::uint32_t output_height = 0;
  std::uint32_t invert        = 0;
  std::uint32_t pad0          = 0;
  std::uint32_t pad1          = 0;
};

struct alignas(16) MaskFeatherParams {
  float         render_to_uv[9]{};
  std::uint32_t source_width  = 0;
  std::uint32_t source_height = 0;
  std::uint32_t output_width  = 0;
  std::uint32_t output_height = 0;
  float         radius_texels = 0.0f;
  std::uint32_t invert        = 0;
  std::uint32_t pad0          = 0;
};

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
  std::uint32_t graduated_invert    = 0;
};

struct alignas(16) MaskBandParams {
  std::uint32_t width         = 0;
  std::uint32_t height        = 0;
  std::uint32_t target_inside = 0;
  std::uint32_t pad           = 0;
};

struct alignas(16) MaskMipParams {
  std::uint32_t source_width       = 0;
  std::uint32_t source_height      = 0;
  std::uint32_t destination_width  = 0;
  std::uint32_t destination_height = 0;
};

auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

auto UnionDirtyRectangles(std::span<const RectI> rectangles, Extent2D extent) -> RectI {
  if (rectangles.empty()) {
    return {};
  }
  std::int32_t x0 = static_cast<std::int32_t>(extent.width);
  std::int32_t y0 = static_cast<std::int32_t>(extent.height);
  std::int32_t x1 = 0;
  std::int32_t y1 = 0;
  for (const auto& rect : rectangles) {
    if (rect.width <= 0 || rect.height <= 0) {
      continue;
    }
    x0 = std::min(x0, std::max(rect.x, 0));
    y0 = std::min(y0, std::max(rect.y, 0));
    x1 = std::max(x1, std::min(rect.X1(), static_cast<std::int32_t>(extent.width)));
    y1 = std::max(y1, std::min(rect.Y1(), static_cast<std::int32_t>(extent.height)));
  }
  return x1 > x0 && y1 > y0 ? RectI{x0, y0, x1 - x0, y1 - y0} : RectI{};
}

auto CopyRectangle(const MaskAsset& asset, RectI rectangle) -> std::vector<std::byte> {
  std::vector<std::byte> bytes(static_cast<std::size_t>(rectangle.width) * rectangle.height);
  for (std::int32_t row = 0; row < rectangle.height; ++row) {
    const auto source =
        static_cast<std::size_t>(rectangle.y + row) * asset.descriptor.extent.width + rectangle.x;
    std::copy_n(reinterpret_cast<const std::byte*>(asset.pixels.data() + source), rectangle.width,
                bytes.data() + static_cast<std::size_t>(row) * rectangle.width);
  }
  return bytes;
}

auto HashSignedDistanceKey(const MaskAssetKey& key, Extent2D extent) -> ContentKey {
  ContentHash hash;
  hash.MixText(key.Value());
  hash.MixU32(extent.width);
  hash.MixU32(extent.height);
  return hash.Key();
}

auto DistanceId(const NodeId& node) -> GraphValueId { return {node, PortId{"signed_distance"}}; }

auto DistanceMetaId(const NodeId& node) -> GraphValueId {
  return {node, PortId{"signed_distance.meta"}};
}

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

void Dispatch1D(MTL::ComputeCommandEncoder* encoder, MTL::ComputePipelineState*,
                std::uint32_t               count) {
  encoder->dispatchThreads(MTL::Size{count, 1, 1}, MTL::Size{1, 1, 1});
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

auto EnsureValueBuffer(MetalRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> MetalBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) {
    return *existing;
  }
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  auto* stored = workspace.Values().Find(id);
  if (stored == nullptr) {
    throw std::runtime_error("ExecuteMetalMask: failed to store signed-distance buffer");
  }
  return *stored;
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

void GenerateMipChain(MetalRenderDevice& device, MaskTextureLease<MetalBackend>& source) {
  for (std::size_t level = 1; level < source.MipLevelCount(); ++level) {
    auto&         previous = source.Texture(level - 1);
    auto&         next     = source.Texture(level);
    auto*         encoder  = Encoder(device);
    auto          pipeline = Pipeline("mask_generate_r8_mip", "Metal Mask mip");
    MaskMipParams params;
    params.source_width       = previous.Width();
    params.source_height      = previous.Height();
    params.destination_width  = next.Width();
    params.destination_height = next.Height();
    encoder->setComputePipelineState(pipeline.get());
    encoder->setTexture(static_cast<MTL::Texture*>(previous.Native()), 0);
    encoder->setTexture(static_cast<MTL::Texture*>(next.Native()), 1);
    encoder->setBytes(&params, sizeof(params), 0);
    Dispatch2D(encoder, pipeline.get(), next.Width(), next.Height());
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  }
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
  const auto graduated       = LinearGradientParamsFromMask(mask);
  params.origin_x            = graduated.origin_x;
  params.origin_y            = graduated.origin_y;
  params.normal_x            = graduated.normal_x;
  params.normal_y            = graduated.normal_y;
  params.transition_distance = graduated.transition_distance;
  params.start_value         = graduated.start_value;
  params.end_value           = graduated.end_value;
  params.graduated_invert    = graduated.invert ? 1u : 0u;
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 0);
  encoder->setBytes(&params, sizeof(params), 0);
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

void EncodeRasterSample(MetalRenderDevice& device, const MetalBackend::Texture2D& source,
                        MetalBackend::Texture2D& output, const Matrix3x3& render_to_uv,
                        bool invert) {
  auto*            encoder  = Encoder(device);
  auto             pipeline = Pipeline("mask_raster_sample", "Metal Mask raster sample");
  MaskSampleParams params;
  CopyMatrix(params.render_to_uv, render_to_uv);
  params.source_width  = source.Width();
  params.source_height = source.Height();
  params.output_width  = output.Width();
  params.output_height = output.Height();
  params.invert        = invert ? 1u : 0u;
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(source.Native()), 0);
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 1);
  encoder->setBytes(&params, sizeof(params), 0);
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

auto EncodeSignedDistance(MetalRenderDevice& device, const MetalBackend::Texture2D& source,
                          MetalBackend::Buffer& distance, std::uint32_t width,
                          std::uint32_t height) -> std::uint32_t {
  auto&      workspace  = device.Workspace();
  const auto pixels     = static_cast<std::size_t>(width) * height;
  const auto plane      = AlignUp(pixels * sizeof(float), kAlign);
  const auto sites      = AlignUp(pixels * sizeof(int), kAlign);
  const auto mark        = workspace.Device().RecordedWorkScratchBufferBytes();
  const auto horizontal  = AllocateTransientPlane(workspace, plane);
  const auto inside      = AllocateTransientPlane(workspace, plane);
  const auto outside     = AllocateTransientPlane(workspace, plane);
  const auto site_plane  = AllocateTransientPlane(workspace, sites);
  const auto bound_plane = AllocateTransientPlane(workspace, plane);

  auto       dispatch_horizontal = [&](bool target_inside, const Plane& dest) {
    auto*          encoder  = Encoder(device);
    auto           pipeline = Pipeline("mask_band_horizontal", "Metal Mask band H");
    MaskBandParams params;
    params.width         = width;
    params.height        = height;
    params.target_inside = target_inside ? 1u : 0u;
    encoder->setComputePipelineState(pipeline.get());
    encoder->setTexture(static_cast<MTL::Texture*>(source.Native()), 0);
    encoder->setBuffer(dest.native, dest.offset, 0);
    encoder->setBytes(&params, sizeof(params), 1);
    Dispatch1D(encoder, pipeline.get(), height);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  };
  auto dispatch_vertical = [&](const Plane& src, const Plane& dest) {
    auto*          encoder  = Encoder(device);
    auto           pipeline = Pipeline("mask_band_vertical", "Metal Mask band V");
    MaskBandParams params;
    params.width  = width;
    params.height = height;
    encoder->setComputePipelineState(pipeline.get());
    encoder->setBuffer(src.native, src.offset, 0);
    encoder->setBuffer(dest.native, dest.offset, 1);
    encoder->setBuffer(site_plane.native, site_plane.offset, 2);
    encoder->setBuffer(bound_plane.native, bound_plane.offset, 3);
    encoder->setBytes(&params, sizeof(params), 4);
    Dispatch1D(encoder, pipeline.get(), width);
    device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  };

  dispatch_horizontal(true, horizontal);
  dispatch_vertical(horizontal, inside);
  dispatch_horizontal(false, horizontal);
  dispatch_vertical(horizontal, outside);

  auto*          encoder  = Encoder(device);
  auto           pipeline = Pipeline("mask_compose_signed_distance", "Metal Mask compose SDF");
  MaskBandParams params;
  params.width  = width;
  params.height = height;
  encoder->setComputePipelineState(pipeline.get());
  encoder->setTexture(static_cast<MTL::Texture*>(source.Native()), 0);
  encoder->setBuffer(inside.native, inside.offset, 0);
  encoder->setBuffer(outside.native, outside.offset, 1);
  encoder->setBuffer(static_cast<MTL::Buffer*>(distance.Native()), 0, 2);
  encoder->setBytes(&params, sizeof(params), 3);
  Dispatch2D(encoder, pipeline.get(), width, height);
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
  return static_cast<std::uint32_t>(workspace.Device().RecordedWorkScratchBufferBytes() - mark);
}

void EncodeFeatherSample(MetalRenderDevice& device, const MetalBackend::Buffer& distance,
                         MetalBackend::Texture2D& output, Extent2D source_extent,
                         const Matrix3x3& render_to_uv, float radius_texels, bool invert) {
  auto*             encoder  = Encoder(device);
  auto              pipeline = Pipeline("mask_feather_sample", "Metal Mask feather");
  MaskFeatherParams params;
  CopyMatrix(params.render_to_uv, render_to_uv);
  params.source_width  = source_extent.width;
  params.source_height = source_extent.height;
  params.output_width  = output.Width();
  params.output_height = output.Height();
  params.radius_texels = radius_texels;
  params.invert        = invert ? 1u : 0u;
  encoder->setComputePipelineState(pipeline.get());
  encoder->setBuffer(static_cast<MTL::Buffer*>(distance.Native()), 0, 0);
  encoder->setTexture(static_cast<MTL::Texture*>(output.Native()), 0);
  encoder->setBytes(&params, sizeof(params), 1);
  Dispatch2D(encoder, pipeline.get(), output.Width(), output.Height());
  device.Workspace().Device().NoteComputeDispatch(device.CommandContext());
}

}  // namespace

void AppendMetalMaskWarmup(std::vector<MetalPipelineWarmup>& pipelines) {
#ifdef ALCEDO_METAL_MASK_METALLIB_PATH
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_generate_r8_mip",
                                          "Metal Mask mip"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_raster_sample",
                                          "Metal Mask raster sample"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_band_horizontal",
                                          "Metal Mask band H"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_band_vertical",
                                          "Metal Mask band V"});
  pipelines.push_back(MetalPipelineWarmup{
      ALCEDO_METAL_MASK_METALLIB_PATH, "mask_compose_signed_distance", "Metal Mask compose SDF"});
  pipelines.push_back(MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_feather_sample",
                                          "Metal Mask feather"});
  pipelines.push_back(
      MetalPipelineWarmup{ALCEDO_METAL_MASK_METALLIB_PATH, "mask_analytic", "Metal Mask analytic"});
#else
  (void)pipelines;
#endif
}

auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                      const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                      MaskStore* store, std::span<const RectI> dirty_rectangles)
    -> MetalMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteMetalMask: BeginRender has not been called");
  }
  if (!compiled_grade.mask.has_value()) {
    throw std::runtime_error("ExecuteMetalMask: compiled Color Grade has no mask");
  }
  auto&           workspace = device.Workspace();
  auto&           context   = device.CommandContext();
  const auto      extent    = plan.geometry.render_extent;
  auto&           output    = EnsureOutput(workspace, compiled_grade.mask_output, extent);
  MetalMaskResult result{compiled_grade.mask_output};

  const auto&     mask_model = RequireCompiledMaskModel(document, compiled_grade);
  if (std::holds_alternative<RadialMaskSource>(mask_model.source) ||
      std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    EncodeAnalytic(device, output.Texture(), mask_model, plan);
    return result;
  }
  const auto* brush = std::get_if<BrushMaskSource>(&mask_model.source);
  if (brush == nullptr) {
    throw std::runtime_error("ExecuteMetalMask: compiled mask does not match document");
  }
  if (store == nullptr) {
    throw std::invalid_argument("ExecuteMetalMask: raster mask needs MaskStore");
  }
  if (!brush->asset_key.has_value() || brush->asset_key->Empty()) {
    throw std::runtime_error("ExecuteMetalMask: Brush Mask has no asset");
  }
  const auto asset  = store->Load(*brush->asset_key);
  const bool cached = workspace.MaskTextures().Contains(asset->key);
  auto       source = workspace.MaskTextures().Acquire(asset->key, asset->descriptor.extent);
  result.persistent_texture_resource_id = source.Texture().ResourceId();
  result.mip_level_count                = static_cast<std::uint32_t>(source.MipLevelCount());
  const auto dirty     = UnionDirtyRectangles(dirty_rectangles, asset->descriptor.extent);
  const bool has_dirty = dirty.width > 0 && dirty.height > 0;
  if (!cached) {
    workspace.Device().UploadTexture2D(
        source.Texture(),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(asset->pixels.data()),
                                   asset->pixels.size()),
        context);
    GenerateMipChain(device, source);
  } else if (has_dirty) {
    const auto bytes = CopyRectangle(*asset, dirty);
    workspace.Device().UploadR8TextureRect(source.Texture(), dirty, bytes, context);
    GenerateMipChain(device, source);
  }

  const auto sampling = MakeRasterMaskSamplingPlan(plan.geometry, brush->descriptor.reference_bounds,
                                                   asset->descriptor.extent);
  if (brush->feather_radius <= 0.0f) {
    const auto selected_level = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(std::floor(sampling.mip_level), 0.0f)),
        source.MipLevelCount() - 1);
    EncodeRasterSample(device, source.Texture(selected_level), output.Texture(),
                       sampling.render_to_texture_uv, mask_model.invert);
    return result;
  }

  const auto distance_id    = DistanceId(compiled_grade.node_id);
  const auto meta_id        = DistanceMetaId(compiled_grade.node_id);
  const auto distance_key   = HashSignedDistanceKey(asset->key, asset->descriptor.extent);
  const auto distance_bytes = static_cast<std::size_t>(asset->descriptor.extent.width) *
                              asset->descriptor.extent.height * sizeof(float);
  auto&              distance = EnsureValueBuffer(workspace, distance_id, distance_bytes);
  auto&              meta_buf = EnsureValueBuffer(workspace, meta_id, sizeof(SignedDistanceMeta));
  SignedDistanceMeta meta{};
  if (meta_buf.DevicePointer() != nullptr && meta_buf.Bytes() >= sizeof(SignedDistanceMeta)) {
    std::memcpy(&meta, meta_buf.DevicePointer(), sizeof(meta));
  }
  const bool key_matches = meta.content_hash == distance_key.hash &&
                           meta.width == asset->descriptor.extent.width &&
                           meta.height == asset->descriptor.extent.height;
  const bool must_compute =
      !cached || has_dirty || !key_matches || distance.Bytes() < distance_bytes;
  if (must_compute) {
    result.transient_bytes =
        EncodeSignedDistance(device, source.Texture(), distance, asset->descriptor.extent.width,
                             asset->descriptor.extent.height);
    meta.content_hash = distance_key.hash;
    meta.width        = asset->descriptor.extent.width;
    meta.height       = asset->descriptor.extent.height;
    workspace.Device().UploadBufferRange(
        meta_buf, 0,
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(&meta), sizeof(meta)),
        context);
  }
  result.signed_distance_resource_id = distance.ResourceId();
  const auto  bounds                 = brush->descriptor.reference_bounds;
  const float x_scale =
      static_cast<float>(asset->descriptor.extent.width) /
      (static_cast<float>(plan.geometry.full_reference_extent.width) * std::max(bounds.w, 1.0e-6f));
  const float y_scale = static_cast<float>(asset->descriptor.extent.height) /
                        (static_cast<float>(plan.geometry.full_reference_extent.height) *
                         std::max(bounds.h, 1.0e-6f));
  const float radius_texels = brush->feather_radius * 0.5f * (x_scale + y_scale);
  EncodeFeatherSample(device, distance, output.Texture(), asset->descriptor.extent,
                      sampling.render_to_texture_uv, radius_texels, mask_model.invert);
  return result;
}

auto ExecuteMetalMask(MetalRenderDevice& device, const ExecutionPlan& plan,
                      const PipelineDocument& document, MaskStore* store,
                      std::span<const RectI> dirty_rectangles) -> MetalMaskResult {
  bool any_mask = false;
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask.has_value()) {
      any_mask = true;
      break;
    }
  }
  if (!any_mask) {
    throw std::runtime_error("ExecuteMetalMask: plan has no mask");
  }
  MetalMaskResult result{};
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask.has_value()) {
      result = ExecuteMetalMask(device, plan, document, grade, store, dirty_rectangles);
    }
  }
  return result;
}

}  // namespace alcedo
