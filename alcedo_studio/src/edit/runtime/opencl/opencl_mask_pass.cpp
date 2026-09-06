//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef HAVE_OPENCL

#include "edit/runtime/opencl/opencl_mask_pass.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/graph/active_raster_mask_validation.hpp"
#include "edit/mask/active_raster_mask.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/compiled_mask_stack.hpp"
#include "edit/runtime/result_representation.hpp"
#include "edit/runtime/content_key.hpp"
#include "edit/runtime/opencl/opencl_dag_programs.hpp"
#include "opencl/opencl_api_counters.hpp"
#include "opencl/opencl_check.hpp"
#include "opencl/opencl_kernel_cache.hpp"

namespace alcedo {
namespace {

constexpr std::size_t kAlignment = 256;

struct Plane {
  cl_mem        native       = nullptr;
  std::uint32_t offset_bytes = 0;
};

struct MaskSampleParams {
  float         render_to_uv[9]{};
  std::uint32_t source_width  = 0;
  std::uint32_t source_height = 0;
  std::uint32_t output_width  = 0;
  std::uint32_t output_height = 0;
  std::uint32_t invert        = 0;
  float         opacity       = 1.0f;
  std::uint32_t pad1          = 0;
};

struct MaskFeatherParams {
  float         render_to_uv[9]{};
  std::uint32_t source_width  = 0;
  std::uint32_t source_height = 0;
  std::uint32_t output_width  = 0;
  std::uint32_t output_height = 0;
  float         radius_texels = 0.0f;
  std::uint32_t invert        = 0;
  float         opacity       = 1.0f;
};

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

struct MaskBandParams {
  std::uint32_t width         = 0;
  std::uint32_t height        = 0;
  std::uint32_t target_inside = 0;
  std::uint32_t pad           = 0;
};

struct MaskMipParams {
  std::uint32_t source_width       = 0;
  std::uint32_t source_height      = 0;
  std::uint32_t destination_width  = 0;
  std::uint32_t destination_height = 0;
};

static_assert(sizeof(MaskSampleParams) == 64);
static_assert(sizeof(MaskFeatherParams) == 64);
static_assert(sizeof(MaskAnalyticParams) == 120);
static_assert(sizeof(MaskBandParams) == 16);
static_assert(sizeof(MaskMipParams) == 16);

auto AlignUp(std::size_t value, std::size_t alignment) -> std::size_t {
  return (value + alignment - 1) & ~(alignment - 1);
}

auto PlaneBytes(std::size_t pixel_count) -> std::size_t {
  return AlignUp(pixel_count * sizeof(float), kAlignment);
}

auto HashSignedDistanceKey(const MaskAssetKey& key, Extent2D extent) -> ContentKey {
  ContentHash hash;
  hash.MixText(key.Value());
  hash.MixU32(extent.width);
  hash.MixU32(extent.height);
  return hash.Key();
}

auto SignedDistanceId(const NodeId& node, const MaskId& mask_id) -> GraphValueId {
  return MaskSignedDistanceValue(node, mask_id);
}

auto EnsureOutput(OpenClRenderWorkspace& workspace, const GraphValueId& id, Extent2D extent)
    -> ResourceLease<OpenClBackend>& {
  return workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R8});
}

auto EnsureValueBuffer(OpenClRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> OpenClBackend::Buffer& {
  auto* existing = workspace.Values().Find(id);
  if (existing != nullptr && existing->Bytes() >= bytes) {
    return *existing;
  }
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  auto* stored = workspace.Values().Find(id);
  if (stored == nullptr) {
    throw std::runtime_error("ExecuteOpenClMask: failed to store signed-distance buffer");
  }
  return *stored;
}

auto AllocateTransientPlane(OpenClRenderWorkspace& workspace, std::size_t bytes) -> Plane {
  void* pointer = workspace.TransientBuffers().Allocate(bytes);
  if (pointer == nullptr) {
    throw std::runtime_error("ExecuteOpenClMask: transient allocation failed");
  }
  const auto resolved = workspace.Device().ResolveDeviceMemory(pointer, bytes);
  return Plane{resolved.first, resolved.second};
}

template <typename Element>
auto OffsetElements(const Plane& plane, const char* label) -> cl_uint {
  if (plane.native == nullptr || plane.offset_bytes % sizeof(Element) != 0 ||
      plane.offset_bytes / sizeof(Element) > (std::numeric_limits<cl_uint>::max)()) {
    throw std::runtime_error(std::string{"ExecuteOpenClMask: invalid "} + label + " offset");
  }
  return static_cast<cl_uint>(plane.offset_bytes / sizeof(Element));
}

template <typename Element>
auto BufferOffsetElements(const OpenClBackend::Buffer& buffer, const char* label) -> cl_uint {
  if (buffer.Native() == nullptr || buffer.Bytes() % sizeof(Element) != 0) {
    throw std::runtime_error(std::string{"ExecuteOpenClMask: invalid "} + label + " buffer");
  }
  return 0;
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

void Dispatch1D(OpenClRenderDevice& device, cl_kernel kernel, std::uint32_t count,
                const char* label) {
  if (count == 0) {
    throw std::runtime_error(std::string{"ExecuteOpenClMask: empty "} + label + " dispatch");
  }
  const std::size_t global[1] = {count};
  cl_event          event     = nullptr;
  CheckOpenCl(clEnqueueNDRangeKernel(device.Workspace().Device().NativeQueue(), kernel, 1, nullptr,
                                     global, nullptr, 0, nullptr, &event),
              label);
  NoteOpenClEnqueueNdRange();
  device.Workspace().Device().TrackKernelEvent(device.CommandContext(), event);
}

template <class Key>
void GenerateMipChain(OpenClRenderDevice& device, RasterTextureLease<OpenClBackend, Key>& source) {
  for (std::size_t level = 1; level < source.MipLevelCount(); ++level) {
    auto&         previous = source.Texture(level - 1);
    auto&         next     = source.Texture(level);
    const auto    kernel = OpenClKernelCache::Instance().GetKernel(OpenCL::GpuDag::kMaskProgramName,
                                                                   OpenCL::GpuDag::kMaskMipKernelName);
    MaskMipParams params;
    params.source_width       = previous.Width();
    params.source_height      = previous.Height();
    params.destination_width  = next.Width();
    params.destination_height = next.Height();
    SetMem(kernel, 0, previous.Native(), "mask mip source");
    SetMem(kernel, 1, next.Native(), "mask mip destination");
    SetParams(kernel, 2, params, "mask mip parameters");
    Dispatch2D(device, kernel, next.Width(), next.Height(), "mask mip");
  }
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

void EncodeRasterSample(OpenClRenderDevice& device, const OpenClBackend::Texture2D& source,
                        OpenClBackend::Texture2D& output, const Matrix3x3& render_to_uv,
                        bool invert, float opacity) {
  const auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kMaskProgramName, OpenCL::GpuDag::kMaskRasterSampleKernelName);
  MaskSampleParams params;
  SetMatrix(params.render_to_uv, render_to_uv);
  params.source_width  = source.Width();
  params.source_height = source.Height();
  params.output_width  = output.Width();
  params.output_height = output.Height();
  params.invert        = invert ? 1u : 0u;
  params.opacity       = opacity;
  SetMem(kernel, 0, source.Native(), "raster mask source");
  SetMem(kernel, 1, output.Native(), "raster mask output");
  SetParams(kernel, 2, params, "raster mask parameters");
  Dispatch2D(device, kernel, output.Width(), output.Height(), "raster mask");
}

auto EncodeSignedDistance(OpenClRenderDevice& device, const OpenClBackend::Texture2D& source,
                          OpenClBackend::Buffer& distance, Extent2D extent) -> std::uint32_t {
  auto&      workspace  = device.Workspace();
  const auto pixels     = static_cast<std::size_t>(extent.width) * extent.height;
  const auto plane      = PlaneBytes(pixels);
  const auto site_bytes = AlignUp(pixels * sizeof(std::int32_t), kAlignment);
  const auto needed     = plane * 4 + site_bytes;
  auto&      transients = workspace.TransientBuffers();
  if (transients.used_bytes() == 0) {
    transients.Reserve(std::max(needed, transients.capacity_bytes()));
  }
  const auto mark              = transients.used_bytes();
  const auto horizontal        = AllocateTransientPlane(workspace, plane);
  const auto inside            = AllocateTransientPlane(workspace, plane);
  const auto outside           = AllocateTransientPlane(workspace, plane);
  const auto sites             = AllocateTransientPlane(workspace, site_bytes);
  const auto boundaries        = AllocateTransientPlane(workspace, plane);

  auto       encode_horizontal = [&](bool target_inside, const Plane& destination) {
    const auto kernel = OpenClKernelCache::Instance().GetKernel(
        OpenCL::GpuDag::kMaskProgramName, OpenCL::GpuDag::kMaskBandHorizontalKernelName);
    MaskBandParams params;
    params.width         = extent.width;
    params.height        = extent.height;
    params.target_inside = target_inside ? 1u : 0u;
    SetMem(kernel, 0, source.Native(), "horizontal band source");
    SetMem(kernel, 1, destination.native, "horizontal band destination");
    SetParams(kernel, 2, params, "horizontal band parameters");
    SetUInt(kernel, 3, OffsetElements<float>(destination, "horizontal band"),
                  "horizontal band offset");
    Dispatch1D(device, kernel, extent.height, "horizontal band");
  };

  auto encode_vertical = [&](const Plane& horizontal_plane, const Plane& destination) {
    const auto kernel = OpenClKernelCache::Instance().GetKernel(
        OpenCL::GpuDag::kMaskProgramName, OpenCL::GpuDag::kMaskBandVerticalKernelName);
    MaskBandParams params;
    params.width  = extent.width;
    params.height = extent.height;
    SetMem(kernel, 0, horizontal_plane.native, "vertical band source");
    SetMem(kernel, 1, destination.native, "vertical band destination");
    SetMem(kernel, 2, sites.native, "vertical band sites");
    SetMem(kernel, 3, boundaries.native, "vertical band boundaries");
    SetParams(kernel, 4, params, "vertical band parameters");
    SetUInt(kernel, 5, OffsetElements<float>(horizontal_plane, "vertical band source"),
            "vertical band source offset");
    SetUInt(kernel, 6, OffsetElements<float>(destination, "vertical band destination"),
            "vertical band destination offset");
    SetUInt(kernel, 7, OffsetElements<std::int32_t>(sites, "vertical band sites"),
            "vertical band sites offset");
    SetUInt(kernel, 8, OffsetElements<float>(boundaries, "vertical band boundaries"),
            "vertical band boundaries offset");
    Dispatch1D(device, kernel, extent.width, "vertical band");
  };

  encode_horizontal(true, horizontal);
  encode_vertical(horizontal, inside);
  encode_horizontal(false, horizontal);
  encode_vertical(horizontal, outside);

  const auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kMaskProgramName, OpenCL::GpuDag::kMaskComposeSignedDistanceKernelName);
  MaskBandParams params;
  params.width  = extent.width;
  params.height = extent.height;
  SetMem(kernel, 0, source.Native(), "signed-distance source");
  SetMem(kernel, 1, inside.native, "signed-distance inside");
  SetMem(kernel, 2, outside.native, "signed-distance outside");
  SetMem(kernel, 3, distance.Native(), "signed-distance output");
  SetParams(kernel, 4, params, "signed-distance parameters");
  SetUInt(kernel, 5, OffsetElements<float>(inside, "signed-distance inside"),
          "signed-distance inside offset");
  SetUInt(kernel, 6, OffsetElements<float>(outside, "signed-distance outside"),
          "signed-distance outside offset");
  SetUInt(kernel, 7, BufferOffsetElements<float>(distance, "signed-distance output"),
          "signed-distance output offset");
  Dispatch2D(device, kernel, extent.width, extent.height, "signed-distance compose");
  return static_cast<std::uint32_t>(transients.used_bytes() - mark);
}

void EncodeFeatherSample(OpenClRenderDevice& device, const OpenClBackend::Buffer& distance,
                         OpenClBackend::Texture2D& output, Extent2D source_extent,
                         const Matrix3x3& render_to_uv, float radius_texels, bool invert,
                         float opacity) {
  const auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kMaskProgramName, OpenCL::GpuDag::kMaskFeatherSampleKernelName);
  MaskFeatherParams params;
  SetMatrix(params.render_to_uv, render_to_uv);
  params.source_width  = source_extent.width;
  params.source_height = source_extent.height;
  params.output_width  = output.Width();
  params.output_height = output.Height();
  params.radius_texels = radius_texels;
  params.invert        = invert ? 1u : 0u;
  params.opacity       = opacity;
  SetMem(kernel, 0, distance.Native(), "feather distance");
  SetMem(kernel, 1, output.Native(), "feather output");
  SetParams(kernel, 2, params, "feather parameters");
  SetUInt(kernel, 3, BufferOffsetElements<float>(distance, "feather distance"),
          "feather distance offset");
  Dispatch2D(device, kernel, output.Width(), output.Height(), "mask feather");
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
                       const CompiledMaskSource& compiled_source, MaskStore* store,
                       std::span<const ActiveRasterMaskInput> active_raster_masks)
    -> OpenClMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteOpenClMask: BeginRender has not been called");
  }
  if (!active_raster_masks.empty()) {
    ValidateActiveRasterMaskBindings(document, active_raster_masks);
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

  const auto* brush = std::get_if<BrushMaskSource>(&mask_model.source);
  if (brush == nullptr) {
    throw std::runtime_error("ExecuteOpenClMask: compiled mask does not match document");
  }

  const auto* active = FindActiveRasterMaskInput(active_raster_masks, compiled_grade.node_id,
                                                 compiled_source.mask_id);
  const auto encode_coverage = [&](auto& source, const MaskAssetDescriptor& raster_descriptor,
                                   bool raster_bytes_changed, const MaskAssetKey* persistent_key) {
    result.mip_level_count = static_cast<std::uint32_t>(source.MipLevelCount());
    const auto sampling    = MakeRasterMaskSamplingPlan(
        plan.geometry, raster_descriptor.reference_bounds, raster_descriptor.extent);
    if (brush->feather_radius <= 0.0f) {
      const auto selected_level = std::min<std::size_t>(
          static_cast<std::size_t>(std::max(std::floor(sampling.mip_level), 0.0f)),
          source.MipLevelCount() - 1);
      EncodeRasterSample(device, source.Texture(selected_level), output.Texture(),
                         sampling.render_to_texture_uv, mask_model.invert, mask_model.opacity);
      return;
    }
    const auto distance_id = SignedDistanceId(compiled_grade.node_id, compiled_source.mask_id);
    const auto distance_pixels =
        static_cast<std::size_t>(raster_descriptor.extent.width) * raster_descriptor.extent.height;
    const auto  distance_bytes    = distance_pixels * sizeof(float);
    const auto* existing_distance = workspace.Values().Find(distance_id);
    const auto  metadata          = workspace.Values().GetMetadata(distance_id);
    const bool  size_matches =
        existing_distance != nullptr && existing_distance->Bytes() >= distance_bytes;
    bool key_matches = false;
    ContentKey distance_key{};
    if (persistent_key != nullptr) {
      distance_key = HashSignedDistanceKey(*persistent_key, raster_descriptor.extent);
      key_matches  = metadata.has_value() &&
                    metadata->representation.identity == distance_key.hash &&
                    metadata->extent == ImageExtent{raster_descriptor.extent.width,
                                                    raster_descriptor.extent.height};
    }
    auto&      distance     = EnsureValueBuffer(workspace, distance_id, distance_bytes);
    const bool must_compute = raster_bytes_changed || !size_matches ||
                              (persistent_key != nullptr && !key_matches);
    if (must_compute) {
      result.transient_bytes =
          EncodeSignedDistance(device, source.Texture(), distance, raster_descriptor.extent);
    }
    result.signed_distance_resource_id = distance.ResourceId();
    const auto& bounds                 = raster_descriptor.reference_bounds;
    const float x_scale =
        static_cast<float>(raster_descriptor.extent.width) /
        (static_cast<float>(plan.geometry.full_reference_extent.width) * std::max(bounds.w, 1.0e-6f));
    const float y_scale = static_cast<float>(raster_descriptor.extent.height) /
                          (static_cast<float>(plan.geometry.full_reference_extent.height) *
                           std::max(bounds.h, 1.0e-6f));
    const float radius_texels = brush->feather_radius * 0.5f * (x_scale + y_scale);
    EncodeFeatherSample(device, distance, output.Texture(), raster_descriptor.extent,
                        sampling.render_to_texture_uv, radius_texels, mask_model.invert,
                        mask_model.opacity);
    if (must_compute && persistent_key != nullptr) {
      ResultRepresentation distance_repr;
      distance_repr.identity = distance_key.hash;
      distance_repr.extent   = ImageExtent{raster_descriptor.extent.width,
                                           raster_descriptor.extent.height};
      distance_repr.format   = TextureFormat::R32f;
      workspace.Values().StoreMetadata(distance_id, 1, distance_repr, distance_repr.extent);
    }
  };

  auto upload_full = [&](auto& source, std::span<const std::uint8_t> pixels) {
    workspace.Device().UploadTexture2D(
        source.Texture(),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(pixels.data()), pixels.size()),
        device.CommandContext());
    GenerateMipChain(device, source);
  };

  if (active != nullptr) {
    auto& cache = workspace.ActiveRasterTextures();
    const ActiveRasterTextureKey tex_key{compiled_grade.node_id, compiled_source.mask_id,
                                         active->session_generation};
    cache.EraseIdleIf([&](const ActiveRasterTextureKey& key) {
      return key.owner_node_id == tex_key.owner_node_id && key.mask_id == tex_key.mask_id &&
             key.session_generation != tex_key.session_generation;
    });
    auto source                         = cache.Acquire(tex_key, active->descriptor.extent);
    result.active_texture_resource_id   = source.Texture().ResourceId();
    bool raster_bytes_changed           = false;
    if (!cache.PixelsUploaded(tex_key)) {
      upload_full(source, *active->pixels);
      cache.SetUploadedPixels(tex_key, active->content_revision);
      raster_bytes_changed = true;
    } else if (active->content_revision < cache.UploadedRevision(tex_key)) {
      throw std::runtime_error("ExecuteOpenClMask: active raster revision is stale");
    } else if (active->content_revision > cache.UploadedRevision(tex_key)) {
      const auto dirty =
          ClipRasterDirtyRectangle(active->dirty_rectangle, active->descriptor.extent);
      const auto bytes = CopyPackedR8Rectangle(*active->pixels, active->descriptor.extent, dirty);
      workspace.Device().UploadR8TextureRect(source.Texture(), dirty, bytes,
                                             device.CommandContext());
      GenerateMipChain(device, source);
      cache.SetUploadedPixels(tex_key, active->content_revision);
      raster_bytes_changed = true;
    }
    encode_coverage(source, active->descriptor, raster_bytes_changed, nullptr);
    return result;
  }

  if (store == nullptr) {
    throw std::invalid_argument("ExecuteOpenClMask: raster mask needs MaskStore");
  }
  if (!brush->asset_key.has_value() || brush->asset_key->Empty()) {
    EncodeFillZero(device, output.Texture());
    return result;
  }
  const auto asset = store->Load(*brush->asset_key);
  if (asset == nullptr || asset->descriptor.extent.Empty()) {
    throw std::runtime_error("ExecuteOpenClMask: MaskStore returned an invalid asset");
  }
  auto&      cache                      = workspace.MaskTextures();
  const bool cached                     = cache.Contains(asset->key);
  auto       source                     = cache.Acquire(asset->key, asset->descriptor.extent);
  result.persistent_texture_resource_id = source.Texture().ResourceId();
  if (!cached) {
    upload_full(source, asset->pixels);
  }
  encode_coverage(source, asset->descriptor, !cached, &asset->key);
  return result;
}

auto ExecuteOpenClMaskUnion(OpenClRenderDevice& device, const ExecutionPlan& plan,
                            const PipelineDocument& document,
                            const CompiledGradeNode& compiled_grade) -> OpenClMaskResult {
  // Union writes the full render extent from current enabled sources. Brush dirty
  // rectangles limit host-to-device upload only. Signed-distance feather uses the
  // full raster because the Euclidean field is global.
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
                       const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                       MaskStore* store, std::span<const ActiveRasterMaskInput> active_raster_masks)
    -> OpenClMaskResult {
  const auto& stack = RequireMaskStack(compiled_grade);
  OpenClMaskResult sources{};
  for (const auto& source : stack.sources) {
    if (!MaskSourceIsEnabled(document, compiled_grade.node_id, source.mask_id)) {
      continue;
    }
    sources = ExecuteOpenClMask(device, plan, document, compiled_grade, source, store,
                                active_raster_masks);
  }
  auto unified                           = ExecuteOpenClMaskUnion(device, plan, document, compiled_grade);
  unified.persistent_texture_resource_id = sources.persistent_texture_resource_id;
  unified.active_texture_resource_id     = sources.active_texture_resource_id;
  unified.signed_distance_resource_id    = sources.signed_distance_resource_id;
  unified.mip_level_count                = sources.mip_level_count;
  unified.transient_bytes                = sources.transient_bytes;
  return unified;
}

auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document, MaskStore* store,
                       std::span<const ActiveRasterMaskInput> active_raster_masks)
    -> OpenClMaskResult {
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
      result = ExecuteOpenClMask(device, plan, document, grade, store, active_raster_masks);
    }
  }
  return result;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
