//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <variant>
#include <vector>

#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/mask/mask_model.hpp"
#include "edit/runtime/compiled_grade_mask.hpp"
#include "edit/runtime/cuda/cuda_mask_pass.hpp"

namespace alcedo {
namespace {

auto UnionDirtyRectangles(std::span<const RectI> rectangles, Extent2D extent) -> RectI {
  if (rectangles.empty()) return {};
  std::int32_t x0 = static_cast<std::int32_t>(extent.width);
  std::int32_t y0 = static_cast<std::int32_t>(extent.height);
  std::int32_t x1 = 0;
  std::int32_t y1 = 0;
  for (const auto& rect : rectangles) {
    if (rect.width <= 0 || rect.height <= 0) continue;
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

auto EnsureOutput(CudaRenderWorkspace& workspace, const GraphValueId& id, Extent2D extent)
    -> ResourceLease<CudaBackend>& {
  return workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R8});
}

__device__ auto Transform(const float* matrix, float x, float y) -> float2 {
  return make_float2(matrix[0] * x + matrix[1] * y + matrix[2],
                     matrix[3] * x + matrix[4] * y + matrix[5]);
}

__device__ auto SampleR8(const std::uint8_t* pixels, std::uint32_t width, std::uint32_t height,
                         float u, float v) -> float {
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) return 0.0f;
  const float x  = u * width - 0.5f;
  const float y  = v * height - 0.5f;
  const int   x0 = max(0, min(static_cast<int>(width) - 1, static_cast<int>(floorf(x))));
  const int   y0 = max(0, min(static_cast<int>(height) - 1, static_cast<int>(floorf(y))));
  const int   x1 = min(x0 + 1, static_cast<int>(width) - 1);
  const int   y1 = min(y0 + 1, static_cast<int>(height) - 1);
  const float tx = fminf(fmaxf(x - floorf(x), 0.0f), 1.0f);
  const float ty = fminf(fmaxf(y - floorf(y), 0.0f), 1.0f);
  const float a  = pixels[y0 * width + x0] * (1.0f - tx) + pixels[y0 * width + x1] * tx;
  const float b  = pixels[y1 * width + x0] * (1.0f - tx) + pixels[y1 * width + x1] * tx;
  return (a * (1.0f - ty) + b * ty) / 255.0f;
}

__device__ auto SampleF32(const float* pixels, std::uint32_t width, std::uint32_t height, float u,
                          float v) -> float {
  if (u < 0.0f || v < 0.0f || u > 1.0f || v > 1.0f) return -1.0e6f;
  const float x  = u * width - 0.5f;
  const float y  = v * height - 0.5f;
  const int   x0 = max(0, min(static_cast<int>(width) - 1, static_cast<int>(floorf(x))));
  const int   y0 = max(0, min(static_cast<int>(height) - 1, static_cast<int>(floorf(y))));
  const int   x1 = min(x0 + 1, static_cast<int>(width) - 1);
  const int   y1 = min(y0 + 1, static_cast<int>(height) - 1);
  const float tx = fminf(fmaxf(x - floorf(x), 0.0f), 1.0f);
  const float ty = fminf(fmaxf(y - floorf(y), 0.0f), 1.0f);
  const float a  = pixels[y0 * width + x0] * (1.0f - tx) + pixels[y0 * width + x1] * tx;
  const float b  = pixels[y1 * width + x0] * (1.0f - tx) + pixels[y1 * width + x1] * tx;
  return a * (1.0f - ty) + b * ty;
}

__global__ void RasterSampleKernel(const std::uint8_t* source, std::uint32_t source_width,
                                   std::uint32_t source_height, std::uint8_t* output,
                                   std::uint32_t output_width, std::uint32_t output_height,
                                   Matrix3x3 render_to_uv, bool invert) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= output_width * output_height) return;
  const auto x     = index % output_width;
  const auto y     = index / output_width;
  const auto uv    = Transform(render_to_uv.m, x + 0.5f, y + 0.5f);
  float      value = SampleR8(source, source_width, source_height, uv.x, uv.y);
  if (invert) value = 1.0f - value;
  output[index] = static_cast<std::uint8_t>(fminf(fmaxf(value * 255.0f + 0.5f, 0.0f), 255.0f));
}

__global__ void GenerateR8MipKernel(const std::uint8_t* source, std::uint32_t source_width,
                                    std::uint32_t source_height, std::uint8_t* destination,
                                    std::uint32_t destination_width,
                                    std::uint32_t destination_height) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= destination_width * destination_height) return;
  const auto    x        = index % destination_width;
  const auto    y        = index / destination_width;
  const auto    source_x = x * 2;
  const auto    source_y = y * 2;
  std::uint32_t sum      = 0;
  std::uint32_t count    = 0;
  for (std::uint32_t dy = 0; dy < 2; ++dy) {
    for (std::uint32_t dx = 0; dx < 2; ++dx) {
      const auto sx = source_x + dx;
      const auto sy = source_y + dy;
      if (sx < source_width && sy < source_height) {
        sum += source[sy * source_width + sx];
        ++count;
      }
    }
  }
  destination[index] = static_cast<std::uint8_t>((sum + count / 2) / count);
}

void GenerateMipChain(MaskTextureLease<CudaBackend>& source, CudaCommandContext& context) {
  constexpr std::uint32_t block = 256;
  for (std::size_t level = 1; level < source.MipLevelCount(); ++level) {
    auto&      previous = source.Texture(level - 1);
    auto&      next     = source.Texture(level);
    const auto pixels   = next.Width() * next.Height();
    GenerateR8MipKernel<<<(pixels + block - 1) / block, block, 0, context.Stream()>>>(
        static_cast<const std::uint8_t*>(previous.DevicePointer()), previous.Width(),
        previous.Height(), static_cast<std::uint8_t*>(next.DevicePointer()), next.Width(),
        next.Height());
  }
}

__global__ void ParallelBandHorizontalKernel(const std::uint8_t* source, float* squared_distance,
                                             std::uint32_t width, std::uint32_t height,
                                             bool target_inside) {
  const auto y = blockIdx.x;
  if (y >= height || threadIdx.x != 0) return;
  constexpr int missing = -100000;
  int           nearest = missing;
  for (std::uint32_t x = 0; x < width; ++x) {
    const bool inside = source[y * width + x] >= 128;
    if (inside == target_inside) nearest = static_cast<int>(x);
    const float dx                  = static_cast<float>(static_cast<int>(x) - nearest);
    squared_distance[y * width + x] = nearest == missing ? 1.0e20f : dx * dx;
  }
  nearest = -missing;
  for (int x = static_cast<int>(width) - 1; x >= 0; --x) {
    const bool inside = source[y * width + x] >= 128;
    if (inside == target_inside) nearest = x;
    if (nearest != -missing) {
      const float dx                  = static_cast<float>(x - nearest);
      squared_distance[y * width + x] = fminf(squared_distance[y * width + x], dx * dx);
    }
  }
}

__global__ void ParallelBandVerticalKernel(const float* horizontal, float* squared_distance,
                                           std::uint32_t width, std::uint32_t height) {
  const auto x = blockIdx.x;
  if (x >= width || threadIdx.x != 0) return;
  extern __shared__ unsigned char shared[];
  auto*                           sites      = reinterpret_cast<int*>(shared);
  auto*                           boundaries = reinterpret_cast<float*>(sites + height);
  int                             count      = 0;
  for (std::uint32_t y = 0; y < height; ++y) {
    const float f = horizontal[y * width + x];
    if (f >= 1.0e19f) continue;
    float boundary = -1.0e20f;
    while (count > 0) {
      const int   previous   = sites[count - 1];
      const float previous_f = horizontal[previous * width + x];
      boundary               = ((f + static_cast<float>(y * y)) -
                  (previous_f + static_cast<float>(previous * previous))) /
                 (2.0f * (static_cast<float>(y) - previous));
      if (boundary > boundaries[count - 1]) break;
      --count;
    }
    sites[count]      = static_cast<int>(y);
    boundaries[count] = count == 0 ? -1.0e20f : boundary;
    ++count;
  }
  if (count == 0) {
    for (std::uint32_t y = 0; y < height; ++y) squared_distance[y * width + x] = 1.0e20f;
    return;
  }
  int site_index = 0;
  for (std::uint32_t y = 0; y < height; ++y) {
    while (site_index + 1 < count && boundaries[site_index + 1] < y) ++site_index;
    const int   site                = sites[site_index];
    const float dy                  = static_cast<float>(static_cast<int>(y) - site);
    squared_distance[y * width + x] = horizontal[site * width + x] + dy * dy;
  }
}

__global__ void ComposeSignedDistanceKernel(const std::uint8_t* source,
                                            const float*        distance_to_inside,
                                            const float* distance_to_outside, float* distance,
                                            std::uint32_t pixel_count) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  const float coverage = source[index] / 255.0f;
  const bool  inside   = coverage >= 0.5f;
  const float exact    = sqrtf(inside ? distance_to_outside[index] : distance_to_inside[index]);
  if (coverage > 0.0f && coverage < 1.0f) {
    distance[index] = coverage - 0.5f;
  } else {
    const float to_boundary = fmaxf(exact - 0.5f, 0.0f);
    distance[index]         = inside ? to_boundary : -to_boundary;
  }
}

__global__ void FeatherSampleKernel(const float* distance, std::uint32_t source_width,
                                    std::uint32_t source_height, std::uint8_t* output,
                                    std::uint32_t output_width, std::uint32_t output_height,
                                    Matrix3x3 render_to_uv, float radius_texels, bool invert) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= output_width * output_height) return;
  const auto  x     = index % output_width;
  const auto  y     = index / output_width;
  const auto  uv    = Transform(render_to_uv.m, x + 0.5f, y + 0.5f);
  const float d     = SampleF32(distance, source_width, source_height, uv.x, uv.y);
  float       value = radius_texels <= 0.0f ? (d >= 0.0f ? 1.0f : 0.0f)
                                            : fminf(fmaxf(0.5f + d / (2.0f * radius_texels), 0.0f), 1.0f);
  value             = value * value * (3.0f - 2.0f * value);
  if (invert) value = 1.0f - value;
  output[index] = static_cast<std::uint8_t>(value * 255.0f + 0.5f);
}

__global__ void AnalyticMaskKernel(std::uint8_t* output, std::uint32_t width, std::uint32_t height,
                                   Matrix3x3 render_to_reference, Extent2D reference_extent,
                                   AnalyticMaskKind kind, RadialMaskParams radial,
                                   LinearGradientMaskParams graduated) {
  const auto index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= width * height) return;
  const auto  x         = index % width;
  const auto  y         = index / width;
  const auto  reference = Transform(render_to_reference.m, x + 0.5f, y + 0.5f);
  const float nx        = reference.x / reference_extent.width;
  const float ny        = reference.y / reference_extent.height;
  float       value     = 0.0f;
  bool        invert    = false;
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
    value  = 1.0f - fminf(fmaxf((radius - inner) / fmaxf(outer - inner, 1.0e-6f), 0.0f), 1.0f);
    invert = radial.invert;
  } else {
    const float normal_length = hypotf(graduated.normal_x, graduated.normal_y);
    const float normal_x      = graduated.normal_x / fmaxf(normal_length, 1.0e-6f);
    const float normal_y      = graduated.normal_y / fmaxf(normal_length, 1.0e-6f);
    const float distance =
        (nx - graduated.origin_x) * normal_x + (ny - graduated.origin_y) * normal_y;
    const float t =
        fminf(fmaxf(distance / fmaxf(graduated.transition_distance, 1.0e-6f) + 0.5f, 0.0f), 1.0f);
    value  = graduated.start_value + (graduated.end_value - graduated.start_value) * t;
    invert = graduated.invert;
  }
  if (invert) value = 1.0f - value;
  output[index] = static_cast<std::uint8_t>(fminf(fmaxf(value * 255.0f + 0.5f, 0.0f), 255.0f));
}

}  // namespace

auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PipelineDocument& document, const CompiledGradeNode& compiled_grade,
                     MaskStore* store, std::span<const RectI> dirty_rectangles) -> CudaMaskResult {
  if (!device.Workspace().IsRendering())
    throw std::runtime_error("ExecuteCudaMask: BeginRender has not been called");
  if (!compiled_grade.mask.has_value()) {
    throw std::runtime_error("ExecuteCudaMask: compiled Color Grade has no mask");
  }
  auto&                   workspace     = device.Workspace();
  auto&                   context       = device.CommandContext();
  const auto              extent        = plan.geometry.render_extent;
  auto&                   output        = EnsureOutput(workspace, compiled_grade.mask_output, extent);
  constexpr std::uint32_t block         = 256;
  const auto              render_pixels = extent.width * extent.height;
  CudaMaskResult          result{compiled_grade.mask_output};

  const auto&             mask_model = RequireCompiledMaskModel(document, compiled_grade);
  if (std::holds_alternative<RadialMaskSource>(mask_model.source) ||
      std::holds_alternative<LinearGradientMaskSource>(mask_model.source)) {
    AnalyticMaskKernel<<<(render_pixels + block - 1) / block, block, 0, context.Stream()>>>(
        static_cast<std::uint8_t*>(output.Texture().DevicePointer()), extent.width, extent.height,
        plan.geometry.render_to_reference, plan.geometry.full_reference_extent,
        AnalyticKindFromMask(mask_model), RadialParamsFromMask(mask_model),
        LinearGradientParamsFromMask(mask_model));
  } else if (const auto* brush = std::get_if<BrushMaskSource>(&mask_model.source)) {
    if (store == nullptr)
      throw std::invalid_argument("ExecuteCudaMask: raster mask needs MaskStore");
    if (!brush->asset_key.has_value() || brush->asset_key->Empty()) {
      throw std::runtime_error("ExecuteCudaMask: Brush Mask has no asset");
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
      GenerateMipChain(source, context);
    } else if (has_dirty) {
      const auto bytes = CopyRectangle(*asset, dirty);
      workspace.Device().UploadR8TextureRect(source.Texture(), dirty, bytes, context);
      GenerateMipChain(source, context);
    }
    const auto sampling = MakeRasterMaskSamplingPlan(plan.geometry, brush->descriptor.reference_bounds,
                                                     asset->descriptor.extent);
    if (brush->feather_radius <= 0.0f) {
      const auto selected_level = std::min<std::size_t>(
          static_cast<std::size_t>(std::max(std::floor(sampling.mip_level), 0.0f)),
          source.MipLevelCount() - 1);
      auto& sampled_texture = source.Texture(selected_level);
      RasterSampleKernel<<<(render_pixels + block - 1) / block, block, 0, context.Stream()>>>(
          static_cast<const std::uint8_t*>(sampled_texture.DevicePointer()),
          sampled_texture.Width(), sampled_texture.Height(),
          static_cast<std::uint8_t*>(output.Texture().DevicePointer()), extent.width, extent.height,
          sampling.render_to_texture_uv, mask_model.invert);
    } else {
      const GraphValueId distance_id{compiled_grade.node_id, PortId{"signed_distance"}};
      const GraphValueId horizontal_id{compiled_grade.node_id, PortId{"distance.horizontal"}};
      const GraphValueId inside_id{compiled_grade.node_id, PortId{"distance.inside"}};
      const GraphValueId outside_id{compiled_grade.node_id, PortId{"distance.outside"}};
      auto*              distance       = workspace.Values().Find(distance_id);
      const auto         distance_bytes = asset->pixels.size() * sizeof(float);
      const bool         must_compute =
          distance == nullptr || distance->Bytes() != distance_bytes || !cached || has_dirty;
      if (distance == nullptr || distance->Bytes() != distance_bytes) {
        workspace.Values().Store(distance_id, workspace.Device().CreateBuffer(distance_bytes));
        distance = workspace.Values().Find(distance_id);
      }
      if (must_compute) {
        const auto source_pixels = asset->descriptor.extent.width * asset->descriptor.extent.height;
        auto*      horizontal    = workspace.Values().Find(horizontal_id);
        auto*      inside_distance  = workspace.Values().Find(inside_id);
        auto*      outside_distance = workspace.Values().Find(outside_id);
        if (horizontal == nullptr || horizontal->Bytes() != distance_bytes) {
          workspace.Values().Store(horizontal_id, workspace.Device().CreateBuffer(distance_bytes));
          horizontal = workspace.Values().Find(horizontal_id);
        }
        if (inside_distance == nullptr || inside_distance->Bytes() != distance_bytes) {
          workspace.Values().Store(inside_id, workspace.Device().CreateBuffer(distance_bytes));
          inside_distance = workspace.Values().Find(inside_id);
        }
        if (outside_distance == nullptr || outside_distance->Bytes() != distance_bytes) {
          workspace.Values().Store(outside_id, workspace.Device().CreateBuffer(distance_bytes));
          outside_distance = workspace.Values().Find(outside_id);
        }
        const auto shared_bytes = asset->descriptor.extent.height * sizeof(int) +
                                  asset->descriptor.extent.height * sizeof(float);
        ParallelBandHorizontalKernel<<<asset->descriptor.extent.height, 1, 0, context.Stream()>>>(
            static_cast<const std::uint8_t*>(source.Texture().DevicePointer()),
            static_cast<float*>(horizontal->DevicePointer()), asset->descriptor.extent.width,
            asset->descriptor.extent.height, true);
        ParallelBandVerticalKernel<<<asset->descriptor.extent.width, 1, shared_bytes,
                                     context.Stream()>>>(
            static_cast<const float*>(horizontal->DevicePointer()),
            static_cast<float*>(inside_distance->DevicePointer()), asset->descriptor.extent.width,
            asset->descriptor.extent.height);
        ParallelBandHorizontalKernel<<<asset->descriptor.extent.height, 1, 0, context.Stream()>>>(
            static_cast<const std::uint8_t*>(source.Texture().DevicePointer()),
            static_cast<float*>(horizontal->DevicePointer()), asset->descriptor.extent.width,
            asset->descriptor.extent.height, false);
        ParallelBandVerticalKernel<<<asset->descriptor.extent.width, 1, shared_bytes,
                                     context.Stream()>>>(
            static_cast<const float*>(horizontal->DevicePointer()),
            static_cast<float*>(outside_distance->DevicePointer()), asset->descriptor.extent.width,
            asset->descriptor.extent.height);
        ComposeSignedDistanceKernel<<<(source_pixels + block - 1) / block, block, 0,
                                      context.Stream()>>>(
            static_cast<const std::uint8_t*>(source.Texture().DevicePointer()),
            static_cast<const float*>(inside_distance->DevicePointer()),
            static_cast<const float*>(outside_distance->DevicePointer()),
            static_cast<float*>(distance->DevicePointer()), source_pixels);
      }
      result.signed_distance_resource_id = distance->ResourceId();
      const auto  bounds                 = brush->descriptor.reference_bounds;
      const float x_scale =
          asset->descriptor.extent.width /
          (plan.geometry.full_reference_extent.width * std::max(bounds.w, 1.0e-6f));
      const float y_scale =
          asset->descriptor.extent.height /
          (plan.geometry.full_reference_extent.height * std::max(bounds.h, 1.0e-6f));
      const float radius_texels = brush->feather_radius * 0.5f * (x_scale + y_scale);
      FeatherSampleKernel<<<(render_pixels + block - 1) / block, block, 0, context.Stream()>>>(
          static_cast<const float*>(distance->DevicePointer()), asset->descriptor.extent.width,
          asset->descriptor.extent.height,
          static_cast<std::uint8_t*>(output.Texture().DevicePointer()), extent.width, extent.height,
          sampling.render_to_texture_uv, radius_texels, mask_model.invert);
    }
  } else {
    throw std::runtime_error("ExecuteCudaMask: compiled mask does not match document");
  }
  if (::cudaGetLastError() != cudaSuccess)
    throw std::runtime_error("ExecuteCudaMask: CUDA kernel launch failed");
  return result;
}

auto ExecuteCudaMask(CudaRenderDevice& device, const ExecutionPlan& plan,
                     const PipelineDocument& document, MaskStore* store,
                     std::span<const RectI> dirty_rectangles) -> CudaMaskResult {
  const CompiledGradeNode* last = nullptr;
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask.has_value()) {
      last = &grade;
    }
  }
  if (last == nullptr) {
    throw std::runtime_error("ExecuteCudaMask: plan has no mask");
  }
  CudaMaskResult result{};
  for (const auto& grade : plan.grade_nodes) {
    if (grade.mask.has_value()) {
      result = ExecuteCudaMask(device, plan, document, grade, store, dirty_rectangles);
    }
  }
  return result;
}

}  // namespace alcedo
