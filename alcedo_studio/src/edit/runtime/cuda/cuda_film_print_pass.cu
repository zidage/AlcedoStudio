//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cuda_runtime.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "color/color_encoding_math.h"
#include "cuda/cuda_check.hpp"
#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/cuda/cuda_backend.hpp"
#include "edit/runtime/cuda/cuda_drt_runtime_state.hpp"
#include "edit/runtime/cuda/cuda_film_grain_tile.cuh"
#include "edit/runtime/cuda/cuda_film_print_pass.hpp"
#include "edit/runtime/cuda/cuda_scene_work.hpp"
#include "edit/runtime/cuda/drt/drt_display_pixel.cuh"
#include "edit/runtime/diffusion_filter_plan.hpp"
#include "edit/runtime/drt/drt_output_resolver.hpp"
#include "edit/runtime/drt_post_film_grain.hpp"
#include "edit/runtime/film/film_grain_field.h"
#include "edit/runtime/film/film_grain_negative.h"
#include "edit/runtime/film/film_negative_scan.hpp"
#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_print_math.h"
#include "edit/runtime/film/film_print_resources.hpp"
#include "edit/runtime/film_print_plan.hpp"
#include "edit/runtime/runtime_invalidation.hpp"
#include "edit/runtime/texture_format.hpp"
#include "edit/runtime/texture_pool.hpp"

namespace alcedo {
namespace {

constexpr std::uint32_t kBlock      = 256;
constexpr unsigned      kImageBlock = 16;

// The scan display kernel takes the film values, the scan terms, and the DRT values by value,
// within the 4 KB of kernel arguments.
static_assert(sizeof(FilmKernelParams) + sizeof(FilmKernelNegativeScan) + sizeof(CudaDrtGpuParams) +
                  64 <=
              4096);

auto                    GridFor(std::uint32_t width, std::uint32_t height) -> dim3 {
  return dim3{(width + kImageBlock - 1) / kImageBlock, (height + kImageBlock - 1) / kImageBlock};
}

__device__ __forceinline__ auto Transform(const Matrix3x3& matrix, float x, float y) -> float2 {
  return make_float2(matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
                     matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]);
}

/// The boundary rule 'reflect' of scipy.ndimage: (d c b a | a b c d | d c b a).
__device__ __forceinline__ auto Reflect(int position, int size) -> int {
  if (position >= 0 && position < size) return position;
  const int period = 2 * size;
  position %= period;
  if (position < 0) position += period;
  return position < size ? position : period - 1 - position;
}

/// Bilinear read at a position where texel `i` covers [i, i + 1), as the lab enlarges an image.
template <class Texel>
__device__ __forceinline__ auto ReadBilinear(const Texel* image, int width, int height, float px,
                                             float py) -> float3 {
  const float  x  = fminf(fmaxf(px - 0.5f, 0.0f), static_cast<float>(width - 1));
  const float  y  = fminf(fmaxf(py - 0.5f, 0.0f), static_cast<float>(height - 1));
  const int    x0 = static_cast<int>(x);
  const int    y0 = static_cast<int>(y);
  const int    x1 = min(x0 + 1, width - 1);
  const int    y1 = min(y0 + 1, height - 1);
  const float  fx = x - static_cast<float>(x0);
  const float  fy = y - static_cast<float>(y0);
  const Texel& a  = image[y0 * width + x0];
  const Texel& b  = image[y0 * width + x1];
  const Texel& c  = image[y1 * width + x0];
  const Texel& d  = image[y1 * width + x1];
  const float  gx = 1.0f - fx;
  const float  gy = 1.0f - fy;
  return make_float3(gy * (gx * a.x + fx * b.x) + fy * (gx * c.x + fx * d.x),
                     gy * (gx * a.y + fx * b.y) + fy * (gx * c.y + fx * d.y),
                     gy * (gx * a.z + fx * b.z) + fy * (gx * c.z + fx * d.z));
}

__device__ __forceinline__ void StoreRgb(float3& out, float r, float g, float b) {
  out = make_float3(r, g, b);
}

/// The canonical results keep opaque alpha.
__device__ __forceinline__ void StoreRgb(float4& out, float r, float g, float b) {
  out = make_float4(r, g, b, 1.0f);
}

/// The first source pixel of cell @p index, for cells that cover the image exactly.
__device__ __forceinline__ auto BoxCellStart(int index, int size, int count) -> int {
  return static_cast<int>((static_cast<std::int64_t>(index) * size + count - 1) / count);
}

/**
 * The first source pixel of cell @p index of a half-size image with @p count cells: a cell has two
 * pixels, and the last cell of an odd size has three. The center of each cell is then at the
 * position where `StepWithHalfSizeInhibitor` reads it. Cells that cover the image at a scale near 2
 * move their centers up to half a pixel from that position.
 */
__device__ __forceinline__ auto HalfCellStart(int index, int size, int count) -> int {
  return index == count ? size : 2 * index;
}

/// One step at pixel (@p x, @p y), with a half-size inhibitor that the bilinear read of the lab
/// enlarges. @p log_q is the stored log exposure.
__device__ __forceinline__ auto StepWithHalfSizeInhibitor(const float3& log_q, int x, int y,
                                                          const float3* inhibitor, int columns,
                                                          int rows, const float* values,
                                                          const FilmKernelParams& params)
    -> FilmKernelRgb {
  const float3 i = ReadBilinear(inhibitor, columns, rows, (static_cast<float>(x) + 0.5f) * 0.5f,
                                (static_cast<float>(y) + 0.5f) * 0.5f);
  return FilmKernelStep(&params, values, params.inhibition_image_k,
                        FilmKernelMakeRgb(log_q.x, log_q.y, log_q.z),
                        FilmKernelMakeRgb(i.x, i.y, i.z));
}

/**
 * FP4 at one pixel from the exposure @p value. With a blur of the inhibitor, @p a takes the first
 * amounts and @p q the log exposure of the later steps. With no blur, @p a takes the final amounts,
 * and there are no later steps.
 */
__device__ __forceinline__ void FirstStage(const float3& value, float3& q, float3& a,
                                           bool inhibitor_blur, const float* values,
                                           const FilmKernelParams& params) {
  const auto exposure = FilmKernelMakeRgb(value.x, value.y, value.z);
  if (inhibitor_blur) {
    const auto first = FilmKernelFirstAmountsWithPointPart(&params, values, exposure);
    q                = make_float3(first.log_q.r, first.log_q.g, first.log_q.b);
    a                = make_float3(first.amounts.r, first.amounts.g, first.amounts.b);
  } else {
    const auto amounts = FilmKernelPointAmounts(&params, values, exposure);
    a                  = make_float3(amounts.r, amounts.g, amounts.b);
  }
}

/// The point stages have their own kernels, because the blurs run between them.
__global__ void ExposureKernel(const float4* input, float3* output, std::uint32_t pixels,
                               const float* values, FilmKernelParams params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels) return;
  const float4 source = input[index];
  // Input is linear AP1. The final colour space of a print is not known here, so a colour that a
  // grade moved out of AP1 comes back at this point, as in DrtKernel (lab rule
  // 'entry_reference_gamut_compression'). Negative Scan has no such step: its display transform
  // has its own entry compression.
  if (params.scan != 0U) {
    const auto q  = FilmKernelLayerExposure(&params, values, source.x, source.y, source.z);
    output[index] = make_float3(q.r, q.g, q.b);
    return;
  }
  const AcesRgcRgb compressed = AcesReferenceGamutCompress(source.x, source.y, source.z);
  const auto q = FilmKernelLayerExposure(&params, values, compressed.r, compressed.g, compressed.b);
  output[index]       = make_float3(q.r, q.g, q.b);
}

__global__ void FirstAmountsKernel(float3* q, float3* a, std::uint32_t pixels, bool inhibitor_blur,
                                   const float* values, FilmKernelParams params) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels) return;
  FirstStage(q[index], q[index], a[index], inhibitor_blur, values, params);
}

/// The last step with a half-size inhibitor.
__global__ void InhibitedStepHalfSizeKernel(const float3* log_q, float3* amounts, int width,
                                            int height, const float3* inhibitor, int columns,
                                            int rows, const float* values,
                                            FilmKernelParams params) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const int  index = y * width + x;
  const auto a =
      StepWithHalfSizeInhibitor(log_q[index], x, y, inhibitor, columns, rows, values, params);
  amounts[index] = make_float3(a.r, a.g, a.b);
}

/**
 * A step with a half-size inhibitor before the last step: the output is only the input of the next
 * blur, so this form writes its box mean and no full-size image. One thread covers one cell.
 */
__global__ void InhibitedStepToReducedKernel(const float3* log_q, int width, int height,
                                             const float3* inhibitor, float3* reduced, int columns,
                                             int rows, const float* values,
                                             FilmKernelParams params) {
  const int i = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  const int j = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  if (i >= rows || j >= columns) return;
  const int y0 = HalfCellStart(i, height, rows);
  const int y1 = HalfCellStart(i + 1, height, rows);
  const int x0 = HalfCellStart(j, width, columns);
  const int x1 = HalfCellStart(j + 1, width, columns);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const auto a = StepWithHalfSizeInhibitor(log_q[y * width + x], x, y, inhibitor, columns, rows,
                                               values, params);
      r += a.r;
      g += a.g;
      b += a.b;
    }
  }
  const float scale        = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
  reduced[i * columns + j] = make_float3(r * scale, g * scale, b * scale);
}

/**
 * FP4 and FP5 in one dispatch at full size: the vertical pass of the inhibitor blur, then one step.
 * @p middle holds the horizontal pass. This form writes no blurred image, so it has one image write
 * and one image read less than a blur and a step.
 */
__global__ void InhibitedStepVerticalKernel(const float3* middle, const float3* log_q,
                                            float3* amounts, int width, int height,
                                            FilmBlurKernel inhibitor_y, const float* values,
                                            FilmKernelParams params) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const std::size_t stride = static_cast<std::size_t>(width);
  const std::size_t index  = static_cast<std::size_t>(y) * stride + x;
  const float3      center = middle[index];
  float             r      = inhibitor_y.weights[0][0] * center.x;
  float             g      = inhibitor_y.weights[1][0] * center.y;
  float             b      = inhibitor_y.weights[2][0] * center.z;
  const int         radius = static_cast<int>(inhibitor_y.radius);
  for (int tap = 1; tap <= radius; ++tap) {
    const float3& top    = middle[static_cast<std::size_t>(Reflect(y - tap, height)) * stride + x];
    const float3& bottom = middle[static_cast<std::size_t>(Reflect(y + tap, height)) * stride + x];
    r += inhibitor_y.weights[0][tap] * (top.x + bottom.x);
    g += inhibitor_y.weights[1][tap] * (top.y + bottom.y);
    b += inhibitor_y.weights[2][tap] * (top.z + bottom.z);
  }
  const float3 q = log_q[index];
  const auto   a = FilmKernelStep(&params, values, params.inhibition_image_k,
                                  FilmKernelMakeRgb(q.x, q.y, q.z), FilmKernelMakeRgb(r, g, b));
  amounts[index] = make_float3(a.r, a.g, a.b);
}

template <class Texel>
__global__ void BlurHorizontalKernel(const Texel* src, Texel* dst, int width, int height,
                                     FilmBlurKernel kernel) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const Texel* row    = src + static_cast<std::size_t>(y) * width;
  const Texel  center = row[x];
  float        r      = kernel.weights[0][0] * center.x;
  float        g      = kernel.weights[1][0] * center.y;
  float        b      = kernel.weights[2][0] * center.z;
  const int    radius = static_cast<int>(kernel.radius);
  for (int tap = 1; tap <= radius; ++tap) {
    const Texel& left  = row[Reflect(x - tap, width)];
    const Texel& right = row[Reflect(x + tap, width)];
    r += kernel.weights[0][tap] * (left.x + right.x);
    g += kernel.weights[1][tap] * (left.y + right.y);
    b += kernel.weights[2][tap] * (left.z + right.z);
  }
  StoreRgb(dst[static_cast<std::size_t>(y) * width + x], r, g, b);
}

/// `dst = weight * blur`, or with @p accumulate, `dst += weight * blur`.
template <class Texel>
__global__ void BlurVerticalKernel(const Texel* src, Texel* dst, int width, int height,
                                   FilmBlurKernel kernel, float weight, bool accumulate) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const std::size_t stride = static_cast<std::size_t>(width);
  const Texel       center = src[static_cast<std::size_t>(y) * stride + x];
  float             r      = kernel.weights[0][0] * center.x;
  float             g      = kernel.weights[1][0] * center.y;
  float             b      = kernel.weights[2][0] * center.z;
  const int         radius = static_cast<int>(kernel.radius);
  for (int tap = 1; tap <= radius; ++tap) {
    const Texel& top    = src[static_cast<std::size_t>(Reflect(y - tap, height)) * stride + x];
    const Texel& bottom = src[static_cast<std::size_t>(Reflect(y + tap, height)) * stride + x];
    r += kernel.weights[0][tap] * (top.x + bottom.x);
    g += kernel.weights[1][tap] * (top.y + bottom.y);
    b += kernel.weights[2][tap] * (top.z + bottom.z);
  }
  Texel& out = dst[static_cast<std::size_t>(y) * stride + x];
  // A new pool image can hold NaN, so this form must not read the output.
  if (accumulate) {
    StoreRgb(out, out.x + weight * r, out.y + weight * g, out.z + weight * b);
  } else {
    StoreRgb(out, weight * r, weight * g, weight * b);
  }
}

/**
 * FP2 in one vertical dispatch: `dst = wide_weight * wide + narrow_weight * narrow`. @p middle
 * holds the horizontal pass of the wide Gaussian. The narrow Gaussian reads @p exposure directly,
 * so this form needs no second horizontal pass and does not read the output.
 */
__global__ void SpreadVerticalKernel(const float3* middle, const float3* exposure, float3* dst,
                                     int width, int height, FilmBlurKernel wide_y,
                                     float wide_weight, FilmNarrowKernel narrow,
                                     float narrow_weight) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const std::size_t stride = static_cast<std::size_t>(width);
  const float3      center = middle[static_cast<std::size_t>(y) * stride + x];
  float             wr     = wide_y.weights[0][0] * center.x;
  float             wg     = wide_y.weights[1][0] * center.y;
  float             wb     = wide_y.weights[2][0] * center.z;
  const int         radius = static_cast<int>(wide_y.radius);
  for (int tap = 1; tap <= radius; ++tap) {
    const float3& top    = middle[static_cast<std::size_t>(Reflect(y - tap, height)) * stride + x];
    const float3& bottom = middle[static_cast<std::size_t>(Reflect(y + tap, height)) * stride + x];
    wr += wide_y.weights[0][tap] * (top.x + bottom.x);
    wg += wide_y.weights[1][tap] * (top.y + bottom.y);
    wb += wide_y.weights[2][tap] * (top.z + bottom.z);
  }
  float     nr      = 0.0f;
  float     ng      = 0.0f;
  float     nb      = 0.0f;
  const int nradius = static_cast<int>(narrow.radius);
  for (int dy = -nradius; dy <= nradius; ++dy) {
    const float3* row = exposure + static_cast<std::size_t>(Reflect(y + dy, height)) * stride;
    const int     ay  = dy < 0 ? -dy : dy;
    for (int dx = -nradius; dx <= nradius; ++dx) {
      const float3& value = row[Reflect(x + dx, width)];
      const int     ax    = dx < 0 ? -dx : dx;
      nr += narrow.y[0][ay] * narrow.x[0][ax] * value.x;
      ng += narrow.y[1][ay] * narrow.x[1][ax] * value.y;
      nb += narrow.y[2][ay] * narrow.x[2][ax] * value.z;
    }
  }
  dst[static_cast<std::size_t>(y) * stride + x] =
      make_float3(wide_weight * wr + narrow_weight * nr, wide_weight * wg + narrow_weight * ng,
                  wide_weight * wb + narrow_weight * nb);
}

/// Box mean with cells that cover the image exactly, as the lab reduces an image.
__global__ void BoxReduceKernel(const float4* src, int src_width, int src_height, float4* dst,
                                int columns, int rows) {
  const int i = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  const int j = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  if (i >= rows || j >= columns) return;
  const int y0 = BoxCellStart(i, src_height, rows);
  const int y1 = BoxCellStart(i + 1, src_height, rows);
  const int x0 = BoxCellStart(j, src_width, columns);
  const int x1 = BoxCellStart(j + 1, src_width, columns);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const float4& value = src[y * src_width + x];
      r += value.x;
      g += value.y;
      b += value.z;
    }
  }
  const float scale    = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
  dst[i * columns + j] = make_float4(r * scale, g * scale, b * scale, 1.0f);
}

/// What a canvas holds: the mean of the values, of their transmittance, or of their display light.
enum class CanvasValue : int {
  kValue         = 0,  ///< The exposure, for Halation
  kTransmittance = 1,  ///< 10^-a of the amounts, for the Enlarger flare of a print
  kDisplayLight  = 2,  ///< Linear display light of the light table, for the loupe flare
};

/**
 * Canvas from the render: the mean of @p samples x @p samples render pixels in the footprint of one
 * texel, of the value that @p mode selects. Only kDisplayLight reads @p print_rgba and @p params.
 */
__global__ void CanvasReduceKernel(const float3* src, int width, int height, float4* dst,
                                   int dst_width, int dst_height, Matrix3x3 canvas_to_render,
                                   int samples, CanvasValue mode, const float* print_rgba,
                                   FilmKernelParams params) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= dst_width || y >= dst_height) return;
  const float step = 1.0f / static_cast<float>(samples);
  float       r    = 0.0f;
  float       g    = 0.0f;
  float       b    = 0.0f;
  for (int j = 0; j < samples; ++j) {
    const float cy = static_cast<float>(y) + (static_cast<float>(j) + 0.5f) * step;
    for (int i = 0; i < samples; ++i) {
      const float  cx     = static_cast<float>(x) + (static_cast<float>(i) + 0.5f) * step;
      const float2 render = Transform(canvas_to_render, cx, cy);
      const int    sx     = min(max(static_cast<int>(floorf(render.x)), 0), width - 1);
      const int    sy     = min(max(static_cast<int>(floorf(render.y)), 0), height - 1);
      const float3 value  = src[static_cast<std::size_t>(sy) * width + sx];
      if (mode == CanvasValue::kTransmittance) {
        r += FilmKernelTransmittance(value.x);
        g += FilmKernelTransmittance(value.y);
        b += FilmKernelTransmittance(value.z);
      } else if (mode == CanvasValue::kDisplayLight) {
        const auto light = FilmKernelPrintDisplay(&params, print_rgba,
                                                  FilmKernelMakeRgb(value.x, value.y, value.z));
        r += light.r;
        g += light.g;
        b += light.b;
      } else {
        r += value.x;
        g += value.y;
        b += value.z;
      }
    }
  }
  const float scale      = step * step;
  dst[y * dst_width + x] = make_float4(r * scale, g * scale, b * scale, 1.0f);
}

/// FP3 and FP4 in one dispatch. See `FirstStage` for what @p q and @p a take.
__global__ void HalationAndFirstAmountsKernel(float3* q, float3* a, int width, int height,
                                              const float4* canvas, int canvas_width,
                                              int canvas_height, Matrix3x3 render_to_canvas,
                                              float3 strength, bool inhibitor_blur,
                                              const float* values, FilmKernelParams params) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= width || y >= height) return;
  const float2 position =
      Transform(render_to_canvas, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
  const float3 blurred = ReadBilinear(canvas, canvas_width, canvas_height, position.x, position.y);
  const int    index   = y * width + x;
  const float3 value   = q[index];
  const float3 mix     = make_float3((1.0f - strength.x) * value.x + strength.x * blurred.x,
                                     (1.0f - strength.y) * value.y + strength.y * blurred.y,
                                     (1.0f - strength.z) * value.z + strength.z * blurred.z);
  FirstStage(mix, q[index], a[index], inhibitor_blur, values, params);
}

/**
 * FP4 for a half-size inhibitor, with FP3 when @p canvas is not null. The amounts are only the input
 * of the first blur, so this form writes their box mean and no full-size image of them. @p q takes
 * the log exposure of the later steps. One thread covers one cell.
 */
__global__ void ReducedFirstAmountsKernel(float3* q, int width, int height, float3* reduced,
                                          int columns, int rows, const float4* canvas,
                                          int canvas_width, int canvas_height,
                                          Matrix3x3 render_to_canvas, float3 strength,
                                          const float* values, FilmKernelParams params) {
  const int i = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  const int j = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  if (i >= rows || j >= columns) return;
  const int y0 = HalfCellStart(i, height, rows);
  const int y1 = HalfCellStart(i + 1, height, rows);
  const int x0 = HalfCellStart(j, width, columns);
  const int x1 = HalfCellStart(j + 1, width, columns);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const int index = y * width + x;
      float3    value = q[index];
      if (canvas != nullptr) {
        const float2 position =
            Transform(render_to_canvas, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
        const float3 blurred =
            ReadBilinear(canvas, canvas_width, canvas_height, position.x, position.y);
        value = make_float3((1.0f - strength.x) * value.x + strength.x * blurred.x,
                            (1.0f - strength.y) * value.y + strength.y * blurred.y,
                            (1.0f - strength.z) * value.z + strength.z * blurred.z);
      }
      const auto first = FilmKernelFirstAmountsWithPointPart(
          &params, values, FilmKernelMakeRgb(value.x, value.y, value.z));
      q[index] = make_float3(first.log_q.r, first.log_q.g, first.log_q.b);
      r += first.amounts.r;
      g += first.amounts.g;
      b += first.amounts.b;
    }
  }
  const float scale        = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
  reduced[i * columns + j] = make_float3(r * scale, g * scale, b * scale);
}

/// Rule 'negative_show_grade': the box mean of the amounts on the half-size image. One thread covers
/// one cell.
__global__ void AmountsReduceKernel(const float3* amounts, int width, int height, float3* reduced,
                                    int columns, int rows) {
  const int i = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  const int j = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  if (i >= rows || j >= columns) return;
  const int y0 = HalfCellStart(i, height, rows);
  const int y1 = HalfCellStart(i + 1, height, rows);
  const int x0 = HalfCellStart(j, width, columns);
  const int x1 = HalfCellStart(j + 1, width, columns);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const float3 value = amounts[y * width + x];
      r += value.x;
      g += value.y;
      b += value.z;
    }
  }
  const float scale        = 1.0f / static_cast<float>((y1 - y0) * (x1 - x0));
  reduced[i * columns + j] = make_float3(r * scale, g * scale, b * scale);
}

/// Threads of one window of the horizontal prescan pass: one warp.
constexpr unsigned kPrescanLanes   = 32;
/// Windows of one block of the horizontal prescan pass.
constexpr unsigned kPrescanWindows = 8;

/**
 * The prescan of Negative Scan, horizontal pass (`film_negative_scan::ReduceFrame`): per row of the
 * render and output column, the mean of the scan density T over the window of the column.
 * @p windows holds the windows of `film_negative_scan::PrescanPlanOf`. One warp covers one window,
 * so the reads of a warp are adjacent; the warp adds its sums with shuffles.
 */
__global__ void ScanPrescanRowsKernel(const float3* amounts, int width, int height,
                                      const float* scanner_table, FilmKernelParams params,
                                      const std::int32_t* windows, int columns, float3* middle) {
  const unsigned lane   = threadIdx.x;
  const auto     window = static_cast<std::int64_t>(blockIdx.x) * kPrescanWindows + threadIdx.y;
  if (window >= static_cast<std::int64_t>(columns) * height) return;
  const int     y     = static_cast<int>(window / columns);
  const int     j     = static_cast<int>(window % columns);
  const int     first = windows[j];
  const int     count = windows[columns + j];
  const float3* row   = amounts + static_cast<std::size_t>(y) * width;
  float         r     = 0.0f;
  float         g     = 0.0f;
  float         b     = 0.0f;
  for (int x = first + static_cast<int>(lane); x < first + count; x += kPrescanLanes) {
    const float3 a = row[x];
    const auto   t =
        FilmKernelReadRecordTable(&params, scanner_table, FilmKernelMakeRgb(a.x, a.y, a.z));
    r += t.r;
    g += t.g;
    b += t.b;
  }
  for (unsigned offset = kPrescanLanes / 2; offset > 0; offset /= 2) {
    r += __shfl_down_sync(0xffffffffU, r, offset);
    g += __shfl_down_sync(0xffffffffU, g, offset);
    b += __shfl_down_sync(0xffffffffU, b, offset);
  }
  if (lane == 0) {
    const float scale = 1.0f / static_cast<float>(count);
    middle[static_cast<std::size_t>(y) * columns + j] = make_float3(r * scale, g * scale, b * scale);
  }
}

/// The vertical pass of the prescan: per output pixel, the mean of @p middle over the window of
/// the row.
__global__ void ScanPrescanColumnsKernel(const float3* middle, int columns, int height,
                                         const std::int32_t* windows, int rows, float4* prescan) {
  const int j = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int i = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (j >= columns || i >= rows) return;
  const int first = windows[2 * columns + i];
  const int count = windows[2 * columns + rows + i];
  float     r     = 0.0f;
  float     g     = 0.0f;
  float     b     = 0.0f;
  for (int y = first; y < first + count; ++y) {
    const float3 value = middle[static_cast<std::size_t>(y) * columns + j];
    r += value.x;
    g += value.y;
    b += value.z;
  }
  const float scale        = 1.0f / static_cast<float>(count);
  prescan[i * columns + j] = make_float4(r * scale, g * scale, b * scale, 1.0f);
}

/**
 * FP6 of print material Negative Scan (lab step P14): the scan density T of the scanner table, the
 * point form of `FilmKernelNegativeScanPixel` to linear AP1, then the display transform of the
 * document (`DrtDisplayPixel`, the arithmetic of `DrtKernel`). There is no flare and no grade. With
 * @p linear_output, the linear AP1 itself, for a test of the form. Alpha comes from @p scene.
 */
/// The inputs of the Negative Scan form of FP6. With @ref linear_output, the linear AP1 itself,
/// for a test of the form. Alpha comes from @ref scene.
struct ScanDisplayArgs {
  const float3*          amounts;
  const float4*          scene;
  float4*                output;
  int                    width;
  const float*           scanner_table;
  FilmKernelParams       params;
  FilmKernelNegativeScan terms;
  CudaDrtGpuParams       drt;
  bool                   linear_output;
};

/**
 * FP6 of print material Negative Scan (lab step P14) at pixel @p index with the record @p record:
 * the scan density T of the scanner table, the point form of `FilmKernelNegativeScanPixel` to
 * linear AP1, then the display transform of the document (`DrtDisplayPixel`, the arithmetic of
 * `DrtKernel`). There is no flare and no grade.
 */
__device__ __forceinline__ void ScanDisplayPixel(const ScanDisplayArgs& args, std::size_t index,
                                                 FilmKernelRgb record) {
  const auto  t      = FilmKernelReadRecordTable(&args.params, args.scanner_table, record);
  const auto  linear = FilmKernelNegativeScanPixel(t, &args.terms);
  const float alpha  = args.scene[index].w;
  if (args.linear_output) {
    args.output[index] = make_float4(linear.r, linear.g, linear.b, alpha);
    return;
  }
  const float3 encoded = CUDA::DrtDisplayPixel(make_float3(linear.r, linear.g, linear.b), args.drt);
  args.output[index]   = make_float4(encoded.x, encoded.y, encoded.z, alpha);
}

__global__ void ScanDisplayKernel(ScanDisplayArgs args, std::uint32_t pixels) {
  const std::uint32_t index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index >= pixels) return;
  const float3 a = args.amounts[index];
  ScanDisplayPixel(args, index, FilmKernelMakeRgb(a.x, a.y, a.z));
}

/// The inputs of the print and the light table forms of FP6. @ref flare is null when the Enlarger
/// flare is off. Alpha comes from @ref scene. @ref soft is the blurred amounts of rule
/// 'negative_show_grade', null with no image plane, and half size when @ref soft_half_size is true.
/// On a light table, @ref flare is the blurred display light of the loupe flare, and @ref soft is
/// not read.
struct DisplayArgs {
  const float3*    amounts;
  const float4*    scene;
  float4*          output;
  int              width;
  int              height;
  const float*     print_rgba;
  const float4*    flare;
  int              flare_width;
  int              flare_height;
  Matrix3x3        render_to_flare;
  float            flare_strength;
  const float3*    soft;
  int              soft_columns;
  int              soft_rows;
  bool             soft_half_size;
  FilmKernelParams params;
};

/// FP6, fused, at pixel (@p x, @p y) with the record @p record of the negative.
__device__ __forceinline__ void DisplayPixel(const DisplayArgs& args, int x, int y,
                                             FilmKernelRgb record) {
  const int               index  = y * args.width + x;
  const FilmKernelParams& params = args.params;
  const int               tf     = static_cast<int>(params.display_encoding);
  if (params.light_table != 0U) {
    // Lab rules 'light_table_view', 'loupe_flare', and 'dark_surround_on_display' in display light.
    auto light = FilmKernelPrintDisplay(&params, args.print_rgba, record);
    if (args.flare != nullptr) {
      const float2 position = Transform(args.render_to_flare, static_cast<float>(x) + 0.5f,
                                        static_cast<float>(y) + 0.5f);
      const float3 blurred =
          ReadBilinear(args.flare, args.flare_width, args.flare_height, position.x, position.y);
      light = FilmKernelLoupeFlare(light, FilmKernelMakeRgb(blurred.x, blurred.y, blurred.z),
                                   args.flare_strength);
    }
    const auto finished =
        FilmKernelFinishDisplay(&params, FilmKernelDisplaySurround(&params, light));
    args.output[index] = make_float4(
        CeDisplayEncodeChannel(tf, finished.r, 1.0f), CeDisplayEncodeChannel(tf, finished.g, 1.0f),
        CeDisplayEncodeChannel(tf, finished.b, 1.0f), args.scene[index].w);
    return;
  }
  FilmKernelRgb soft_record = record;
  if (params.negative_grade != 0U && args.soft != nullptr) {
    const float3 s = args.soft_half_size
                         ? ReadBilinear(args.soft, args.soft_columns, args.soft_rows,
                                        (static_cast<float>(x) + 0.5f) * 0.5f,
                                        (static_cast<float>(y) + 0.5f) * 0.5f)
                         : args.soft[index];
    soft_record = FilmKernelMakeRgb(s.x, s.y, s.z);
  }
  if (args.flare != nullptr) {
    const float2 position =
        Transform(args.render_to_flare, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f);
    const float3 blurred =
        ReadBilinear(args.flare, args.flare_width, args.flare_height, position.x, position.y);
    record = FilmKernelFlareAmounts(record, FilmKernelMakeRgb(blurred.x, blurred.y, blurred.z),
                                    args.flare_strength);
  }
  if (params.negative_grade != 0U) {
    if (args.soft == nullptr) soft_record = record;
    record = FilmKernelNegativeGrade(&params, record, soft_record);
  }
  const auto finished =
      FilmKernelFinishDisplay(&params, FilmKernelPrintDisplay(&params, args.print_rgba, record));
  const auto linear  = params.cinema_look != 0U ? FilmKernelCinemaLook(finished) : finished;
  args.output[index] = make_float4(CeDisplayEncodeChannel(tf, linear.r, 1.0f),
                                   CeDisplayEncodeChannel(tf, linear.g, 1.0f),
                                   CeDisplayEncodeChannel(tf, linear.b, 1.0f), args.scene[index].w);
}

__global__ void DisplayKernel(DisplayArgs args) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= args.width || y >= args.height) return;
  const float3 a = args.amounts[y * args.width + x];
  DisplayPixel(args, x, y, FilmKernelMakeRgb(a.x, a.y, a.z));
}

/// The record of pixel @p index of @p amounts with the grain @p grain of rule 'grain_layer_mix'.
__device__ __forceinline__ auto GrainRecord(const FilmGrainNegativeParams* params,
                                            const float3* amounts, std::size_t index,
                                            FilmGrainNoise3 grain) -> FilmKernelRgb {
  const float3               a = amounts[index];
  const FilmGrainNegativeRgb r =
      FilmGrainNegativeRecord(params, FilmGrainNegativeRgb{a.x, a.y, a.z}, grain);
  return FilmKernelMakeRgb(r.r, r.g, r.b);
}

/// Phase G5: FP6 of the print and the light table with the grain of the negative, as the apply
/// step of the grain tile kernel (cuda_film_grain_tile.cuh).
struct GrainDisplayApply {
  using Params                              = FilmGrainNegativeParams;
  static constexpr int            kTileRows = 2 * cuda_film_grain::kTile;
  DisplayArgs                     args;

  __device__ __forceinline__ void operator()(const Params* params, int x, int y,
                                             FilmGrainNoise3 grain) const {
    const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(args.width) + x;
    DisplayPixel(args, x, y, GrainRecord(params, args.amounts, index, grain));
  }
};

/// Phase G5: FP6 of Negative Scan with the grain of the negative, as the apply step of the grain
/// tile kernel.
struct GrainScanDisplayApply {
  using Params                              = FilmGrainNegativeParams;
  static constexpr int            kTileRows = 2 * cuda_film_grain::kTile;
  ScanDisplayArgs                 args;

  __device__ __forceinline__ void operator()(const Params* params, int x, int y,
                                             FilmGrainNoise3 grain) const {
    const auto index = static_cast<std::size_t>(y) * static_cast<std::size_t>(args.width) + x;
    ScanDisplayPixel(args, index, GrainRecord(params, args.amounts, index, grain));
  }
};

// The grain forms of FP6 take their inputs, the field, and the grain values by value, within the
// 4 KB of kernel arguments.
static_assert(sizeof(GrainScanDisplayApply) + sizeof(FilmGrainNegativeParams) +
                  sizeof(FilmGrainFieldParams) + 64 <=
              4096);
static_assert(sizeof(GrainDisplayApply) + sizeof(FilmGrainNegativeParams) +
                  sizeof(FilmGrainFieldParams) + 64 <=
              4096);

using Lease = ResourceLease<CudaBackend>;

auto Pointer(Lease& lease) -> float4* {
  return static_cast<float4*>(lease.Texture().DevicePointer());
}

/// The images between the steps have no alpha, so they use three channels of the pool image.
auto PointerRgb(Lease& lease) -> float3* {
  return static_cast<float3*>(lease.Texture().DevicePointer());
}

/// GPU times between the marks of one recording, for the cost record of the plan.
class PassClock {
 public:
  PassClock(std::vector<FilmPrintPassTime>* out, cudaStream_t stream) : out_(out), stream_(stream) {
    if (out_ != nullptr) Record({});
  }
  PassClock(const PassClock&)                    = delete;
  auto operator=(const PassClock&) -> PassClock& = delete;
  ~PassClock() {
    for (auto& mark : marks_) {
      (void)::cudaEventDestroy(mark.second);
    }
  }

  void Mark(const char* name) {
    if (out_ != nullptr) Record(name);
  }

  /// Wait for the marks and add their times to the output.
  void Finish() {
    if (out_ == nullptr || marks_.empty()) return;
    cuda::CheckCuda(::cudaEventSynchronize(marks_.back().second), "RecordCudaFilmPrint: clock");
    for (std::size_t index = 1; index < marks_.size(); ++index) {
      float milliseconds = 0.0f;
      cuda::CheckCuda(
          ::cudaEventElapsedTime(&milliseconds, marks_[index - 1].second, marks_[index].second),
          "RecordCudaFilmPrint: clock");
      out_->push_back({marks_[index].first, milliseconds});
    }
  }

 private:
  void Record(std::string name) {
    cudaEvent_t event = nullptr;
    cuda::CheckCuda(::cudaEventCreate(&event), "RecordCudaFilmPrint: clock");
    marks_.emplace_back(std::move(name), event);
    cuda::CheckCuda(::cudaEventRecord(event, stream_), "RecordCudaFilmPrint: clock");
  }

  std::vector<FilmPrintPassTime>*                  out_;
  cudaStream_t                                     stream_;
  std::vector<std::pair<std::string, cudaEvent_t>> marks_;
};

/**
 * @brief The canonical result @p id: the published image, or an image from @p build.
 *
 * The rule of `DecideDiffusionScatter`: a render that covers the full edit space builds and
 * publishes; an ROI samples a published image, and otherwise builds a temporary image from its own
 * pixels. Nothing is visible to another render before `PublishResults`.
 */
template <class Build>
auto ReadOrBuildCanvasResult(CudaRenderDevice& device, const ExecutionPlan& plan,
                             const GraphValueId& id, ImageExtent extent, std::uint32_t long_edge,
                             std::optional<Lease>& temporary, Build&& build) -> const float4* {
  auto&      workspace    = device.Workspace();
  auto&      invalidation = workspace.ResultInvalidation();
  const bool persist      = workspace.PersistsResult(id);
  const bool full_edit    = CoversFullEditSpace(plan.geometry);
  Lease*     canonical    = nullptr;
  if (persist) {
    const auto needed = invalidation.MakeImageRepresentation(
        id, extent, TextureFormat::Rgba32f, DiffusionScatterRequiredDetail(full_edit, long_edge));
    canonical = workspace.Images().BindValidResult(id, invalidation.RequiredRevision(id), needed,
                                                   workspace.Device().CompletedSubmission());
  } else {
    ++device.PassStats().result_policy_bypass;
  }
  const auto decision = DecideDiffusionScatter(persist, canonical != nullptr, full_edit, long_edge);
  if (decision.action == DiffusionScatterAction::SampleCanonical) {
    ++device.PassStats().film_canvas_sample;
    return Pointer(*canonical);
  }
  float4* target = nullptr;
  if (decision.persist_canonical) {
    target = Pointer(
        workspace.AcquireImageForWrite(id, {extent.width, extent.height, TextureFormat::Rgba32f}));
  } else {
    temporary.emplace(
        workspace.Textures().Acquire({extent.width, extent.height, TextureFormat::Rgba32f}));
    target = Pointer(*temporary);
  }
  build(target);
  if (decision.persist_canonical) {
    const auto published = invalidation.MakeImageRepresentation(id, extent, TextureFormat::Rgba32f,
                                                                decision.current_long_edge);
    workspace.Images().RecordUnpublished(id, invalidation.RequiredRevision(id), published,
                                         device.CommandContext().SubmissionId(),
                                         decision.current_long_edge);
  }
  ++device.PassStats().film_canvas_rebuild;
  return target;
}

}  // namespace

void RecordCudaFilmPrint(CudaRenderDevice& device, const ExecutionPlan& plan,
                         const DrtNodeModel& drt, std::optional<double> raw_cct,
                         const FrameSceneBinding& scene, const FrameSceneBinding& display,
                         std::uint32_t width, std::uint32_t height, float grain_strength,
                         std::vector<FilmPrintPassTime>* pass_times, bool separate_narrow_spread) {
  const auto resolved =
      DrtOutputResolver::ResolveNode(drt, plan.output_color_override, "ExecuteCudaDrt");
  const auto  film      = drt.Params().FilmPrint();
  auto&       tables    = SharedFilmPrintTables();
  const auto& bundle    = tables.Bundle();
  // Phase G5: the grain of the negative. A strength outside [0, 2] stops the render here, before
  // any GPU work.
  std::optional<FilmGrainPlan> grain;
  if (grain_strength != 0.0f) {
    grain = MakeDrtFilmPrintGrainPlan(drt, grain_strength, plan.geometry);
  }
  const auto  light     = bundle.SceneLightMix(film.scene_light, raw_cct);
  // Negative Scan reads the scanner table of the scanner of the document. A rendering setting reads
  // the kept table.
  const bool  scan      = IsFilmNegativeScan(film.negative, film.print_material);
  const auto  data =
      scan ? tables.ResolveNegativeScan(film.negative, light,
                                        film_negative_scan::ScannerOf(film.scan))
           : tables.Resolve(film.negative, film.print_material, light);
  // With auto colour off the terms are known now, so a black at or above the grey stops the render
  // before any GPU work.
  const bool  auto_colour = scan && film.scan.auto_colour != kFilmScanAutoIds[0];
  std::optional<film_negative_scan::RenderTerms> scan_terms;
  if (scan && !auto_colour) {
    scan_terms = film_negative_scan::MakeRenderTerms(data->negative_scan, film.scan, {});
  }

  auto&       workspace = device.Workspace();
  auto&       backend   = workspace.Device();
  auto&       context   = device.CommandContext();
  const auto  print = backend.AcquireLut(data->print_key, data->print_rgba, data->edge, context);
  const auto  table = backend.AcquireKeyedBuffer(
      data->kernel_key, std::as_bytes(std::span(data->kernel_values)), context);

  FilmKernelParams params{};
  film_print_kernel_tables::FillParams(bundle, film.negative, film.print_material, light,
                                       data->edge, params);
  params.display_encoding = static_cast<std::uint32_t>(resolved.eotf_);

  const auto negative     = bundle.Film(film.negative);
  const auto image =
      MakeFilmPrintImagePlan(plan.geometry, bundle.ImagePlaneConstants(), negative.layer_spread_um,
                             params.inhibition_has_image != 0U || params.negative_grade != 0U, film,
                             drt.Params().FilmFormat());
  // With no blur, one dispatch computes the final amounts, so FP4 and FP5 need no other image.
  const bool inhibitor_blur = image.image_plane && params.inhibition_has_image != 0U;

  const auto* values  = static_cast<const float*>(table.device_pointer);
  const auto* rgba    = static_cast<const float*>(print.device_pointer);
  const auto  acquire = [&workspace](std::uint32_t w, std::uint32_t h) {
    return workspace.Textures().Acquire({w, h, TextureFormat::Rgba32f});
  };
  const auto          w      = static_cast<int>(width);
  const auto          h      = static_cast<int>(height);

  const std::uint32_t pixels = width * height;
  const std::uint32_t blocks = (pixels + kBlock - 1) / kBlock;
  const dim3          image_block{kImageBlock, kImageBlock};
  const dim3          image_grid = GridFor(width, height);
  auto                stream     = context.Stream();
  PassClock           clock(pass_times, stream);

  // FP1.
  const auto*         scene_pixels =
      static_cast<const float4*>(CudaSceneTexture(device, scene).DevicePointer());
  auto                first_exposure = acquire(width, height);
  auto*               q              = PointerRgb(first_exposure);
  ExposureKernel<<<blocks, kBlock, 0, stream>>>(scene_pixels, q, pixels, values, params);
  clock.Mark("FP1 exposure");

  // FP2, rule 'spread_per_layer_pass'. After the spread, the first image holds the amounts.
  std::optional<Lease> second_exposure;
  std::optional<Lease> scratch;
  float3*              a = nullptr;
  std::optional<Lease> amounts;
  if (image.spread_passes != 0U) {
    second_exposure.emplace(acquire(width, height));
    scratch.emplace(acquire(width, height));
    auto* spread = PointerRgb(*second_exposure);
    auto* middle = PointerRgb(*scratch);
    if (image.spread_narrow_fused && !separate_narrow_spread) {
      BlurHorizontalKernel<<<image_grid, image_block, 0, stream>>>(static_cast<const float3*>(q),
                                                                   middle, w, h, image.spread_x[1]);
      clock.Mark("FP2 wide horizontal");
      SpreadVerticalKernel<<<image_grid, image_block, 0, stream>>>(
          middle, q, spread, w, h, image.spread_y[1], image.spread_weight[1], image.spread_narrow,
          image.spread_weight[0]);
      clock.Mark("FP2 vertical, with the narrow Gaussian");
    } else {
      for (std::uint32_t pass = 0; pass < image.spread_passes; ++pass) {
        BlurHorizontalKernel<<<image_grid, image_block, 0, stream>>>(
            static_cast<const float3*>(q), middle, w, h, image.spread_x[pass]);
        BlurVerticalKernel<<<image_grid, image_block, 0, stream>>>(
            static_cast<const float3*>(middle), spread, w, h, image.spread_y[pass],
            image.spread_weight[pass], pass != 0U);
      }
      clock.Mark("FP2 spread, 2 separate passes");
    }
    a = q;
    q = spread;
  } else {
    amounts.emplace(acquire(width, height));
    a = PointerRgb(*amounts);
  }

  const auto           long_edge = (std::max)(width, height);
  const auto           canvas    = image.canvas_extent;

  // FP3: the canonical image, then a read of it per layer, in one dispatch with the first amounts.
  std::optional<Lease> temporary_halation;
  const float4*        halation = nullptr;
  if (image.halation_active) {
    halation = ReadOrBuildCanvasResult(
        device, plan, FilmHalationResultId(plan.drt.node_id), canvas, long_edge, temporary_halation,
        [&](float4* target) {
          auto       reduced = acquire(canvas.width, canvas.height);
          auto       middle  = acquire(canvas.width, canvas.height);
          const auto grid    = GridFor(canvas.width, canvas.height);
          const auto cw      = static_cast<int>(canvas.width);
          const auto ch      = static_cast<int>(canvas.height);
          CanvasReduceKernel<<<grid, image_block, 0, stream>>>(
              q, w, h, Pointer(reduced), cw, ch, image.canvas_to_render,
              static_cast<int>(image.canvas_samples), CanvasValue::kValue, rgba, params);
          BlurHorizontalKernel<<<grid, image_block, 0, stream>>>(
              static_cast<const float4*>(Pointer(reduced)), Pointer(middle), cw, ch,
              image.halation_x);
          BlurVerticalKernel<<<grid, image_block, 0, stream>>>(
              static_cast<const float4*>(Pointer(middle)), target, cw, ch, image.halation_y, 1.0f,
              false);
        });
    clock.Mark("FP3 canvas");
  }
  const auto halation_strength = make_float3(image.halation_strength[0], image.halation_strength[1],
                                             image.halation_strength[2]);
  const auto canvas_width      = static_cast<int>(canvas.width);
  const auto canvas_height     = static_cast<int>(canvas.height);

  // FP4 and FP5, rules 'inhibition_two_steps' and 'inhibition_image_part'. After FP4, the image of
  // the exposure holds the log exposure that the steps read.
  if (inhibitor_blur && image.inhibitor_half_size) {
    // Only the last step writes full-size amounts. The first amounts and the first step write the
    // box mean directly, so the full-size image has no write and no read to reduce it.
    const auto extent  = image.inhibitor_extent;
    const auto grid    = GridFor(extent.width, extent.height);
    const auto columns = static_cast<int>(extent.width);
    const auto rows    = static_cast<int>(extent.height);
    // The full-size image holds both half-size images. The pool frees a size that no result
    // holds, so images of their own would need a new allocation on each edit.
    if (!scratch.has_value()) scratch.emplace(acquire(width, height));
    auto* reduced = PointerRgb(*scratch);
    auto* middle  = reduced + static_cast<std::size_t>(columns) * rows;
    ReducedFirstAmountsKernel<<<grid, image_block, 0, stream>>>(
        q, w, h, reduced, columns, rows, halation, canvas_width, canvas_height,
        image.render_to_canvas, halation_strength, values, params);
    clock.Mark("FP3 mix and FP4 first amounts, to half size");
    BlurHorizontalKernel<<<grid, image_block, 0, stream>>>(
        static_cast<const float3*>(reduced), middle, columns, rows, image.inhibitor_x);
    BlurVerticalKernel<<<grid, image_block, 0, stream>>>(
        static_cast<const float3*>(middle), reduced, columns, rows, image.inhibitor_y, 1.0f, false);
    clock.Mark("FP4 inhibitor blur");
    InhibitedStepToReducedKernel<<<grid, image_block, 0, stream>>>(q, w, h, reduced, middle,
                                                                   columns, rows, values, params);
    clock.Mark("FP5 first step, to half size");
    BlurHorizontalKernel<<<grid, image_block, 0, stream>>>(
        static_cast<const float3*>(middle), reduced, columns, rows, image.inhibitor_x);
    BlurVerticalKernel<<<grid, image_block, 0, stream>>>(
        static_cast<const float3*>(reduced), middle, columns, rows, image.inhibitor_y, 1.0f, false);
    clock.Mark("FP4 inhibitor blur");
    InhibitedStepHalfSizeKernel<<<image_grid, image_block, 0, stream>>>(q, a, w, h, middle, columns,
                                                                        rows, values, params);
    clock.Mark("FP5 last step");
  } else {
    if (image.halation_active) {
      HalationAndFirstAmountsKernel<<<image_grid, image_block, 0, stream>>>(
          q, a, w, h, halation, canvas_width, canvas_height, image.render_to_canvas,
          halation_strength, inhibitor_blur, values, params);
      clock.Mark("FP3 mix and FP4 amounts");
    } else {
      FirstAmountsKernel<<<blocks, kBlock, 0, stream>>>(q, a, pixels, inhibitor_blur, values,
                                                        params);
      clock.Mark("FP4 amounts");
    }
    if (inhibitor_blur) {
      if (!scratch.has_value()) scratch.emplace(acquire(width, height));
      auto* middle = PointerRgb(*scratch);
      for (int step = 0; step < 2; ++step) {
        BlurHorizontalKernel<<<image_grid, image_block, 0, stream>>>(
            static_cast<const float3*>(a), middle, w, h, image.inhibitor_x);
        clock.Mark("FP4 inhibitor horizontal");
        InhibitedStepVerticalKernel<<<image_grid, image_block, 0, stream>>>(
            middle, q, a, w, h, image.inhibitor_y, values, params);
        clock.Mark("FP4 vertical and FP5 inhibited step");
      }
    }
  }

  // Rule 'negative_show_grade': the blurred amounts, before the flare of FP6.
  std::optional<Lease> soft_reduced;
  std::optional<Lease> soft_middle;
  std::optional<Lease> soft_image;
  const float3*        soft = nullptr;
  if (image.image_plane && params.negative_grade != 0U) {
    const auto    extent  = image.inhibitor_extent;
    const auto    grid    = GridFor(extent.width, extent.height);
    const auto    columns = static_cast<int>(extent.width);
    const auto    rows    = static_cast<int>(extent.height);
    const float3* source  = a;
    if (image.inhibitor_half_size) {
      soft_reduced.emplace(acquire(extent.width, extent.height));
      AmountsReduceKernel<<<grid, image_block, 0, stream>>>(a, w, h, PointerRgb(*soft_reduced),
                                                            columns, rows);
      source = PointerRgb(*soft_reduced);
    }
    soft_middle.emplace(acquire(extent.width, extent.height));
    soft_image.emplace(acquire(extent.width, extent.height));
    BlurHorizontalKernel<<<grid, image_block, 0, stream>>>(source, PointerRgb(*soft_middle), columns,
                                                           rows, image.inhibitor_x);
    BlurVerticalKernel<<<grid, image_block, 0, stream>>>(
        static_cast<const float3*>(PointerRgb(*soft_middle)), PointerRgb(*soft_image), columns, rows,
        image.inhibitor_y, 1.0f, false);
    soft = PointerRgb(*soft_image);
    clock.Mark("FP6 blurred amounts of the negative grade");
  }

  // FP6: the canonical image of the blurred transmittance, or on a light table of the blurred
  // display light, then the fused point stages.
  std::optional<Lease> temporary_flare;
  const float4*        flare       = nullptr;
  const bool           light_table = params.light_table != 0U;
  if (image.flare_active) {
    const auto extent = image.flare_extent;
    const auto id     = light_table ? FilmLoupeFlareResultId(plan.drt.node_id)
                                    : FilmFlareResultId(plan.drt.node_id);
    flare             = ReadOrBuildCanvasResult(
        device, plan, id, extent, long_edge, temporary_flare, [&](float4* target) {
          auto       reduced = acquire(canvas.width, canvas.height);
          const auto cw      = static_cast<int>(canvas.width);
          const auto ch      = static_cast<int>(canvas.height);
          CanvasReduceKernel<<<GridFor(canvas.width, canvas.height), image_block, 0, stream>>>(
              a, w, h, Pointer(reduced), cw, ch, image.canvas_to_render,
              static_cast<int>(image.canvas_samples),
              light_table ? CanvasValue::kDisplayLight : CanvasValue::kTransmittance, rgba, params);
          const auto           columns = static_cast<int>(extent.width);
          const auto           rows    = static_cast<int>(extent.height);
          const auto           grid    = GridFor(extent.width, extent.height);
          auto                 middle  = acquire(extent.width, extent.height);
          // A canvas with a small flare sigma has no second reduction.
          std::optional<Lease> coarse;
          float4*              source = Pointer(reduced);
          if (extent.width != canvas.width || extent.height != canvas.height) {
            coarse.emplace(acquire(extent.width, extent.height));
            source = Pointer(*coarse);
            BoxReduceKernel<<<grid, image_block, 0, stream>>>(Pointer(reduced), cw, ch, source,
                                                                          columns, rows);
          }
          BlurHorizontalKernel<<<grid, image_block, 0, stream>>>(
              static_cast<const float4*>(source), Pointer(middle), columns, rows, image.flare_x);
          BlurVerticalKernel<<<grid, image_block, 0, stream>>>(
              static_cast<const float4*>(Pointer(middle)), target, columns, rows, image.flare_y,
              1.0f, false);
        });
    clock.Mark("FP6 flare canvas");
  }
  // Phase G5: the grain of the negative. The FP6 kernel adds it to the record of each pixel, before
  // the flare amounts and the table read, as the apply step of the grain tile kernel. The blurred
  // amounts, the flare canvas, and the prescan read the amounts with no grain: they are means over
  // many pixels, where the grain has a mean of 0, so their canonical results do not depend on it.
  constexpr const char*                  kCaller = "RecordCudaFilmPrint";
  std::optional<FilmGrainFieldParams>    grain_field;
  std::optional<FilmGrainNegativeParams> grain_params;
  if (grain.has_value() && grain->enabled) {
    grain_field = cuda_film_grain::FieldParamsFor(*grain, plan.geometry, width, height, kCaller);
    grain_params =
        MakeFilmGrainNegativeParams(*grain, grain->reversal ? bundle.Reversal(film.negative).dye_max
                                                            : std::span<const double>{});
  }
  auto* output = static_cast<float4*>(CudaSceneTexture(device, display).DevicePointer());
  if (params.scan != 0U) {
    if (auto_colour) {
      // Rule 'frame_auto_colour': the prescan of the full frame, a canonical result. The host reads
      // only the prescan and computes the shift, as the lab does; the per-pixel pass applies it.
      const auto&          frame        = plan.geometry.edit_extent;
      const auto           prescan_plan = film_negative_scan::PrescanPlanOf(frame.height, frame.width,
                                                                            height, width);
      const ImageExtent    extent{static_cast<std::uint32_t>(prescan_plan.columns),
                                  static_cast<std::uint32_t>(prescan_plan.rows)};
      std::optional<Lease> temporary_prescan;
      const float4*        prescan      = ReadOrBuildCanvasResult(
          device, plan, FilmScanPrescanResultId(plan.drt.node_id), extent, long_edge,
          temporary_prescan, [&](float4* target) {
            const auto windows = backend.AcquireKeyedBuffer(
                prescan_plan.windows_key, std::as_bytes(std::span(prescan_plan.windows)), context);
            const auto* window = static_cast<const std::int32_t*>(windows.device_pointer);
            auto        middle = acquire(extent.width, height);
            const auto  cols   = static_cast<int>(extent.width);
            const auto windows_per_render = static_cast<std::uint64_t>(extent.width) * height;
            const auto row_blocks         = static_cast<unsigned>(
                (windows_per_render + kPrescanWindows - 1) / kPrescanWindows);
            ScanPrescanRowsKernel<<<row_blocks, dim3{kPrescanLanes, kPrescanWindows}, 0,
                                    stream>>>(a, w, h, rgba, params, window, cols,
                                              PointerRgb(middle));
            ScanPrescanColumnsKernel<<<GridFor(extent.width, extent.height), image_block, 0,
                                       stream>>>(PointerRgb(middle), cols, h, window,
                                                 static_cast<int>(extent.height), target);
          });
      clock.Mark("FP6 prescan");
      std::vector<float> texels(static_cast<std::size_t>(extent.width) * extent.height * 4U);
      cuda::CheckCuda(::cudaMemcpyAsync(texels.data(), prescan, texels.size() * sizeof(float),
                                        cudaMemcpyDeviceToHost, stream),
                      "RecordCudaFilmPrint: prescan read");
      cuda::CheckCuda(::cudaStreamSynchronize(stream), "RecordCudaFilmPrint: prescan read");
      const auto result = film_negative_scan::AutoColourOfDensityPrescan(
          texels, extent.height, extent.width, data->negative_scan, film.scan);
      ++device.PassStats().film_scan_prescan_reads;
      device.PassStats().film_scan_auto_shift = result.shift;
      scan_terms = film_negative_scan::MakeRenderTerms(data->negative_scan, film.scan, result.shift);
      clock.Mark("FP6 prescan read and auto colour on the host");
    }
    FilmKernelNegativeScan terms{};
    for (std::size_t k = 0; k < 3; ++k) {
      terms.slope[k]  = static_cast<float>(scan_terms->slope[k]);
      terms.offset[k] = static_cast<float>(scan_terms->offset[k]);
      terms.scale[k]  = static_cast<float>(scan_terms->scale[k]);
      terms.floor[k]  = static_cast<float>(scan_terms->floor[k]);
    }
    // The form ends in linear AP1: the OpenDRT of the document shows it. The resolver gives
    // Negative Scan the transform of method OpenDRT.
    const ScanDisplayArgs scan_args{a,
                                    scene_pixels,
                                    output,
                                    w,
                                    rgba,
                                    params,
                                    terms,
                                    device.DrtRuntime().Pack(resolved),
                                    plan.film_scan_linear_output};
    if (grain_field.has_value()) {
      cuda_film_grain::Launch(stream, GrainScanDisplayApply{scan_args}, *grain_params, *grain_field,
                              width, height, kCaller);
      ++device.PassStats().film_grain_negative;
      clock.Mark("FP6 scan and display transform, with the grain of the negative");
    } else {
      ScanDisplayKernel<<<blocks, kBlock, 0, stream>>>(scan_args, pixels);
      clock.Mark("FP6 scan and display transform");
    }
    cuda::CheckCuda(::cudaGetLastError(), "RecordCudaFilmPrint: kernel launch");
    clock.Finish();
    return;
  }
  const DisplayArgs display_args{a,
                                 scene_pixels,
                                 output,
                                 w,
                                 h,
                                 rgba,
                                 flare,
                                 static_cast<int>(image.flare_extent.width),
                                 static_cast<int>(image.flare_extent.height),
                                 image.render_to_flare,
                                 image.flare,
                                 soft,
                                 static_cast<int>(image.inhibitor_extent.width),
                                 static_cast<int>(image.inhibitor_extent.height),
                                 image.inhibitor_half_size,
                                 params};
  if (grain_field.has_value()) {
    cuda_film_grain::Launch(stream, GrainDisplayApply{display_args}, *grain_params, *grain_field,
                            width, height, kCaller);
    ++device.PassStats().film_grain_negative;
    clock.Mark("FP6 display, with the grain of the negative");
  } else {
    DisplayKernel<<<image_grid, image_block, 0, stream>>>(display_args);
    clock.Mark("FP6 display");
  }
  cuda::CheckCuda(::cudaGetLastError(), "RecordCudaFilmPrint: kernel launch");
  clock.Finish();
}

}  // namespace alcedo
