//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM8.2: CUDA Feather and Expand of AI Mask coverage (SM8 section 5.1). The passes follow
// AiMaskEdgeDispatch: alternating 3 x 3 morphology on R8, then a separable finite Gaussian with a
// float horizontal intermediate. Every pass clamps coordinates to the output (replicated border).

#include <cuda_runtime.h>

#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/runtime/ai_mask_coverage_work.hpp"
#include "edit/runtime/cuda/cuda_backend.hpp"

namespace alcedo {
namespace {

constexpr int kBlockWidth  = 16;
constexpr int kBlockHeight = 16;

/// R8 input of a pass: @p width x @p height texels placed at (@p x, @p y) in the output grid. A
/// placed texel outside them reads 0.
struct PlacedR8 {
  const std::uint8_t* pixels = nullptr;
  int                 width  = 0;
  int                 height = 0;
  int                 x      = 0;
  int                 y      = 0;
};

/// Half kernel of AiMaskFeatherWeights, passed by value (808 bytes at the largest radius).
struct FeatherWeights {
  int   radius = 0;
  float values[kAiMaskFeatherMaximumRadius + 1];
};

__device__ auto ClampToRange(int value, int last) -> int { return min(max(value, 0), last); }

/// R8 value at output texel (x, y), with the coordinate clamped to the @p width x @p height
/// output first.
__device__ auto ReadPlaced(const PlacedR8& input, int x, int y, int width, int height)
    -> std::uint32_t {
  const int source_x = ClampToRange(x, width - 1) - input.x;
  const int source_y = ClampToRange(y, height - 1) - input.y;
  if (source_x < 0 || source_y < 0 || source_x >= input.width || source_y >= input.height) {
    return 0u;
  }
  return input.pixels[source_y * input.width + source_x];
}

/// One 3 x 3 dilate (@p dilate) or erode step with the cross or the square kernel.
__global__ void MorphologyStepKernel(PlacedR8 input, std::uint8_t* output, int width, int height,
                                     int dilate, int square) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) {
    return;
  }
  std::uint32_t value = ReadPlaced(input, x, y, width, height);
  for (int dy = -1; dy <= 1; ++dy) {
    for (int dx = -1; dx <= 1; ++dx) {
      if ((dx == 0 && dy == 0) || (!square && dx != 0 && dy != 0)) {
        continue;
      }
      const auto neighbor = ReadPlaced(input, x + dx, y + dy, width, height);
      value               = dilate ? max(value, neighbor) : min(value, neighbor);
    }
  }
  output[y * width + x] = static_cast<std::uint8_t>(value);
}

/// Horizontal Feather pass: float sum of the R8 values / 255.
__global__ void FeatherRowsKernel(PlacedR8 input, float* output, int width, int height,
                                  FeatherWeights weights) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) {
    return;
  }
  // The host converts with `u * float(1 / 255.0)`; 1.0f / 255.0f is the same float.
  constexpr float kScale = 1.0f / 255.0f;
  float           sum =
      weights.values[0] * (static_cast<float>(ReadPlaced(input, x, y, width, height)) * kScale);
  for (int k = 1; k <= weights.radius; ++k) {
    const float left  = static_cast<float>(ReadPlaced(input, x - k, y, width, height)) * kScale;
    const float right = static_cast<float>(ReadPlaced(input, x + k, y, width, height)) * kScale;
    sum += weights.values[k] * (left + right);
  }
  output[y * width + x] = sum;
}

/// Vertical Feather pass over the float rows; quantizes like cv::saturate_cast<uchar>.
__global__ void FeatherColumnsKernel(const float* input, std::uint8_t* output, int width,
                                     int height, FeatherWeights weights) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) {
    return;
  }
  const int last = height - 1;
  float     sum  = weights.values[0] * input[y * width + x];
  for (int k = 1; k <= weights.radius; ++k) {
    const float above = input[ClampToRange(y - k, last) * width + x];
    const float below = input[ClampToRange(y + k, last) * width + x];
    sum += weights.values[k] * (above + below);
  }
  const int quantized   = min(max(__float2int_rn(sum * 255.0f), 0), 255);
  output[y * width + x] = static_cast<std::uint8_t>(quantized);
}

void RequireTexture(const CudaBackend::Texture2D* texture, Extent2D extent, TextureFormat format,
                    const char* name) {
  if (texture == nullptr || texture->DevicePointer() == nullptr) {
    throw std::runtime_error(std::string{"CudaBackend::RecordAiMaskEdge: the "} + name +
                             " texture is missing");
  }
  if (texture->Width() != extent.width || texture->Height() != extent.height ||
      texture->Format() != format) {
    throw std::runtime_error(std::string{"CudaBackend::RecordAiMaskEdge: the "} + name +
                             " texture is not " + std::to_string(extent.width) + " x " +
                             std::to_string(extent.height) + " " + TextureFormatName(format));
  }
}

void CheckLaunch(const char* pass) {
  const auto status = ::cudaGetLastError();
  if (status != cudaSuccess) {
    throw std::runtime_error(std::string{"CudaBackend::RecordAiMaskEdge: the "} + pass +
                             " launch failed: " + ::cudaGetErrorString(status));
  }
}

auto ToPlaced(const CudaBackend::Texture2D& texture, int x, int y) -> PlacedR8 {
  return PlacedR8{static_cast<const std::uint8_t*>(texture.DevicePointer()),
                  static_cast<int>(texture.Width()), static_cast<int>(texture.Height()), x, y};
}

}  // namespace

void CudaBackend::RecordAiMaskEdge(const AiMaskEdgeDispatch&            dispatch,
                                   const AiMaskEdgeTextures<Texture2D>& textures,
                                   CommandContext&                      command_context) {
  const auto extent = dispatch.output_extent;
  if (extent.Empty() || (dispatch.expand_steps == 0 && !dispatch.HasFeather())) {
    throw std::runtime_error("CudaBackend::RecordAiMaskEdge: the dispatch has no work");
  }
  if (static_cast<int>(dispatch.feather_weights.size()) > kAiMaskFeatherMaximumRadius + 1) {
    throw std::runtime_error("CudaBackend::RecordAiMaskEdge: the Feather radius is too large");
  }
  RequireTexture(textures.source, dispatch.source_extent, TextureFormat::R8, "source");
  RequireTexture(textures.output, extent, TextureFormat::R8, "output");
  for (std::uint32_t i = 0; i < dispatch.MorphologyWorkTextureCount(); ++i) {
    RequireTexture(textures.morphology[i], extent, TextureFormat::R8, "morphology work");
  }
  if (dispatch.HasFeather()) {
    RequireTexture(textures.feather_rows, extent, TextureFormat::R32f, "Feather work");
  }
  // Earlier launch errors on this thread would otherwise be reported for these passes.
  (void)::cudaGetLastError();

  const int  width  = static_cast<int>(extent.width);
  const int  height = static_cast<int>(extent.height);
  const dim3 block(kBlockWidth, kBlockHeight);
  const dim3 grid((extent.width + kBlockWidth - 1) / kBlockWidth,
                  (extent.height + kBlockHeight - 1) / kBlockHeight);
  const auto stream  = command_context.Stream();

  PlacedR8   current = ToPlaced(*textures.source, dispatch.source_x, dispatch.source_y);
  for (int step = 0; step < dispatch.expand_steps; ++step) {
    const bool last_writes_output = !dispatch.HasFeather() && step + 1 == dispatch.expand_steps;
    Texture2D& target = last_writes_output ? *textures.output : *textures.morphology[step % 2];
    MorphologyStepKernel<<<grid, block, 0, stream>>>(
        current, static_cast<std::uint8_t*>(target.DevicePointer()), width, height,
        dispatch.dilate ? 1 : 0, step % 2);
    CheckLaunch("morphology");
    current = ToPlaced(target, 0, 0);
  }

  if (dispatch.HasFeather()) {
    FeatherWeights weights{};
    weights.radius = static_cast<int>(dispatch.feather_weights.size()) - 1;
    for (std::size_t k = 0; k < dispatch.feather_weights.size(); ++k) {
      weights.values[k] = dispatch.feather_weights[k];
    }
    auto* rows = static_cast<float*>(textures.feather_rows->DevicePointer());
    FeatherRowsKernel<<<grid, block, 0, stream>>>(current, rows, width, height, weights);
    CheckLaunch("Feather rows");
    FeatherColumnsKernel<<<grid, block, 0, stream>>>(
        rows, static_cast<std::uint8_t*>(textures.output->DevicePointer()), width, height, weights);
    CheckLaunch("Feather columns");
  }
}

}  // namespace alcedo
