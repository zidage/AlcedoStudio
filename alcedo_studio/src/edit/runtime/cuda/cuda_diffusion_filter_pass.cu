//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_diffusion_filter_pass.hpp"

#include <cuda_runtime.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

#include "cuda/cuda_check.hpp"
#include "cuda_acescc.cuh"
#include "edit/graph/diffusion_filter_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"

namespace alcedo {
namespace {

constexpr unsigned kBlock = 16;

auto GridFor(std::uint32_t width, std::uint32_t height) -> dim3 {
  return dim3{(width + kBlock - 1) / kBlock, (height + kBlock - 1) / kBlock};
}

__device__ __forceinline__ auto DecodeAcescc(float4 value) -> float4 {
  return make_float4(cuda_acescc::Decode(value.x), cuda_acescc::Decode(value.y),
                     cuda_acescc::Decode(value.z), value.w);
}

__device__ __forceinline__ auto Add(float4 a, float4 b) -> float4 {
  return make_float4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
}

__device__ __forceinline__ auto Scale(float4 a, float s) -> float4 {
  return make_float4(a.x * s, a.y * s, a.z * s, a.w * s);
}

__device__ __forceinline__ auto Fetch(const float4* image, int width, int height, int x, int y)
    -> float4 {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  return image[y * width + x];
}

__device__ __forceinline__ auto Transform(const Matrix3x3& matrix, float x, float y) -> float2 {
  return make_float2(matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
                     matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]);
}

/// Bilinear sample with clamp-to-edge addressing. Texel `i` covers [i, i + 1).
__device__ __forceinline__ auto SampleBilinear(const float4* image, int width, int height, float px,
                                               float py) -> float4 {
  const float fx = px - 0.5f;
  const float fy = py - 0.5f;
  const float x0 = floorf(fx);
  const float y0 = floorf(fy);
  const float ax = fx - x0;
  const float ay = fy - y0;
  const int   ix = static_cast<int>(x0);
  const int   iy = static_cast<int>(y0);
  const auto  a  = Fetch(image, width, height, ix, iy);
  const auto  b  = Fetch(image, width, height, ix + 1, iy);
  const auto  c  = Fetch(image, width, height, ix, iy + 1);
  const auto  d  = Fetch(image, width, height, ix + 1, iy + 1);
  const auto  top    = Add(Scale(a, 1.0f - ax), Scale(b, ax));
  const auto  bottom = Add(Scale(c, 1.0f - ax), Scale(d, ax));
  return Add(Scale(top, 1.0f - ay), Scale(bottom, ay));
}

/// Bilinear sample of the ACEScc scene, decoded to linear AP1 per tap before interpolation.
__device__ auto SampleBilinearDecoded(const float4* image, int width, int height, float px,
                                      float py) -> float4 {
  const float fx = px - 0.5f;
  const float fy = py - 0.5f;
  const float x0 = floorf(fx);
  const float y0 = floorf(fy);
  const float ax = fx - x0;
  const float ay = fy - y0;
  const int   ix = static_cast<int>(x0);
  const int   iy = static_cast<int>(y0);
  const auto  a  = DecodeAcescc(Fetch(image, width, height, ix, iy));
  const auto  b  = DecodeAcescc(Fetch(image, width, height, ix + 1, iy));
  const auto  c  = DecodeAcescc(Fetch(image, width, height, ix, iy + 1));
  const auto  d  = DecodeAcescc(Fetch(image, width, height, ix + 1, iy + 1));
  const auto  top    = Add(Scale(a, 1.0f - ax), Scale(b, ax));
  const auto  bottom = Add(Scale(c, 1.0f - ax), Scale(d, ax));
  return Add(Scale(top, 1.0f - ay), Scale(bottom, ay));
}

__device__ __forceinline__ void BSplineWeights(float t, float weights[4]) {
  const float t2 = t * t;
  const float t3 = t2 * t;
  const float u  = 1.0f - t;
  weights[0]     = u * u * u / 6.0f;
  weights[1]     = (3.0f * t3 - 6.0f * t2 + 4.0f) / 6.0f;
  weights[2]     = (-3.0f * t3 + 3.0f * t2 + 3.0f * t + 1.0f) / 6.0f;
  weights[3]     = t3 / 6.0f;
}

/// Cubic B-spline sample; smooth magnification of the coarse scatter image.
__device__ auto SampleBSpline(const float4* image, int width, int height, float px, float py)
    -> float4 {
  const float fx = px - 0.5f;
  const float fy = py - 0.5f;
  const float x0 = floorf(fx);
  const float y0 = floorf(fy);
  float       wx[4];
  float       wy[4];
  BSplineWeights(fx - x0, wx);
  BSplineWeights(fy - y0, wy);
  const int ix  = static_cast<int>(x0) - 1;
  const int iy  = static_cast<int>(y0) - 1;
  float4    sum = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
  for (int j = 0; j < 4; ++j) {
    float4 row = make_float4(0.0f, 0.0f, 0.0f, 0.0f);
    for (int i = 0; i < 4; ++i) {
      row = Add(row, Scale(Fetch(image, width, height, ix + i, iy + j), wx[i]));
    }
    sum = Add(sum, Scale(row, wy[j]));
  }
  return sum;
}

/// Linear AP1 with the near-clip highlight boost. Negative (out-of-gamut) light does not scatter.
__device__ __forceinline__ auto BoostHighlights(float4 linear, float gain, float knee) -> float3 {
  const float r    = fmaxf(linear.x, 0.0f);
  const float g    = fmaxf(linear.y, 0.0f);
  const float b    = fmaxf(linear.z, 0.0f);
  const float peak = fmaxf(r, fmaxf(g, b));
  const float t    = fminf(fmaxf((peak - knee) / (1.0f - knee), 0.0f), 1.0f);
  const float lift = 1.0f + gain * t * t * (3.0f - 2.0f * t);
  return make_float3(r * lift, g * lift, b * lift);
}

__global__ void DecodeKernel(const float4* src, float4* dst, std::uint32_t pixel_count) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixel_count) return;
  dst[index] = DecodeAcescc(src[index]);
}

/// Base level: average of @p samples x @p samples decoded, boosted render samples inside the
/// footprint of one base texel. @p base_to_render maps base texel coordinates to the render.
__global__ void ReduceBoostKernel(const float4* src, int src_width, int src_height, float4* dst,
                                  int dst_width, int dst_height, Matrix3x3 base_to_render,
                                  int samples, float gain, float knee) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= dst_width || y >= dst_height) return;
  const float step = 1.0f / static_cast<float>(samples);
  float3      sum  = make_float3(0.0f, 0.0f, 0.0f);
  for (int j = 0; j < samples; ++j) {
    const float by = static_cast<float>(y) + (static_cast<float>(j) + 0.5f) * step;
    for (int i = 0; i < samples; ++i) {
      const float  bx      = static_cast<float>(x) + (static_cast<float>(i) + 0.5f) * step;
      const float2 render  = Transform(base_to_render, bx, by);
      const auto   linear  = SampleBilinearDecoded(src, src_width, src_height, render.x, render.y);
      const auto   boosted = BoostHighlights(linear, gain, knee);
      sum.x += boosted.x;
      sum.y += boosted.y;
      sum.z += boosted.z;
    }
  }
  const float inv        = step * step;
  dst[y * dst_width + x] = make_float4(sum.x * inv, sum.y * inv, sum.z * inv, 1.0f);
}

/// 13-tap downsample (Jimenez, SIGGRAPH 2014). Destination texel `x` spans source [2x, 2x + 2).
__global__ void DownsampleKernel(const float4* src, int src_width, int src_height, float4* dst,
                                 int dst_width, int dst_height) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= dst_width || y >= dst_height) return;
  const float cx = 2.0f * (static_cast<float>(x) + 0.5f);
  const float cy = 2.0f * (static_cast<float>(y) + 0.5f);
  auto        s  = [&](float ox, float oy) {
    return SampleBilinear(src, src_width, src_height, cx + ox, cy + oy);
  };
  const auto center  = s(0.0f, 0.0f);
  const auto corners = Add(Add(s(-2.0f, -2.0f), s(2.0f, -2.0f)), Add(s(-2.0f, 2.0f), s(2.0f, 2.0f)));
  const auto edges   = Add(Add(s(0.0f, -2.0f), s(-2.0f, 0.0f)), Add(s(2.0f, 0.0f), s(0.0f, 2.0f)));
  const auto inner   = Add(Add(s(-1.0f, -1.0f), s(1.0f, -1.0f)), Add(s(-1.0f, 1.0f), s(1.0f, 1.0f)));
  dst[y * dst_width + x] = Add(Add(Scale(center, 0.125f), Scale(corners, 0.03125f)),
                               Add(Scale(edges, 0.0625f), Scale(inner, 0.125f)));
}

/// `dst = level_weight * level + coarse_weight * tent_upsample(coarse)` at the level extent.
__global__ void UpsampleAccumulateKernel(const float4* coarse, int coarse_width, int coarse_height,
                                         float coarse_weight, const float4* level, float4* dst,
                                         int width, int height, float level_weight) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const float cx = (static_cast<float>(x) + 0.5f) * 0.5f;
  const float cy = (static_cast<float>(y) + 0.5f) * 0.5f;
  auto        s  = [&](float ox, float oy) {
    return SampleBilinear(coarse, coarse_width, coarse_height, cx + ox, cy + oy);
  };
  const auto center  = s(0.0f, 0.0f);
  const auto edges   = Add(Add(s(-1.0f, 0.0f), s(1.0f, 0.0f)), Add(s(0.0f, -1.0f), s(0.0f, 1.0f)));
  const auto corners = Add(Add(s(-1.0f, -1.0f), s(1.0f, -1.0f)), Add(s(-1.0f, 1.0f), s(1.0f, 1.0f)));
  const auto tent =
      Scale(Add(Add(Scale(center, 4.0f), Scale(edges, 2.0f)), corners), 1.0f / 16.0f);
  const int index = y * width + x;
  dst[index]      = Add(Scale(level[index], level_weight), Scale(tent, coarse_weight));
}

/// out = T * ((1 - s) * I + s * B); B is the scatter image at the reference position of the
/// render pixel, so every render of the same frame reads the same glow.
__global__ void MixKernel(const float4* src, float4* dst, int width, int height,
                          const float4* scatter, int scatter_width, int scatter_height,
                          Matrix3x3 render_to_base, float scatter_fraction, float transmission) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const int    index  = y * width + x;
  const auto   linear = DecodeAcescc(src[index]);
  const float2 base =
      Transform(render_to_base, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
  const auto  glow   = SampleBSpline(scatter, scatter_width, scatter_height, base.x, base.y);
  const float direct = 1.0f - scatter_fraction;
  dst[index] = make_float4(transmission * (direct * linear.x + scatter_fraction * glow.x),
                           transmission * (direct * linear.y + scatter_fraction * glow.y),
                           transmission * (direct * linear.z + scatter_fraction * glow.z),
                           linear.w);
}

auto Pointer(ResourceLease<CudaBackend>& lease) -> float4* {
  return static_cast<float4*>(lease.Texture().DevicePointer());
}

/**
 * @brief Build the scatter image of @p layout from the render @p src into @p target.
 *
 * @p target has the base-level extent. Pyramid levels are pooled scratch leases, returned at
 * scope exit; later users run on the same stream, so these kernels finish before any reuse.
 */
void BuildScatter(CudaRenderWorkspace& workspace, cudaStream_t stream, const float4* src,
                  std::uint32_t width, std::uint32_t height, const DiffusionFilterLayout& layout,
                  const DiffusionScatterMapping& mapping, float4* target) {
  const auto                              count = layout.level_count;
  std::vector<ResourceLease<CudaBackend>> leases;
  leases.reserve(2U * count);
  auto acquire = [&](ImageExtent extent) {
    leases.push_back(
        workspace.Textures().Acquire({extent.width, extent.height, TextureFormat::Rgba32f}));
    return Pointer(leases.back());
  };
  // With one level the reduction is the scatter image. Otherwise the level-0 accumulation is.
  std::vector<float4*> levels(count, nullptr);
  std::vector<float4*> accumulated(count, nullptr);
  for (std::uint32_t index = 0; index < count; ++index) {
    levels[index] = count == 1 ? target : acquire(layout.extents[index]);
    if (index + 1 < count) {
      accumulated[index] = index == 0 ? target : acquire(layout.extents[index]);
    }
  }

  const dim3 block{kBlock, kBlock};
  const auto base = layout.extents[0];
  ReduceBoostKernel<<<GridFor(base.width, base.height), block, 0, stream>>>(
      src, static_cast<int>(width), static_cast<int>(height), levels[0],
      static_cast<int>(base.width), static_cast<int>(base.height), mapping.base_to_render,
      static_cast<int>(mapping.reduce_samples), layout.highlight_gain, layout.highlight_knee);
  for (std::uint32_t index = 1; index < count; ++index) {
    const auto from = layout.extents[index - 1];
    const auto to   = layout.extents[index];
    DownsampleKernel<<<GridFor(to.width, to.height), block, 0, stream>>>(
        levels[index - 1], static_cast<int>(from.width), static_cast<int>(from.height),
        levels[index], static_cast<int>(to.width), static_cast<int>(to.height));
  }

  // Accumulate from the coarsest level. The coarsest level enters with its own weight.
  const float4* coarse        = levels[count - 1];
  float         coarse_weight = layout.weights[count - 1];
  for (std::uint32_t index = count - 1; index-- > 0;) {
    const auto coarse_extent = layout.extents[index + 1];
    const auto extent        = layout.extents[index];
    UpsampleAccumulateKernel<<<GridFor(extent.width, extent.height), block, 0, stream>>>(
        coarse, static_cast<int>(coarse_extent.width), static_cast<int>(coarse_extent.height),
        coarse_weight, levels[index], accumulated[index], static_cast<int>(extent.width),
        static_cast<int>(extent.height), layout.weights[index]);
    coarse        = accumulated[index];
    coarse_weight = 1.0f;
  }
  cuda::CheckCuda(::cudaGetLastError(), "ExecuteCudaDiffusionFilter: scatter launch");
}

}  // namespace

auto ExecuteCudaDiffusionFilter(CudaRenderDevice& device, const ExecutionPlan& plan,
                                const PipelineDocument& document, const FrameSceneBinding& scene)
    -> FrameSceneBinding {
  auto& workspace = device.Workspace();
  if (!workspace.IsRendering()) {
    throw std::runtime_error("ExecuteCudaDiffusionFilter: BeginRender has not been called");
  }
  const auto* drt = document.Drt();
  if (drt == nullptr) {
    throw std::runtime_error("ExecuteCudaDiffusionFilter: missing DRT endpoint");
  }
  const auto output  = FrameSceneBinding::WorkImage(DestinationWorkMember(scene));
  auto&      src_tex = CudaSceneTexture(device, scene);
  auto&      dst_tex = CudaSceneTexture(device, output);
  const auto width   = src_tex.Width();
  const auto height  = src_tex.Height();
  if (dst_tex.Width() != width || dst_tex.Height() != height) {
    throw std::runtime_error("ExecuteCudaDiffusionFilter: scene-work extent does not match scene");
  }
  const auto* src    = static_cast<const float4*>(src_tex.DevicePointer());
  auto*       dst    = static_cast<float4*>(dst_tex.DevicePointer());
  auto        stream = device.CommandContext().Stream();

  const float strength = drt->Params().DiffusionStrength();
  if (!IsDiffusionFilterActive(strength)) {
    const std::uint32_t pixels = width * height;
    DecodeKernel<<<(pixels + 255U) / 256U, 256U, 0, stream>>>(src, dst, pixels);
    cuda::CheckCuda(::cudaGetLastError(), "ExecuteCudaDiffusionFilter: decode launch");
    return output;
  }

  const auto& geometry = plan.geometry;
  const auto  layout   = MakeDiffusionFilterLayout(
      DiffusionCanvasExtent(geometry.full_reference_extent), ResolveDiffusionFilterShape(strength));
  const auto mapping = MakeDiffusionScatterMapping(geometry, layout);
  const auto base    = layout.extents[0];

  // The scatter image covers the full frame. A full-edit render builds and publishes it; an
  // ROI render samples the published image, so light outside the viewport still scatters in.
  auto&      invalidation = workspace.ResultInvalidation();
  const auto scatter_id   = DiffusionScatterId(plan.drt.node_id);
  const bool persist      = workspace.PersistsResult(scatter_id);
  const bool full_edit    = CoversFullEditSpace(geometry);
  const auto long_edge    = (std::max)(width, height);
  ResourceLease<CudaBackend>* canonical = nullptr;
  if (persist) {
    const auto needed = invalidation.MakeImageRepresentation(
        scatter_id, base, TextureFormat::Rgba32f,
        DiffusionScatterRequiredDetail(full_edit, long_edge));
    canonical = workspace.Images().BindValidResult(
        scatter_id, invalidation.RequiredRevision(scatter_id), needed,
        workspace.Device().CompletedSubmission());
  } else {
    ++device.PassStats().result_policy_bypass;
  }
  const auto decision = DecideDiffusionScatter(persist, canonical != nullptr, full_edit, long_edge);

  const float4*                             scatter = nullptr;
  std::optional<ResourceLease<CudaBackend>> transient_scatter;
  if (decision.action == DiffusionScatterAction::SampleCanonical) {
    scatter = Pointer(*canonical);
    ++device.PassStats().diffusion_scatter_sample;
  } else {
    float4* target = nullptr;
    if (decision.persist_canonical) {
      target = Pointer(workspace.AcquireImageForWrite(
          scatter_id, {base.width, base.height, TextureFormat::Rgba32f}));
    } else {
      transient_scatter.emplace(
          workspace.Textures().Acquire({base.width, base.height, TextureFormat::Rgba32f}));
      target = Pointer(*transient_scatter);
    }
    BuildScatter(workspace, stream, src, width, height, layout, mapping, target);
    if (decision.persist_canonical) {
      const auto published = invalidation.MakeImageRepresentation(
          scatter_id, base, TextureFormat::Rgba32f, decision.current_long_edge);
      workspace.Images().RecordUnpublished(scatter_id, invalidation.RequiredRevision(scatter_id),
                                           published, device.CommandContext().SubmissionId(),
                                           decision.current_long_edge);
    }
    scatter = target;
    ++device.PassStats().diffusion_scatter_rebuild;
  }

  const dim3 block{kBlock, kBlock};
  MixKernel<<<GridFor(width, height), block, 0, stream>>>(
      src, dst, static_cast<int>(width), static_cast<int>(height), scatter,
      static_cast<int>(base.width), static_cast<int>(base.height), mapping.render_to_base,
      layout.scatter_fraction, layout.transmission);
  cuda::CheckCuda(::cudaGetLastError(), "ExecuteCudaDiffusionFilter: mix launch");
  return output;
}

}  // namespace alcedo
