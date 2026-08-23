//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "cuda_acescc.cuh"
#include "edit/geometry/texture_sampling_plan.hpp"
#include "edit/pipeline/local_tone_mapping.hpp"
#include "edit/runtime/cuda/cuda_local_tone_pass.hpp"
#include "edit/runtime/cuda/cuda_render_device.hpp"
#include "edit/runtime/texture_format.hpp"

namespace alcedo {
namespace {

using local_tone_mapping::LlfSample;

constexpr int   kMaxLevels = local_tone_mapping::kMaxLevels;
constexpr float kRadius    = 18.0f;

struct PyramidLayout {
  int                         count = 0;
  std::array<int, kMaxLevels> widths{};
  std::array<int, kMaxLevels> heights{};
};

auto MakeLayout(int width, int height) -> PyramidLayout {
  PyramidLayout layout;
  layout.count      = local_tone_mapping::ComputeLevelCount(width, height, kRadius);
  layout.widths[0]  = width;
  layout.heights[0] = height;
  for (int level = 1; level < layout.count; ++level) {
    layout.widths[level]  = std::max(1, (layout.widths[level - 1] + 1) / 2);
    layout.heights[level] = std::max(1, (layout.heights[level - 1] + 1) / 2);
  }
  return layout;
}

auto EnsureBuffer(CudaRenderWorkspace& workspace, const GraphValueId& id, std::size_t bytes)
    -> CudaBackend::Buffer& {
  auto* buffer = workspace.Values().Find(id);
  if (buffer != nullptr && buffer->Bytes() >= bytes) return *buffer;
  workspace.Values().Store(id, workspace.Device().CreateBuffer(bytes));
  return *workspace.Values().Find(id);
}

auto LevelId(const NodeId& grade_id, const char* family, int level) -> GraphValueId {
  return {grade_id, PortId{std::string{"local_tone."} + family + "." + std::to_string(level)}};
}

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

struct CanonicalLlfCache {
  ContentKey key{};
  int        mask_width        = 0;
  int        mask_height       = 0;
  int        source_long_edge  = 0;
  bool       canonical         = false;
};

auto CacheSlot(const CudaRenderWorkspace& workspace, const NodeId& grade_id) -> CanonicalLlfCache& {
  static std::map<std::pair<const void*, std::string>, CanonicalLlfCache> slots;
  return slots[std::make_pair(&workspace, std::string{grade_id.Value()})];
}

auto LocalUvPlan(std::uint32_t width, std::uint32_t height) -> Matrix3x3 {
  Matrix3x3 matrix;
  matrix.m[0] = 1.0f / static_cast<float>(width);
  matrix.m[4] = 1.0f / static_cast<float>(height);
  return matrix;
}

}  // namespace

auto ExecuteCudaLocalTone(CudaRenderDevice& device, const GraphValueId& input_id,
                          const GraphValueId& output_id, const NodeId& grade_id,
                          std::uint32_t width, std::uint32_t height, float shadows_slider,
                          float highlights_slider, const ResolvedRenderGeometry& geometry,
                          ContentKey reference_key) -> CudaLocalToneResult {
  auto& workspace = device.Workspace();
  auto* input     = workspace.Images().Find(input_id);
  if (!workspace.IsRendering() || input == nullptr || input->Empty()) {
    throw std::runtime_error("ExecuteCudaLocalTone: missing active input");
  }
  if (geometry.full_reference_extent.Empty() || geometry.render_extent.Empty()) {
    throw std::runtime_error("ExecuteCudaLocalTone: geometry extents must be positive");
  }
  auto& output = workspace.AcquireImageForWrite(output_id, {width, height, TextureFormat::Rgba32f});
  input        = workspace.Images().Find(input_id);

  const auto canonical_dims = local_tone_mapping::ComputeMaskDimensions(
      static_cast<int>(geometry.full_reference_extent.width),
      static_cast<int>(geometry.full_reference_extent.height),
      local_tone_mapping::kReferenceMaskMaxLongEdge);
  const bool full_edit = CoversFullEditSpace(geometry);
  const int  current_long_edge =
      std::max(static_cast<int>(width), static_cast<int>(height));
  auto&      slot            = CacheSlot(workspace, grade_id);
  const auto canonical_bytes = static_cast<std::size_t>(canonical_dims.width) *
                               static_cast<std::size_t>(canonical_dims.height) * sizeof(float);
  auto* cached_source = workspace.Values().Find(LevelId(grade_id, "source", 0));
  auto* cached_result = workspace.Values().Find(LevelId(grade_id, "result", 0));
  const bool cache_valid =
      slot.canonical && slot.key == reference_key && slot.mask_width == canonical_dims.width &&
      slot.mask_height == canonical_dims.height && cached_source != nullptr &&
      cached_source->Bytes() >= canonical_bytes && cached_result != nullptr &&
      cached_result->Bytes() >= canonical_bytes;
  const bool sample_canonical = cache_valid && !(full_edit && current_long_edge > slot.source_long_edge);

  const dim3          block{16, 16, 1};
  auto                stream = device.CommandContext().Stream();
  CudaLocalToneResult tone;

  if (sample_canonical) {
    const auto sampling = MakeLlfSamplingPlan(
        geometry, Extent2D{static_cast<std::uint32_t>(slot.mask_width),
                           static_cast<std::uint32_t>(slot.mask_height)});
    ApplyKernel<<<Grid(static_cast<int>(width), static_cast<int>(height), block), block, 0,
                  stream>>>(
        static_cast<const float4*>(input->Texture().DevicePointer()),
        static_cast<const float*>(cached_source->DevicePointer()),
        static_cast<const float*>(cached_result->DevicePointer()),
        static_cast<float4*>(output.Texture().DevicePointer()), static_cast<int>(width),
        static_cast<int>(height), slot.mask_width, slot.mask_height, sampling.render_to_texture_uv);
    CheckLaunch("canonical sample");
    tone.reference_resource_id       = cached_source->ResourceId();
    tone.sampled_canonical_reference = true;
    return tone;
  }

  const bool seed_canonical = full_edit;
  const auto mask_dims =
      seed_canonical ? canonical_dims
                     : local_tone_mapping::ComputeMaskDimensions(
                           static_cast<int>(width), static_cast<int>(height),
                           local_tone_mapping::kReferenceMaskMaxLongEdge);
  const auto                     layout = MakeLayout(mask_dims.width, mask_dims.height);
  std::array<float*, kMaxLevels> source{};
  std::array<float*, kMaxLevels> remap_a{};
  std::array<float*, kMaxLevels> remap_b{};
  std::array<float*, kMaxLevels> result{};
  std::uint64_t                  reference_id = 0;
  for (int level = 0; level < layout.count; ++level) {
    const auto bytes =
        static_cast<std::size_t>(layout.widths[level]) * layout.heights[level] * sizeof(float);
    auto& source_buffer = EnsureBuffer(workspace, LevelId(grade_id, "source", level), bytes);
    auto& a_buffer      = EnsureBuffer(workspace, LevelId(grade_id, "remap_a", level), bytes);
    auto& b_buffer      = EnsureBuffer(workspace, LevelId(grade_id, "remap_b", level), bytes);
    auto& result_buffer = EnsureBuffer(workspace, LevelId(grade_id, "result", level), bytes);
    source[level]       = static_cast<float*>(source_buffer.DevicePointer());
    remap_a[level]      = static_cast<float*>(a_buffer.DevicePointer());
    remap_b[level]      = static_cast<float*>(b_buffer.DevicePointer());
    result[level]       = static_cast<float*>(result_buffer.DevicePointer());
    if (level == 0) reference_id = source_buffer.ResourceId();
  }

  if (seed_canonical) {
    ExtractReferenceKernel<<<Grid(layout.widths[0], layout.heights[0], block), block, 0, stream>>>(
        static_cast<const float4*>(input->Texture().DevicePointer()), source[0],
        static_cast<int>(width), static_cast<int>(height), layout.widths[0], layout.heights[0],
        geometry.reference_to_render, static_cast<float>(geometry.full_reference_extent.width),
        static_cast<float>(geometry.full_reference_extent.height));
  } else {
    ExtractKernel<<<Grid(layout.widths[0], layout.heights[0], block), block, 0, stream>>>(
        static_cast<const float4*>(input->Texture().DevicePointer()), source[0],
        static_cast<int>(width), static_cast<int>(height), layout.widths[0], layout.heights[0]);
  }
  for (int level = 1; level < layout.count; ++level) {
    DownKernel<<<Grid(layout.widths[level], layout.heights[level], block), block, 0, stream>>>(
        source[level - 1], source[level], layout.widths[level - 1], layout.heights[level - 1],
        layout.widths[level], layout.heights[level]);
  }

  const float shadow_amount    = std::clamp(shadows_slider * 1.5f / 80.0f, -1.5f, 1.5f);
  const float highlight_amount = std::clamp(-highlights_slider * 1.5f / 100.0f, -1.5f, 1.5f);
  const float sigma            = local_tone_mapping::SigmaR(shadow_amount, highlight_amount);
  const auto  samples          = local_tone_mapping::BuildSamples(shadow_amount, highlight_amount);
  for (int level = 0; level < layout.count; ++level) {
    ::cudaMemsetAsync(
        result[level], 0,
        static_cast<std::size_t>(layout.widths[level]) * layout.heights[level] * sizeof(float),
        stream);
  }
  const auto build_remap = [&](const LlfSample& sample, std::array<float*, kMaxLevels>& levels) {
    RemapKernel<<<Grid(layout.widths[0], layout.heights[0], block), block, 0, stream>>>(
        source[0], levels[0], layout.widths[0], layout.heights[0], sample, sigma);
    for (int level = 1; level < layout.count; ++level) {
      DownKernel<<<Grid(layout.widths[level], layout.heights[level], block), block, 0, stream>>>(
          levels[level - 1], levels[level], layout.widths[level - 1], layout.heights[level - 1],
          layout.widths[level], layout.heights[level]);
    }
  };
  build_remap(samples[0], remap_a);
  build_remap(samples[1], remap_b);
  for (std::size_t pair = 0; pair + 1 < samples.size(); ++pair) {
    for (int level = 0; level < layout.count; ++level) {
      const bool top = level + 1 == layout.count;
      SelectKernel<<<Grid(layout.widths[level], layout.heights[level], block), block, 0, stream>>>(
          source[level], remap_a[level], top ? nullptr : remap_a[level + 1], remap_b[level],
          top ? nullptr : remap_b[level + 1], result[level], layout.widths[level],
          layout.heights[level], top ? 1 : layout.widths[level + 1],
          top ? 1 : layout.heights[level + 1], samples[pair].gamma, samples[pair + 1].gamma,
          pair == 0, pair + 2 == samples.size(), top);
    }
    if (pair + 2 < samples.size()) {
      std::swap(remap_a, remap_b);
      build_remap(samples[pair + 2], remap_b);
    }
  }
  for (int level = layout.count - 2; level >= 0; --level) {
    CollapseKernel<<<Grid(layout.widths[level], layout.heights[level], block), block, 0, stream>>>(
        result[level], result[level + 1], remap_a[level], layout.widths[level],
        layout.heights[level], layout.widths[level + 1], layout.heights[level + 1]);
    std::swap(result[level], remap_a[level]);
  }
  auto* named_result = workspace.Values().Find(LevelId(grade_id, "result", 0));
  if (named_result == nullptr) {
    throw std::runtime_error("ExecuteCudaLocalTone: missing result.0 after pyramid collapse");
  }
  if (result[0] != named_result->DevicePointer()) {
    if (::cudaMemcpyAsync(named_result->DevicePointer(), result[0],
                          static_cast<std::size_t>(layout.widths[0]) * layout.heights[0] *
                              sizeof(float),
                          cudaMemcpyDeviceToDevice, stream) != cudaSuccess) {
      throw std::runtime_error("ExecuteCudaLocalTone: cannot store collapsed reference");
    }
    result[0] = static_cast<float*>(named_result->DevicePointer());
  }

  const Matrix3x3 apply_uv =
      seed_canonical
          ? MakeLlfSamplingPlan(geometry, Extent2D{static_cast<std::uint32_t>(layout.widths[0]),
                                                   static_cast<std::uint32_t>(layout.heights[0])})
                .render_to_texture_uv
          : LocalUvPlan(width, height);
  ApplyKernel<<<Grid(static_cast<int>(width), static_cast<int>(height), block), block, 0, stream>>>(
      static_cast<const float4*>(input->Texture().DevicePointer()), source[0], result[0],
      static_cast<float4*>(output.Texture().DevicePointer()), static_cast<int>(width),
      static_cast<int>(height), layout.widths[0], layout.heights[0], apply_uv);
  CheckLaunch("kernel launch");

  if (seed_canonical) {
    slot.key              = reference_key;
    slot.mask_width       = layout.widths[0];
    slot.mask_height      = layout.heights[0];
    slot.source_long_edge = current_long_edge;
    slot.canonical        = true;
  } else {
    slot = {};
  }
  tone.reference_resource_id = reference_id;
  tone.rebuilt_reference     = true;
  return tone;
}

}  // namespace alcedo
