//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/cuda/cuda_brush_raster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "edit/mask/brush_raster_encoding.hpp"
#include "edit/mask/brush_source_geometry.hpp"
#include "edit/mask/brush_spatial_index.hpp"
#include "edit/mask/brush_stroke.hpp"

namespace alcedo {
namespace {

[[noreturn]] void FailCudaBrush(std::string_view message) {
  throw std::runtime_error(std::string{message});
}

__device__ auto DeviceDabCoverage(float distance, float radius, float strength, float hardness)
    -> float {
  if (hardness >= 1.0f) {
    return distance <= radius ? strength : 0.0f;
  }
  const float inner = hardness * radius;
  if (distance <= inner) {
    return strength;
  }
  if (distance >= radius) {
    return 0.0f;
  }
  const float span = radius - inner;
  return strength * (1.0f - (distance - inner) / span);
}

__global__ void FillR8RectKernel(std::uint8_t* pixels, int width, int height, int x0, int y0,
                                 int rect_w, int rect_h, std::uint8_t value) {
  const int x = x0 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = y0 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x < x0 || y < y0 || x >= x0 + rect_w || y >= y0 + rect_h || x >= width || y >= height) {
    return;
  }
  pixels[y * width + x] = value;
}

__global__ void StampDabKernel(std::uint8_t* pixels, int width, int height, int x0, int y0,
                               int rect_w, int rect_h, float world_x, float world_y, float radius,
                               float strength, float hardness, float full_w, float full_h,
                               int erase) {
  const int x = x0 + static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = y0 + static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x < x0 || y < y0 || x >= x0 + rect_w || y >= y0 + rect_h || x >= width || y >= height) {
    return;
  }
  const float cx = (static_cast<float>(x) + 0.5f) * full_w / static_cast<float>(width);
  const float cy = (static_cast<float>(y) + 0.5f) * full_h / static_cast<float>(height);
  const float coverage =
      DeviceDabCoverage(hypotf(cx - world_x, cy - world_y), radius, strength, hardness);
  const float scaled = fminf(fmaxf(coverage * 255.0f + 0.5f, 0.0f), 255.0f);
  const auto  dab    = static_cast<std::uint8_t>(scaled);
  const int   index  = y * width + x;
  const auto  previous = pixels[index];
  pixels[index] =
      erase != 0 ? (previous < static_cast<std::uint8_t>(255u - dab)
                        ? previous
                        : static_cast<std::uint8_t>(255u - dab))
                 : (previous > dab ? previous : dab);
}

void LaunchFill(std::uint8_t* pixels, Extent2D extent, RectI rect, std::uint8_t value,
                cudaStream_t stream) {
  if (RectIEmpty(rect)) {
    return;
  }
  const dim3 block(16, 16);
  const dim3 grid((static_cast<unsigned>(rect.width) + 15u) / 16u,
                  (static_cast<unsigned>(rect.height) + 15u) / 16u);
  FillR8RectKernel<<<grid, block, 0, stream>>>(
      pixels, static_cast<int>(extent.width), static_cast<int>(extent.height), rect.x, rect.y,
      rect.width, rect.height, value);
}

void LaunchStamp(std::uint8_t* pixels, Extent2D raster, Extent2D full_reference, RectI clip,
                 const BrushCanonicalSample& sample, Vector2 translation, BrushStrokeMode mode,
                 cudaStream_t stream) {
  const auto support = IntersectTexelRect(
      BrushDabOutputTexelSupport(sample, translation, raster, full_reference), clip);
  if (RectIEmpty(support)) {
    return;
  }
  const dim3 block(16, 16);
  const dim3 grid((static_cast<unsigned>(support.width) + 15u) / 16u,
                  (static_cast<unsigned>(support.height) + 15u) / 16u);
  StampDabKernel<<<grid, block, 0, stream>>>(
      pixels, static_cast<int>(raster.width), static_cast<int>(raster.height), support.x, support.y,
      support.width, support.height, sample.local_x + translation.x,
      sample.local_y + translation.y, sample.radius, sample.strength, sample.hardness,
      static_cast<float>(full_reference.width), static_cast<float>(full_reference.height),
      mode == BrushStrokeMode::Erase ? 1 : 0);
}

void StampRange(std::uint8_t* pixels, Extent2D raster, Extent2D full_reference,
                const BrushMaskSource& source, const BrushCoverageUpdate& update,
                cudaStream_t stream) {
  if (update.stroke_index >= source.strokes.size() || update.stroke_end > source.strokes.size() ||
      update.stroke_index >= update.stroke_end) {
    FailCudaBrush("ApplyParameterizedBrushCuda: stamp stroke range is outside the source");
  }
  const auto clip = ClipTexelRect(update.dirty, raster);
  for (auto stroke_index = update.stroke_index; stroke_index < update.stroke_end; ++stroke_index) {
    const auto& stroke  = source.strokes[stroke_index];
    const auto  samples = BrushStrokeSamples(stroke);
    const auto  begin   = stroke_index == update.stroke_index ? update.sample_begin : 0;
    if (begin > samples.size()) {
      FailCudaBrush("ApplyParameterizedBrushCuda: stamp sample begin is outside the stroke");
    }
    for (auto i = begin; i < samples.size(); ++i) {
      LaunchStamp(pixels, raster, full_reference, clip, samples[i], source.placement_translation,
                  stroke.mode, stream);
    }
  }
}

void ReplayDirty(std::uint8_t* pixels, Extent2D raster, Extent2D full_reference,
                 const BrushMaskSource& source, RectI dirty, cudaStream_t stream) {
  const auto region = ClipTexelRect(dirty, raster);
  if (RectIEmpty(region)) {
    return;
  }
  LaunchFill(pixels, raster, region, 0, stream);
  BrushSpatialIndex index;
  index.Rebuild(source, raster, full_reference);
  const auto spans = index.QueryOutput(region, source.placement_translation);
  for (const auto& span : spans) {
    if (span.stroke_index >= source.strokes.size()) {
      FailCudaBrush("ApplyParameterizedBrushCuda: spatial index stroke is outside the source");
    }
    const auto& stroke = source.strokes[span.stroke_index];
    if (stroke.id != span.stroke_id) {
      FailCudaBrush("ApplyParameterizedBrushCuda: spatial index StrokeId does not match");
    }
    const auto samples = BrushStrokeSamples(stroke);
    if (span.sample_end > samples.size() || span.sample_begin >= span.sample_end) {
      FailCudaBrush("ApplyParameterizedBrushCuda: spatial index sample span is outside the stroke");
    }
    for (auto i = span.sample_begin; i < span.sample_end; ++i) {
      LaunchStamp(pixels, raster, full_reference, region, samples[i], source.placement_translation,
                  stroke.mode, stream);
    }
  }
}

}  // namespace

void ApplyParameterizedBrushCuda(CudaRenderDevice& device, CudaBackend::Texture2D& dest,
                                 const BrushMaskSource& source, Extent2D full_reference,
                                 const BrushCoverageUpdate& update, bool texture_uninitialized) {
  if (dest.Format() != TextureFormat::R8 || dest.DevicePointer() == nullptr || dest.Width() == 0 ||
      dest.Height() == 0) {
    FailCudaBrush("ApplyParameterizedBrushCuda: destination must be a non-empty R8 texture");
  }
  if (full_reference.Empty()) {
    throw std::invalid_argument("ApplyParameterizedBrushCuda: full_reference must be positive");
  }
  if (source.raster_algorithm_version != kBrushRasterAlgorithmVersion ||
      source.source_format_version != kBrushSourceFormatVersion) {
    FailCudaBrush("ApplyParameterizedBrushCuda: unsupported brush algorithm version");
  }
  const Extent2D raster{dest.Width(), dest.Height()};
  auto*          pixels = static_cast<std::uint8_t*>(dest.DevicePointer());
  const auto     stream = device.CommandContext().Stream();
  if (texture_uninitialized || update.kind == BrushCoverageUpdateKind::ReplayDirty) {
    auto replay = update;
    if (texture_uninitialized) {
      replay.kind  = BrushCoverageUpdateKind::ReplayDirty;
      replay.dirty = FullTexelRect(raster);
    }
    ReplayDirty(pixels, raster, full_reference, source, replay.dirty, stream);
  } else if (update.kind == BrushCoverageUpdateKind::StampNewSamples) {
    StampRange(pixels, raster, full_reference, source, update, stream);
  }
  if (::cudaGetLastError() != cudaSuccess) {
    FailCudaBrush("ApplyParameterizedBrushCuda: CUDA kernel launch failed");
  }
}

}  // namespace alcedo
