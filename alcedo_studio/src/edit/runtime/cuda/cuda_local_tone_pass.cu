//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cuda_acescc.cuh"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/cuda/cuda_local_tone_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/local_tone_cache_ids.hpp"
#include "edit/runtime/local_tone_executor.hpp"
#include "edit/runtime/local_tone_plan.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

using local_tone_mapping::LlfSample;

__device__ auto Ap1Intensity(const float4& pixel) -> float {
  return 0.272229f * pixel.x + 0.674082f * pixel.y + 0.053689f * pixel.z;
}

__device__ auto LogIntensity(const float4& acescc) -> float {
  const float4 linear = make_float4(cuda_acescc::Decode(acescc.x), cuda_acescc::Decode(acescc.y),
                                    cuda_acescc::Decode(acescc.z), acescc.w);
  return cuda_acescc::Encode(fmaxf(Ap1Intensity(linear), 1.0e-6f));
}

__device__ auto ReadRgbaBilinear(const float4* input, int width, int height, float x, float y)
    -> float4 {
  x               = fminf(fmaxf(x, 0.0f), width - 1.0f);
  y               = fminf(fmaxf(y, 0.0f), height - 1.0f);
  const int    x0 = static_cast<int>(floorf(x));
  const int    y0 = static_cast<int>(floorf(y));
  const int    x1 = min(x0 + 1, width - 1);
  const int    y1 = min(y0 + 1, height - 1);
  const float  tx = x - x0;
  const float  ty = y - y0;
  const float4 a  = input[y0 * width + x0];
  const float4 b  = input[y0 * width + x1];
  const float4 c  = input[y1 * width + x0];
  const float4 d  = input[y1 * width + x1];
  const float4 ab = make_float4(a.x + (b.x - a.x) * tx, a.y + (b.y - a.y) * tx,
                                a.z + (b.z - a.z) * tx, a.w + (b.w - a.w) * tx);
  const float4 cd = make_float4(c.x + (d.x - c.x) * tx, c.y + (d.y - c.y) * tx,
                                c.z + (d.z - c.z) * tx, c.w + (d.w - c.w) * tx);
  return make_float4(ab.x + (cd.x - ab.x) * ty, ab.y + (cd.y - ab.y) * ty,
                     ab.z + (cd.z - ab.z) * ty, ab.w + (cd.w - ab.w) * ty);
}

__device__ auto PlaneRead(const float* src, int x, int y, int width, int height) -> float {
  x = min(max(x, 0), width - 1);
  y = min(max(y, 0), height - 1);
  return src[static_cast<std::size_t>(y) * width + x];
}

__device__ auto Weight(int tap) -> float {
  return (tap == -2 || tap == 2)   ? 1.0f / 16.0f
         : (tap == -1 || tap == 1) ? 4.0f / 16.0f
                                   : 6.0f / 16.0f;
}

__device__ auto Expand(const float* coarse, int coarse_width, int coarse_height, int x, int y)
    -> float {
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    const int sample_y = y - ky;
    if ((sample_y & 1) != 0) continue;
    const int cy = min(max(sample_y / 2, 0), coarse_height - 1);
    for (int kx = -2; kx <= 2; ++kx) {
      const int sample_x = x - kx;
      if ((sample_x & 1) != 0) continue;
      const int cx = min(max(sample_x / 2, 0), coarse_width - 1);
      sum += 4.0f * Weight(kx) * Weight(ky) * coarse[cy * coarse_width + cx];
    }
  }
  return sum;
}

__device__ auto RemapDelta(float delta, float sigma, float alpha, float beta) -> float {
  const float magnitude = fabsf(delta);
  if (magnitude <= 1.0e-6f) return 0.0f;
  const float sign = copysignf(1.0f, delta);
  if (magnitude <= sigma) {
    return sign * sigma * powf(fminf(magnitude / fmaxf(sigma, 1.0e-6f), 1.0f), alpha);
  }
  return sign * (sigma + beta * (magnitude - sigma));
}

__device__ auto Transform(const float* matrix, float x, float y) -> float2 {
  return make_float2(matrix[0] * x + matrix[1] * y + matrix[2],
                     matrix[3] * x + matrix[4] * y + matrix[5]);
}

__global__ void ExtractKernel(const float4* input, float* output, int input_width, int input_height,
                              int output_width, int output_height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output_width || y >= output_height) return;
  const float sx = (x + 0.5f) * input_width / static_cast<float>(output_width) - 0.5f;
  const float sy = (y + 0.5f) * input_height / static_cast<float>(output_height) - 0.5f;
  output[y * output_width + x] =
      LogIntensity(ReadRgbaBilinear(input, input_width, input_height, sx, sy));
}

__global__ void ExtractReferenceKernel(const float4* input, float* output, int input_width,
                                       int input_height, int output_width, int output_height,
                                       Matrix3x3 reference_to_render, float full_ref_w,
                                       float full_ref_h) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output_width || y >= output_height) return;
  const float  u   = (x + 0.5f) / static_cast<float>(output_width);
  const float  v   = (y + 0.5f) / static_cast<float>(output_height);
  const float2 src = Transform(reference_to_render.m, u * full_ref_w, v * full_ref_h);
  output[y * output_width + x] =
      LogIntensity(ReadRgbaBilinear(input, input_width, input_height, src.x - 0.5f, src.y - 0.5f));
}

__global__ void DownKernel(const float* input, float* output, int input_width, int input_height,
                           int output_width, int output_height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output_width || y >= output_height) return;
  float sum = 0.0f;
  for (int ky = -2; ky <= 2; ++ky) {
    for (int kx = -2; kx <= 2; ++kx) {
      sum += Weight(kx) * Weight(ky) *
             PlaneRead(input, x * 2 + kx, y * 2 + ky, input_width, input_height);
    }
  }
  output[y * output_width + x] = sum;
}

__global__ void RemapKernel(const float* input, float* output, int width, int height,
                            LlfSample sample, float sigma) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  const int index = y * width + x;
  output[index] =
      sample.target + RemapDelta(input[index] - sample.gamma, sigma, sample.alpha, sample.beta);
}

__global__ void SelectKernel(const float* source, const float* lo, const float* lo_coarse,
                             const float* hi, const float* hi_coarse, float* output, int width,
                             int height, int coarse_width, int coarse_height, float gamma_lo,
                             float gamma_hi, bool first, bool last, bool top) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  const int   index = y * width + x;
  const float value = source[index];
  if (!((first && value <= gamma_hi) || (last && value >= gamma_lo) ||
        (value >= gamma_lo && value < gamma_hi)))
    return;
  const float t =
      fminf(fmaxf((value - gamma_lo) / fmaxf(gamma_hi - gamma_lo, 1.0e-6f), 0.0f), 1.0f);
  if (top) {
    output[index] = lo[index] + (hi[index] - lo[index]) * t;
    return;
  }
  const float lap_lo = lo[index] - Expand(lo_coarse, coarse_width, coarse_height, x, y);
  const float lap_hi = hi[index] - Expand(hi_coarse, coarse_width, coarse_height, x, y);
  output[index]      = lap_lo + (lap_hi - lap_lo) * t;
}

__global__ void CollapseKernel(const float* lap, const float* coarse, float* output, int width,
                               int height, int coarse_width, int coarse_height) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  const int index = y * width + x;
  output[index]   = lap[index] + Expand(coarse, coarse_width, coarse_height, x, y);
}

__device__ auto Bilinear(const float* plane, int width, int height, float x, float y) -> float {
  x              = fminf(fmaxf(x, 0.0f), width - 1.0f);
  y              = fminf(fmaxf(y, 0.0f), height - 1.0f);
  const int   x0 = static_cast<int>(floorf(x));
  const int   y0 = static_cast<int>(floorf(y));
  const int   x1 = min(x0 + 1, width - 1);
  const int   y1 = min(y0 + 1, height - 1);
  const float tx = x - x0;
  const float ty = y - y0;
  const float a  = plane[y0 * width + x0] + (plane[y0 * width + x1] - plane[y0 * width + x0]) * tx;
  const float b  = plane[y1 * width + x0] + (plane[y1 * width + x1] - plane[y1 * width + x0]) * tx;
  return a + (b - a) * ty;
}

__global__ void ApplyKernel(const float4* input, const float* reference, const float* adjusted,
                            float4* output, int width, int height, int adjusted_width,
                            int adjusted_height, Matrix3x3 render_to_uv) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) return;
  const int       index       = y * width + x;
  const float2    uv          = Transform(render_to_uv.m, x + 0.5f, y + 0.5f);
  const float     ax          = uv.x * adjusted_width - 0.5f;
  const float     ay          = uv.y * adjusted_height - 0.5f;
  const float     reference_l = Bilinear(reference, adjusted_width, adjusted_height, ax, ay);
  const float     adjusted_l  = Bilinear(adjusted, adjusted_width, adjusted_height, ax, ay);
  const float4    pixel       = input[index];
  const float     source_l    = LogIntensity(pixel);
  const float     source_intensity = fmaxf(cuda_acescc::Decode(source_l), 1.0e-5f);
  const float     target_intensity = cuda_acescc::Decode(source_l + adjusted_l - reference_l);
  const float     ratio            = fminf(fmaxf(target_intensity / source_intensity, 0.0f), 32.0f);
  float           r                = cuda_acescc::Decode(pixel.x) * ratio;
  float           g                = cuda_acescc::Decode(pixel.y) * ratio;
  float           b                = cuda_acescc::Decode(pixel.z) * ratio;
  constexpr float kLower           = -1.0e-5f;
  float           gamut_scale      = 1.0f;
  if (r < kLower && target_intensity > r) {
    gamut_scale = fminf(gamut_scale, (target_intensity - kLower) / (target_intensity - r));
  }
  if (g < kLower && target_intensity > g) {
    gamut_scale = fminf(gamut_scale, (target_intensity - kLower) / (target_intensity - g));
  }
  if (b < kLower && target_intensity > b) {
    gamut_scale = fminf(gamut_scale, (target_intensity - kLower) / (target_intensity - b));
  }
  gamut_scale = fminf(fmaxf(gamut_scale, 0.0f), 1.0f);
  r           = target_intensity + (r - target_intensity) * gamut_scale;
  g           = target_intensity + (g - target_intensity) * gamut_scale;
  b           = target_intensity + (b - target_intensity) * gamut_scale;
  output[index] =
      make_float4(cuda_acescc::Encode(r), cuda_acescc::Encode(g), cuda_acescc::Encode(b), pixel.w);
}

auto Grid(int width, int height, dim3 block) -> dim3 {
  return {static_cast<unsigned>((width + block.x - 1) / block.x),
          static_cast<unsigned>((height + block.y - 1) / block.y), 1};
}

void CheckLaunch(const char* operation) {
  if (::cudaGetLastError() != cudaSuccess) {
    throw std::runtime_error(std::string{"CUDA local tone: "} + operation + " failed");
  }
}

struct CudaTonePlane {
  float*      ptr   = nullptr;
  std::size_t bytes = 0;
};

auto CanonicalNeeded(RuntimeInvalidationState& invalidation, const GraphValueId& id,
                     const ResolvedRenderGeometry& geometry, int current_long_edge)
    -> ResultRepresentation {
  const auto canonical = local_tone_mapping::ComputeMaskDimensions(
      static_cast<int>(geometry.full_reference_extent.width),
      static_cast<int>(geometry.full_reference_extent.height),
      local_tone_mapping::kReferenceMaskMaxLongEdge);
  const ImageExtent extent{static_cast<std::uint32_t>(canonical.width),
                           static_cast<std::uint32_t>(canonical.height)};
  return invalidation.MakeImageRepresentation(id, extent, TextureFormat::R32f,
                                              static_cast<std::uint32_t>(current_long_edge));
}

auto BindCanonicalImage(CudaRenderDevice& device, const GraphValueId& id,
                        const ResultRepresentation& needed) -> ResourceLease<CudaBackend>* {
  return device.Workspace().Images().BindValidResult(
      id, device.Workspace().ResultInvalidation().RequiredRevision(id), needed,
      device.Workspace().Device().CompletedSubmission());
}

void CopyPlaneToTexture(CudaRenderDevice& device, const CudaTonePlane& plane,
                        CudaBackend::Texture2D& texture) {
  if (plane.ptr == nullptr || texture.DevicePointer() == nullptr || plane.bytes == 0) {
    throw std::runtime_error("ExecuteCudaLocalTone: cannot copy empty LLF plane");
  }
  if (::cudaMemcpyAsync(texture.DevicePointer(), plane.ptr, plane.bytes, cudaMemcpyDeviceToDevice,
                        device.CommandContext().Stream()) != cudaSuccess) {
    throw std::runtime_error("ExecuteCudaLocalTone: canonical plane copy failed");
  }
}

struct CudaLocalToneOps {
  using Device       = CudaRenderDevice;
  using Texture      = CudaBackend::Texture2D;
  using ScratchPlane = CudaTonePlane;

  static constexpr const char* kErrorPrefix = "ExecuteCudaLocalTone";

  static auto TextureWidth(const Texture& texture) -> std::uint32_t { return texture.Width(); }
  static auto TextureHeight(const Texture& texture) -> std::uint32_t { return texture.Height(); }
  static auto TransientBytes(CudaRenderDevice& device) -> std::size_t {
    return device.Workspace().TransientBuffers().used_bytes();
  }

  static auto LookupCanonical(CudaRenderDevice& device, const GraphValueId& source_id,
                              const GraphValueId& result_id, int current_long_edge,
                              const ResolvedRenderGeometry& geometry) -> LocalToneCanonicalLookup {
    auto& invalidation = device.Workspace().ResultInvalidation();
    const auto source_needed =
        CanonicalNeeded(invalidation, source_id, geometry, current_long_edge);
    const auto result_needed =
        CanonicalNeeded(invalidation, result_id, geometry, current_long_edge);
    auto* source = BindCanonicalImage(device, source_id, source_needed);
    LocalToneCanonicalLookup lookup;
    if (source == nullptr) {
      return lookup;
    }
    const auto long_edge = device.Workspace().Images().PublishedAuxiliary(source_id);
    lookup.source_valid     = long_edge > 0;
    lookup.source_long_edge = static_cast<int>(long_edge);
    lookup.extent           = source_needed.extent;
    lookup.result_valid     = lookup.source_valid &&
                          BindCanonicalImage(device, result_id, result_needed) != nullptr;
    return lookup;
  }

  static void ApplyCanonicalSample(CudaRenderDevice& device, const Texture& input, Texture& output,
                                   const GraphValueId& source_id, const GraphValueId& result_id,
                                   const LocalToneDecision& decision, std::uint32_t width,
                                   std::uint32_t height) {
    auto& invalidation = device.Workspace().ResultInvalidation();
    const auto needed = invalidation.MakeImageRepresentation(
        source_id, decision.mask_extent, TextureFormat::R32f,
        static_cast<std::uint32_t>(decision.current_long_edge));
    const auto result_needed = invalidation.MakeImageRepresentation(
        result_id, decision.mask_extent, TextureFormat::R32f,
        static_cast<std::uint32_t>(decision.current_long_edge));
    auto* source = BindCanonicalImage(device, source_id, needed);
    auto* result = BindCanonicalImage(device, result_id, result_needed);
    if (source == nullptr || result == nullptr) {
      throw std::runtime_error("ExecuteCudaLocalTone: canonical sample lost published planes");
    }
    const dim3 block{16, 16, 1};
    ApplyKernel<<<Grid(static_cast<int>(width), static_cast<int>(height), block), block, 0,
                  device.CommandContext().Stream()>>>(
        static_cast<const float4*>(input.DevicePointer()),
        static_cast<const float*>(source->Texture().DevicePointer()),
        static_cast<const float*>(result->Texture().DevicePointer()),
        static_cast<float4*>(output.DevicePointer()), static_cast<int>(width),
        static_cast<int>(height), static_cast<int>(decision.mask_extent.width),
        static_cast<int>(decision.mask_extent.height), decision.apply_uv);
    CheckLaunch("canonical sample");
  }

  static auto CanonicalResourceId(CudaRenderDevice& device, const GraphValueId& source_id)
      -> std::uint64_t {
    auto* image = device.Workspace().Images().Find(source_id);
    return image == nullptr ? 0 : image->Texture().ResourceId();
  }

  static auto BindCanonicalSourcePlane(CudaRenderDevice& device, const GraphValueId& source_id,
                                       std::size_t bytes) -> CudaTonePlane {
    auto* image = device.Workspace().Images().Find(source_id);
    if (image == nullptr || image->Texture().DevicePointer() == nullptr) {
      throw std::runtime_error("ExecuteCudaLocalTone: canonical source disappeared");
    }
    (void)bytes;
    return CudaTonePlane{static_cast<float*>(image->Texture().DevicePointer()),
                         image->Texture().Bytes()};
  }

  static auto AllocateScratchPlane(CudaRenderDevice& device, std::size_t bytes) -> CudaTonePlane {
    void* ptr = device.Workspace().TransientBuffers().Allocate(bytes);
    if (ptr == nullptr) {
      throw std::runtime_error("ExecuteCudaLocalTone: transient allocation failed");
    }
    return CudaTonePlane{static_cast<float*>(ptr), bytes};
  }

  static void ExtractReference(CudaRenderDevice& device, const Texture& input, CudaTonePlane dest,
                               std::uint32_t width, std::uint32_t height,
                               const LocalToneDecision& decision,
                               const ResolvedRenderGeometry& geometry) {
    const dim3 block{16, 16, 1};
    ExtractReferenceKernel<<<Grid(decision.widths[0], decision.heights[0], block), block, 0,
                             device.CommandContext().Stream()>>>(
        static_cast<const float4*>(input.DevicePointer()), dest.ptr, static_cast<int>(width),
        static_cast<int>(height), decision.widths[0], decision.heights[0],
        geometry.reference_to_render, static_cast<float>(geometry.full_reference_extent.width),
        static_cast<float>(geometry.full_reference_extent.height));
    CheckLaunch("extract reference");
  }

  static void Extract(CudaRenderDevice& device, const Texture& input, CudaTonePlane dest,
                      std::uint32_t width, std::uint32_t height, const LocalToneDecision& decision) {
    const dim3 block{16, 16, 1};
    ExtractKernel<<<Grid(decision.widths[0], decision.heights[0], block), block, 0,
                    device.CommandContext().Stream()>>>(
        static_cast<const float4*>(input.DevicePointer()), dest.ptr, static_cast<int>(width),
        static_cast<int>(height), decision.widths[0], decision.heights[0]);
    CheckLaunch("extract");
  }

  static void PyramidDown(CudaRenderDevice& device, CudaTonePlane src, CudaTonePlane dst,
                          const LocalToneDecision& decision, int level) {
    const dim3 block{16, 16, 1};
    DownKernel<<<Grid(decision.widths[level], decision.heights[level], block), block, 0,
                 device.CommandContext().Stream()>>>(
        src.ptr, dst.ptr, decision.widths[level - 1], decision.heights[level - 1],
        decision.widths[level], decision.heights[level]);
    CheckLaunch("pyramid down");
  }

  static void FillZero(CudaRenderDevice& device, CudaTonePlane plane) {
    if (::cudaMemsetAsync(plane.ptr, 0, plane.bytes, device.CommandContext().Stream()) !=
        cudaSuccess) {
      throw std::runtime_error("ExecuteCudaLocalTone: result clear failed");
    }
  }

  static void Remap(CudaRenderDevice& device, CudaTonePlane src, CudaTonePlane dst,
                    const LocalToneDecision& decision, const local_tone_mapping::LlfSample& sample,
                    float sigma) {
    const dim3 block{16, 16, 1};
    RemapKernel<<<Grid(decision.widths[0], decision.heights[0], block), block, 0,
                  device.CommandContext().Stream()>>>(
        src.ptr, dst.ptr, decision.widths[0], decision.heights[0], sample, sigma);
    CheckLaunch("remap");
  }

  static void Select(CudaRenderDevice& device, CudaTonePlane source, CudaTonePlane lo,
                     CudaTonePlane lo_coarse, CudaTonePlane hi, CudaTonePlane hi_coarse,
                     CudaTonePlane output, const LocalToneDecision& decision, int level,
                     const local_tone_mapping::LlfSample& lo_sample,
                     const local_tone_mapping::LlfSample& hi_sample, bool first, bool last,
                     bool top) {
    const dim3 block{16, 16, 1};
    SelectKernel<<<Grid(decision.widths[level], decision.heights[level], block), block, 0,
                   device.CommandContext().Stream()>>>(
        source.ptr, lo.ptr, top ? nullptr : lo_coarse.ptr, hi.ptr, top ? nullptr : hi_coarse.ptr,
        output.ptr, decision.widths[level], decision.heights[level],
        top ? 1 : decision.widths[level + 1], top ? 1 : decision.heights[level + 1],
        lo_sample.gamma, hi_sample.gamma, first, last, top);
    CheckLaunch("select");
  }

  static void Collapse(CudaRenderDevice& device, CudaTonePlane lap, CudaTonePlane coarse,
                       CudaTonePlane output, const LocalToneDecision& decision, int level) {
    const dim3 block{16, 16, 1};
    CollapseKernel<<<Grid(decision.widths[level], decision.heights[level], block), block, 0,
                     device.CommandContext().Stream()>>>(
        lap.ptr, coarse.ptr, output.ptr, decision.widths[level], decision.heights[level],
        decision.widths[level + 1], decision.heights[level + 1]);
    CheckLaunch("collapse");
  }

  static void ApplyAdjusted(CudaRenderDevice& device, const Texture& input, Texture& output,
                            CudaTonePlane reference, CudaTonePlane adjusted, std::uint32_t width,
                            std::uint32_t height, const LocalToneDecision& decision) {
    const dim3 block{16, 16, 1};
    ApplyKernel<<<Grid(static_cast<int>(width), static_cast<int>(height), block), block, 0,
                  device.CommandContext().Stream()>>>(
        static_cast<const float4*>(input.DevicePointer()), reference.ptr, adjusted.ptr,
        static_cast<float4*>(output.DevicePointer()), static_cast<int>(width),
        static_cast<int>(height), decision.widths[0], decision.heights[0], decision.apply_uv);
    CheckLaunch("apply");
  }

  static void PersistCanonical(CudaRenderDevice& device, CudaTonePlane plane, const GraphValueId& id,
                               const LocalToneDecision& decision, int current_long_edge) {
    auto& workspace    = device.Workspace();
    auto& invalidation = workspace.ResultInvalidation();
    const ImageExtent extent{decision.mask_extent.width, decision.mask_extent.height};
    auto& image =
        workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::R32f});
    CopyPlaneToTexture(device, plane, image.Texture());
    const auto needed = invalidation.MakeImageRepresentation(
        id, extent, TextureFormat::R32f, static_cast<std::uint32_t>(current_long_edge));
    workspace.Images().RecordUnpublished(id, invalidation.RequiredRevision(id), needed,
                                         device.CommandContext().SubmissionId(),
                                         static_cast<std::uint64_t>(current_long_edge));
  }

  static void PersistCanonicalSource(CudaRenderDevice& device, CudaTonePlane plane,
                                     const GraphValueId& source_id, const LocalToneDecision& decision,
                                     int current_long_edge) {
    PersistCanonical(device, plane, source_id, decision, current_long_edge);
  }

  static void PersistCanonicalResult(CudaRenderDevice& device, CudaTonePlane plane,
                                     const GraphValueId& result_id, const LocalToneDecision& decision,
                                     int current_long_edge) {
    PersistCanonical(device, plane, result_id, decision, current_long_edge);
  }
};

}  // namespace

auto ExecuteCudaLocalTone(CudaRenderDevice& device, const CudaBackend::Texture2D& input,
                          CudaBackend::Texture2D& output, const NodeId& grade_id,
                          float shadows_slider, float highlights_slider,
                          const ResolvedRenderGeometry& geometry) -> CudaLocalToneResult {
  const auto executed = LocalToneExecutor<CudaLocalToneOps>::Execute(
      device, input, output, grade_id, shadows_slider, highlights_slider, geometry);
  CudaLocalToneResult tone;
  tone.reference_resource_id       = executed.reference_resource_id;
  tone.rebuilt_reference           = executed.rebuilt_reference;
  tone.sampled_canonical_reference = executed.sampled_canonical_reference;
  tone.transient_bytes             = executed.transient_bytes;
  return tone;
}

}  // namespace alcedo
