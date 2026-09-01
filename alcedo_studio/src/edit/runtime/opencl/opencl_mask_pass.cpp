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
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
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
  std::uint32_t pad0          = 0;
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
  std::uint32_t pad0          = 0;
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
  std::uint32_t graduated_invert    = 0;
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

auto UnionDirtyRectangles(std::span<const RectI> rectangles, Extent2D extent) -> RectI {
  if (rectangles.empty() || extent.Empty()) {
    return {};
  }
  std::int32_t x0 = static_cast<std::int32_t>(extent.width);
  std::int32_t y0 = static_cast<std::int32_t>(extent.height);
  std::int32_t x1 = 0;
  std::int32_t y1 = 0;
  for (const auto& rectangle : rectangles) {
    if (rectangle.width <= 0 || rectangle.height <= 0) {
      continue;
    }
    x0 = std::min(x0, std::max(rectangle.x, 0));
    y0 = std::min(y0, std::max(rectangle.y, 0));
    x1 = std::max(x1, std::min(rectangle.X1(), static_cast<std::int32_t>(extent.width)));
    y1 = std::max(y1, std::min(rectangle.Y1(), static_cast<std::int32_t>(extent.height)));
  }
  return x1 > x0 && y1 > y0 ? RectI{x0, y0, x1 - x0, y1 - y0} : RectI{};
}

auto CopyRectangle(const MaskAsset& asset, RectI rectangle) -> std::vector<std::byte> {
  std::vector<std::byte> bytes(static_cast<std::size_t>(rectangle.width) * rectangle.height);
  for (std::int32_t row = 0; row < rectangle.height; ++row) {
    const auto source =
        static_cast<std::size_t>(rectangle.y + row) * asset.descriptor.extent.width +
        static_cast<std::size_t>(rectangle.x);
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

auto SignedDistanceId(const NodeId& node) -> GraphValueId {
  return {node, PortId{"signed_distance"}};
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

void GenerateMipChain(OpenClRenderDevice& device, MaskTextureLease<OpenClBackend>& source) {
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

  const auto graduated       = LinearGradientParamsFromMask(mask);
  params.origin_x            = graduated.origin_x;
  params.origin_y            = graduated.origin_y;
  params.normal_x            = graduated.normal_x;
  params.normal_y            = graduated.normal_y;
  params.transition_distance = graduated.transition_distance;
  params.start_value         = graduated.start_value;
  params.end_value           = graduated.end_value;
  params.graduated_invert    = graduated.invert ? 1u : 0u;

  SetMem(kernel, 0, output.Native(), "analytic mask output");
  SetParams(kernel, 1, params, "analytic mask parameters");
  Dispatch2D(device, kernel, output.Width(), output.Height(), "analytic mask");
}

void EncodeRasterSample(OpenClRenderDevice& device, const OpenClBackend::Texture2D& source,
                        OpenClBackend::Texture2D& output, const Matrix3x3& render_to_uv,
                        bool invert) {
  const auto kernel = OpenClKernelCache::Instance().GetKernel(
      OpenCL::GpuDag::kMaskProgramName, OpenCL::GpuDag::kMaskRasterSampleKernelName);
  MaskSampleParams params;
  SetMatrix(params.render_to_uv, render_to_uv);
  params.source_width  = source.Width();
  params.source_height = source.Height();
  params.output_width  = output.Width();
  params.output_height = output.Height();
  params.invert        = invert ? 1u : 0u;
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
                         const Matrix3x3& render_to_uv, float radius_texels, bool invert) {
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
  SetMem(kernel, 0, distance.Native(), "feather distance");
  SetMem(kernel, 1, output.Native(), "feather output");
  SetParams(kernel, 2, params, "feather parameters");
  SetUInt(kernel, 3, BufferOffsetElements<float>(distance, "feather distance"),
          "feather distance offset");
  Dispatch2D(device, kernel, output.Width(), output.Height(), "mask feather");
}

}  // namespace

auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                       MaskStore* store, std::span<const RectI> dirty_rectangles)
    -> OpenClMaskResult {
  if (!device.Workspace().IsRendering()) {
    throw std::runtime_error("ExecuteOpenClMask: BeginRender has not been called");
  }
  if (!compiled_grade.mask.has_value()) {
    throw std::runtime_error("ExecuteOpenClMask: compiled Color Grade has no mask");
  }
  const auto extent = plan.geometry.render_extent;
  if (extent.Empty() || plan.geometry.full_reference_extent.Empty()) {
    throw std::runtime_error("ExecuteOpenClMask: geometry extents must be positive");
  }

  auto&            workspace = device.Workspace();
  auto&            output    = EnsureOutput(workspace, compiled_grade.mask_output, extent);
  OpenClMaskResult result{compiled_grade.mask_output};

  const auto&      mask_model = RequireCompiledMaskModel(document, compiled_grade);
  if (std::holds_alternative<RadialMaskSource>(mask_model.source) ||
      std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    EncodeAnalytic(device, output.Texture(), mask_model, plan);
    return result;
  }

  const auto* brush = std::get_if<BrushMaskSource>(&mask_model.source);
  if (brush == nullptr) {
    throw std::runtime_error("ExecuteOpenClMask: compiled mask does not match document");
  }
  if (store == nullptr) {
    throw std::invalid_argument("ExecuteOpenClMask: raster mask needs MaskStore");
  }
  if (!brush->asset_key.has_value() || brush->asset_key->Empty()) {
    throw std::runtime_error("ExecuteOpenClMask: Brush Mask has no asset");
  }
  const auto asset = store->Load(*brush->asset_key);
  if (asset == nullptr || asset->descriptor.extent.Empty()) {
    throw std::runtime_error("ExecuteOpenClMask: MaskStore returned an invalid asset");
  }

  auto&      cache                      = workspace.MaskTextures();
  const bool cached                     = cache.Contains(asset->key);
  auto       source                     = cache.Acquire(asset->key, asset->descriptor.extent);
  result.persistent_texture_resource_id = source.Texture().ResourceId();
  result.mip_level_count                = static_cast<std::uint32_t>(source.MipLevelCount());

  const auto dirty     = UnionDirtyRectangles(dirty_rectangles, asset->descriptor.extent);
  const bool has_dirty = dirty.width > 0 && dirty.height > 0;
  if (!cached) {
    workspace.Device().UploadTexture2D(
        source.Texture(),
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(asset->pixels.data()),
                                   asset->pixels.size()),
        device.CommandContext());
    GenerateMipChain(device, source);
  } else if (has_dirty) {
    const auto bytes = CopyRectangle(*asset, dirty);
    workspace.Device().UploadR8TextureRect(source.Texture(), dirty, bytes, device.CommandContext());
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

  const auto distance_id  = SignedDistanceId(compiled_grade.node_id);
  const auto distance_key = HashSignedDistanceKey(asset->key, asset->descriptor.extent);
  const auto distance_pixels =
      static_cast<std::size_t>(asset->descriptor.extent.width) * asset->descriptor.extent.height;
  const auto  distance_bytes    = distance_pixels * sizeof(float);
  const auto* existing_distance = workspace.Values().Find(distance_id);
  const auto  metadata          = workspace.Values().GetMetadata(distance_id);
  const bool  size_matches =
      existing_distance != nullptr && existing_distance->Bytes() >= distance_bytes;
  const bool key_matches = metadata.has_value() && metadata->content_key == distance_key &&
                           metadata->extent == ImageExtent{asset->descriptor.extent.width,
                                                           asset->descriptor.extent.height};
  auto&      distance     = EnsureValueBuffer(workspace, distance_id, distance_bytes);
  const bool must_compute = !cached || has_dirty || !size_matches || !key_matches;
  if (must_compute) {
    result.transient_bytes =
        EncodeSignedDistance(device, source.Texture(), distance, asset->descriptor.extent);
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
  if (must_compute) {
    workspace.Values().StoreMetadata(
        distance_id, distance_key,
        ImageExtent{asset->descriptor.extent.width, asset->descriptor.extent.height});
  }
  return result;
}

auto ExecuteOpenClMask(OpenClRenderDevice& device, const ExecutionPlan& plan,
                       const PipelineDocument& document, MaskStore* store,
                       std::span<const RectI> dirty_rectangles) -> OpenClMaskResult {
  bool any_mask = false;
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask.has_value()) {
      any_mask = true;
      break;
    }
  }
  if (!any_mask) {
    throw std::runtime_error("ExecuteOpenClMask: plan has no mask");
  }
  OpenClMaskResult result{};
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask.has_value()) {
      result = ExecuteOpenClMask(device, plan, document, grade, store, dirty_rectangles);
    }
  }
  return result;
}

}  // namespace alcedo

#endif  // HAVE_OPENCL
