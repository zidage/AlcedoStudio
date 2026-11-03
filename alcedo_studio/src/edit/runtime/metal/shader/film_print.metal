//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Film print display transform on Metal: steps FP1 to FP6 of the Film Print DRT Plan, with the same
// rules as cuda_film_print_pass.cu and film_print.cl. This file has its own AIR file, compiled with
// no fast math as the CUDA and OpenCL programs are, and the DRT metallib links it.
//
// The images between the steps are row-major buffers with 3 floats per pixel. They are not float3:
// a float3 of Metal has 16 bytes. Each one is an allocation in the transient arena of the render,
// and `setBuffer` gives its offset. The canonical Halation and flare results are RGBA32F textures,
// because they have a place in the result cache. The scene and the display are scene textures.
//
// The fixed values and the blur kernels are `constant` arguments from `setBytes`, which allows
// 4 KB. metal_film_print_pass.mm binds the argument indices that this file states.

#include <metal_stdlib>

using namespace metal;

#include "../../../../include/color/color_encoding_math.h"
#include "../../../../include/edit/runtime/aces_reference_gamut_compression.h"
#include "../../../../include/edit/runtime/film/film_print_math.h"

// kFilmBlurMaxRadius + 1 and kFilmNarrowSpreadMaxRadius + 1 of film_print_plan.hpp.
#define FILM_BLUR_TAPS   129
#define FILM_NARROW_TAPS 3

struct FilmBlurKernel {
  uint  radius;
  float weights[3][FILM_BLUR_TAPS];
};

struct FilmNarrowKernel {
  uint  radius;
  float x[3][FILM_NARROW_TAPS];
  float y[3][FILM_NARROW_TAPS];
};

/// Row major, as Matrix3x3 of the host.
struct FilmMatrix {
  float m[9];
};

/// Width and height of an image, in pixels.
struct FilmSize {
  int width;
  int height;
};

// metal_film_print_pass.mm has the same values in its static_assert declarations.
static_assert(sizeof(FilmBlurKernel) == 1552, "FilmBlurKernel differs from the host");
static_assert(sizeof(FilmNarrowKernel) == 76, "FilmNarrowKernel differs from the host");
static_assert(sizeof(FilmKernelParams) == 392, "FilmKernelParams differs from the host");
static_assert(sizeof(FilmMatrix) == 36, "FilmMatrix differs from the host");
static_assert(sizeof(FilmKernelRgb) == 12, "FilmKernelRgb differs from the host");

static inline FilmKernelRgb FilmLoad(const device float* image, uint index) {
  const device float* texel = image + 3U * index;
  return FilmKernelMakeRgb(texel[0], texel[1], texel[2]);
}

static inline void FilmStore(device float* image, uint index, FilmKernelRgb value) {
  device float* texel = image + 3U * index;
  texel[0]            = value.r;
  texel[1]            = value.g;
  texel[2]            = value.b;
}

static inline float2 FilmTransform(constant FilmMatrix& matrix, float x, float y) {
  return float2(matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
                matrix.m[3] * x + matrix.m[4] * y + matrix.m[5]);
}

/// The boundary rule 'reflect' of scipy.ndimage: (d c b a | a b c d | d c b a).
static inline int FilmReflect(int position, int size) {
  if (position >= 0 && position < size) return position;
  const int period = 2 * size;
  position %= period;
  if (position < 0) position += period;
  return position < size ? position : period - 1 - position;
}

/// The bilinear read of the lab, where texel `i` covers [i, i + 1).
static inline FilmKernelRgb FilmReadBilinear(const device float* image, int width, int height,
                                             float px, float py) {
  const float         x  = fmin(fmax(px - 0.5f, 0.0f), float(width - 1));
  const float         y  = fmin(fmax(py - 0.5f, 0.0f), float(height - 1));
  const int           x0 = int(x);
  const int           y0 = int(y);
  const int           x1 = min(x0 + 1, width - 1);
  const int           y1 = min(y0 + 1, height - 1);
  const float         fx = x - float(x0);
  const float         fy = y - float(y0);
  const float         gx = 1.0f - fx;
  const float         gy = 1.0f - fy;
  const FilmKernelRgb a  = FilmLoad(image, uint(y0 * width + x0));
  const FilmKernelRgb b  = FilmLoad(image, uint(y0 * width + x1));
  const FilmKernelRgb c  = FilmLoad(image, uint(y1 * width + x0));
  const FilmKernelRgb d  = FilmLoad(image, uint(y1 * width + x1));
  return FilmKernelMakeRgb(gy * (gx * a.r + fx * b.r) + fy * (gx * c.r + fx * d.r),
                           gy * (gx * a.g + fx * b.g) + fy * (gx * c.g + fx * d.g),
                           gy * (gx * a.b + fx * b.b) + fy * (gx * c.b + fx * d.b));
}

/// The same read of a canonical result.
static inline FilmKernelRgb FilmReadBilinearTexture(texture2d<float, access::read> image, float px,
                                                    float py) {
  const int    width  = int(image.get_width());
  const int    height = int(image.get_height());
  const float  x      = fmin(fmax(px - 0.5f, 0.0f), float(width - 1));
  const float  y      = fmin(fmax(py - 0.5f, 0.0f), float(height - 1));
  const int    x0     = int(x);
  const int    y0     = int(y);
  const int    x1     = min(x0 + 1, width - 1);
  const int    y1     = min(y0 + 1, height - 1);
  const float  fx     = x - float(x0);
  const float  fy     = y - float(y0);
  const float  gx     = 1.0f - fx;
  const float  gy     = 1.0f - fy;
  const float4 a      = image.read(uint2(x0, y0));
  const float4 b      = image.read(uint2(x1, y0));
  const float4 c      = image.read(uint2(x0, y1));
  const float4 d      = image.read(uint2(x1, y1));
  return FilmKernelMakeRgb(gy * (gx * a.x + fx * b.x) + fy * (gx * c.x + fx * d.x),
                           gy * (gx * a.y + fx * b.y) + fy * (gx * c.y + fx * d.y),
                           gy * (gx * a.z + fx * b.z) + fy * (gx * c.z + fx * d.z));
}

/// The first source pixel of cell @p index, for cells that cover the image exactly.
static inline int FilmBoxCellStart(int index, int size, int count) {
  return int((long(index) * long(size) + long(count) - 1L) / long(count));
}

/// The first source pixel of cell @p index of a half-size image with @p count cells: a cell has two
/// pixels, and the last cell of an odd size has three.
static inline int FilmHalfCellStart(int index, int size, int count) {
  return index == count ? size : 2 * index;
}

/// One step at pixel (@p x, @p y), with a half-size inhibitor.
static inline FilmKernelRgb FilmStepWithHalfSizeInhibitor(FilmKernelRgb log_q, int x, int y,
                                                          const device float* inhibitor,
                                                          FilmSize                  cells,
                                                          const device float*       values,
                                                          constant FilmKernelParams& params) {
  const FilmKernelRgb i = FilmReadBilinear(inhibitor, cells.width, cells.height,
                                           (float(x) + 0.5f) * 0.5f, (float(y) + 0.5f) * 0.5f);
  return FilmKernelStep(&params, values, params.inhibition_image_k, log_q, i);
}

/// `(1 - strength) * value + strength * blurred`, per layer.
static inline FilmKernelRgb FilmMixHalation(FilmKernelRgb value, FilmKernelRgb blurred,
                                            FilmKernelRgb strength) {
  return FilmKernelMakeRgb((1.0f - strength.r) * value.r + strength.r * blurred.r,
                           (1.0f - strength.g) * value.g + strength.g * blurred.g,
                           (1.0f - strength.b) * value.b + strength.b * blurred.b);
}

/// FP4 at one pixel, from the exposure @p value. With a blur of the inhibitor, @p a takes the first
/// amounts and @p q the log exposure of the later steps. With no blur, @p a takes the final amounts.
static inline void FilmFirstStage(FilmKernelRgb value, device float* q, device float* a, uint index,
                                  bool inhibitor_blur, const device float* values,
                                  constant FilmKernelParams& params) {
  if (inhibitor_blur) {
    const FilmKernelFirstStage first = FilmKernelFirstAmountsWithPointPart(&params, values, value);
    FilmStore(q, index, first.log_q);
    FilmStore(a, index, first.amounts);
  } else {
    FilmStore(a, index, FilmKernelPointAmounts(&params, values, value));
  }
}

/// One tap sum of a vertical pass of @p taps at column @p x of row @p y.
static inline FilmKernelRgb FilmBlurColumn(const device float* src, int x, int y, FilmSize size,
                                           constant FilmBlurKernel& taps) {
  const FilmKernelRgb center = FilmLoad(src, uint(y * size.width + x));
  float               r      = taps.weights[0][0] * center.r;
  float               g      = taps.weights[1][0] * center.g;
  float               b      = taps.weights[2][0] * center.b;
  const int           radius = int(taps.radius);
  for (int tap = 1; tap <= radius; ++tap) {
    const FilmKernelRgb top =
        FilmLoad(src, uint(FilmReflect(y - tap, size.height) * size.width + x));
    const FilmKernelRgb bottom =
        FilmLoad(src, uint(FilmReflect(y + tap, size.height) * size.width + x));
    r += taps.weights[0][tap] * (top.r + bottom.r);
    g += taps.weights[1][tap] * (top.g + bottom.g);
    b += taps.weights[2][tap] * (top.b + bottom.b);
  }
  return FilmKernelMakeRgb(r, g, b);
}

/// FP1. Input is linear AP1. A colour that a grade moved out of AP1 comes back at this point.
kernel void film_print_exposure(texture2d<float, access::read> scene [[texture(0)]],
                                device float*                  q [[buffer(0)]],
                                const device float*            values [[buffer(1)]],
                                constant FilmKernelParams&     params [[buffer(2)]],
                                constant FilmSize&             size [[buffer(3)]],
                                uint2                          gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const float4        source     = scene.read(gid);
  const AcesRgcRgb    compressed = AcesReferenceGamutCompress(source.x, source.y, source.z);
  const FilmKernelRgb exposure =
      FilmKernelLayerExposure(&params, values, compressed.r, compressed.g, compressed.b);
  FilmStore(q, gid.y * uint(size.width) + gid.x, exposure);
}

kernel void film_print_first_amounts(device float*              q [[buffer(0)]],
                                     device float*              a [[buffer(1)]],
                                     const device float*        values [[buffer(2)]],
                                     constant FilmKernelParams& params [[buffer(3)]],
                                     constant FilmSize&         size [[buffer(4)]],
                                     constant int&              inhibitor_blur [[buffer(5)]],
                                     uint2                      gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint index = gid.y * uint(size.width) + gid.x;
  FilmFirstStage(FilmLoad(q, index), q, a, index, inhibitor_blur != 0, values, params);
}

/// FP3 and FP4 in one dispatch. See `FilmFirstStage` for what @p q and @p a take.
kernel void film_print_halation_first_amounts(
    device float* q [[buffer(0)]], device float* a [[buffer(1)]],
    const device float* values [[buffer(2)]], constant FilmKernelParams& params [[buffer(3)]],
    constant FilmSize& size [[buffer(4)]], constant int& inhibitor_blur [[buffer(5)]],
    constant FilmMatrix& render_to_canvas [[buffer(6)]],
    constant FilmKernelRgb& strength [[buffer(7)]],
    texture2d<float, access::read> canvas [[texture(0)]],
    uint2                          gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint          index    = gid.y * uint(size.width) + gid.x;
  const float2        position = FilmTransform(render_to_canvas, float(gid.x) + 0.5f,
                                               float(gid.y) + 0.5f);
  const FilmKernelRgb blurred  = FilmReadBilinearTexture(canvas, position.x, position.y);
  const FilmKernelRgb mix      = FilmMixHalation(FilmLoad(q, index), blurred, strength);
  FilmFirstStage(mix, q, a, index, inhibitor_blur != 0, values, params);
}

/// FP4 for a half-size inhibitor, with FP3 when @p has_canvas is not 0. The amounts are only the
/// input of the first blur, so this form writes their box mean. @p q takes the log exposure of the
/// later steps. One thread covers one cell.
kernel void film_print_reduced_first_amounts(
    device float* q [[buffer(0)]], device float* reduced [[buffer(1)]],
    const device float* values [[buffer(2)]], constant FilmKernelParams& params [[buffer(3)]],
    constant FilmSize& size [[buffer(4)]], constant FilmSize& cells [[buffer(5)]],
    constant int& has_canvas [[buffer(6)]], constant FilmMatrix& render_to_canvas [[buffer(7)]],
    constant FilmKernelRgb& strength [[buffer(8)]],
    texture2d<float, access::read> canvas [[texture(0)]],
    uint2                          gid [[thread_position_in_grid]]) {
  const int j = int(gid.x);
  const int i = int(gid.y);
  if (i >= cells.height || j >= cells.width) return;
  const int y0 = FilmHalfCellStart(i, size.height, cells.height);
  const int y1 = FilmHalfCellStart(i + 1, size.height, cells.height);
  const int x0 = FilmHalfCellStart(j, size.width, cells.width);
  const int x1 = FilmHalfCellStart(j + 1, size.width, cells.width);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const uint    index = uint(y * size.width + x);
      FilmKernelRgb value = FilmLoad(q, index);
      if (has_canvas != 0) {
        const float2 position = FilmTransform(render_to_canvas, float(x) + 0.5f, float(y) + 0.5f);
        value = FilmMixHalation(value, FilmReadBilinearTexture(canvas, position.x, position.y),
                                strength);
      }
      const FilmKernelFirstStage first = FilmKernelFirstAmountsWithPointPart(&params, values, value);
      FilmStore(q, index, first.log_q);
      r += first.amounts.r;
      g += first.amounts.g;
      b += first.amounts.b;
    }
  }
  const float scale = 1.0f / float((y1 - y0) * (x1 - x0));
  FilmStore(reduced, uint(i * cells.width + j), FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

/// The last step with a half-size inhibitor.
kernel void film_print_step_half_size(const device float*        log_q [[buffer(0)]],
                                      device float*              amounts [[buffer(1)]],
                                      const device float*        inhibitor [[buffer(2)]],
                                      const device float*        values [[buffer(3)]],
                                      constant FilmKernelParams& params [[buffer(4)]],
                                      constant FilmSize&         size [[buffer(5)]],
                                      constant FilmSize&         cells [[buffer(6)]],
                                      uint2                      gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint index = gid.y * uint(size.width) + gid.x;
  FilmStore(amounts, index,
            FilmStepWithHalfSizeInhibitor(FilmLoad(log_q, index), int(gid.x), int(gid.y),
                                          inhibitor, cells, values, params));
}

/// A step with a half-size inhibitor before the last step: the output is only the input of the next
/// blur, so this form writes its box mean. One thread covers one cell.
kernel void film_print_step_to_reduced(const device float*        log_q [[buffer(0)]],
                                       const device float*        inhibitor [[buffer(1)]],
                                       device float*              reduced [[buffer(2)]],
                                       const device float*        values [[buffer(3)]],
                                       constant FilmKernelParams& params [[buffer(4)]],
                                       constant FilmSize&         size [[buffer(5)]],
                                       constant FilmSize&         cells [[buffer(6)]],
                                       uint2                      gid [[thread_position_in_grid]]) {
  const int j = int(gid.x);
  const int i = int(gid.y);
  if (i >= cells.height || j >= cells.width) return;
  const int y0 = FilmHalfCellStart(i, size.height, cells.height);
  const int y1 = FilmHalfCellStart(i + 1, size.height, cells.height);
  const int x0 = FilmHalfCellStart(j, size.width, cells.width);
  const int x1 = FilmHalfCellStart(j + 1, size.width, cells.width);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const FilmKernelRgb a = FilmStepWithHalfSizeInhibitor(
          FilmLoad(log_q, uint(y * size.width + x)), x, y, inhibitor, cells, values, params);
      r += a.r;
      g += a.g;
      b += a.b;
    }
  }
  const float scale = 1.0f / float((y1 - y0) * (x1 - x0));
  FilmStore(reduced, uint(i * cells.width + j), FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

/// FP4 and FP5 in one dispatch at full size: the vertical pass of the inhibitor blur, then one step.
/// @p middle holds the horizontal pass.
kernel void film_print_step_vertical(const device float*        middle [[buffer(0)]],
                                     const device float*        log_q [[buffer(1)]],
                                     device float*              amounts [[buffer(2)]],
                                     const device float*        values [[buffer(3)]],
                                     constant FilmKernelParams& params [[buffer(4)]],
                                     constant FilmSize&         size [[buffer(5)]],
                                     constant FilmBlurKernel&   taps [[buffer(6)]],
                                     uint2                      gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint          index   = gid.y * uint(size.width) + gid.x;
  const FilmKernelRgb blurred = FilmBlurColumn(middle, int(gid.x), int(gid.y), size, taps);
  FilmStore(amounts, index,
            FilmKernelStep(&params, values, params.inhibition_image_k, FilmLoad(log_q, index),
                           blurred));
}

kernel void film_print_blur_horizontal(const device float*      src [[buffer(0)]],
                                       device float*            dst [[buffer(1)]],
                                       constant FilmSize&       size [[buffer(2)]],
                                       constant FilmBlurKernel& taps [[buffer(3)]],
                                       uint2                    gid [[thread_position_in_grid]]) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  if (x >= size.width || y >= size.height) return;
  const uint          row    = uint(y * size.width);
  const FilmKernelRgb center = FilmLoad(src, row + uint(x));
  float               r      = taps.weights[0][0] * center.r;
  float               g      = taps.weights[1][0] * center.g;
  float               b      = taps.weights[2][0] * center.b;
  const int           radius = int(taps.radius);
  for (int tap = 1; tap <= radius; ++tap) {
    const FilmKernelRgb left  = FilmLoad(src, row + uint(FilmReflect(x - tap, size.width)));
    const FilmKernelRgb right = FilmLoad(src, row + uint(FilmReflect(x + tap, size.width)));
    r += taps.weights[0][tap] * (left.r + right.r);
    g += taps.weights[1][tap] * (left.g + right.g);
    b += taps.weights[2][tap] * (left.b + right.b);
  }
  FilmStore(dst, row + uint(x), FilmKernelMakeRgb(r, g, b));
}

/// `dst = weight * blur`, or with @p accumulate, `dst += weight * blur`. The first form must not read
/// the output, because a new transient allocation can hold NaN.
kernel void film_print_blur_vertical(const device float*      src [[buffer(0)]],
                                     device float*            dst [[buffer(1)]],
                                     constant FilmSize&       size [[buffer(2)]],
                                     constant FilmBlurKernel& taps [[buffer(3)]],
                                     constant float&          weight [[buffer(4)]],
                                     constant int&            accumulate [[buffer(5)]],
                                     uint2                    gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint          index = gid.y * uint(size.width) + gid.x;
  const FilmKernelRgb blur  = FilmBlurColumn(src, int(gid.x), int(gid.y), size, taps);
  if (accumulate != 0) {
    const FilmKernelRgb out = FilmLoad(dst, index);
    FilmStore(dst, index,
              FilmKernelMakeRgb(out.r + weight * blur.r, out.g + weight * blur.g,
                                out.b + weight * blur.b));
  } else {
    FilmStore(dst, index, FilmKernelMakeRgb(weight * blur.r, weight * blur.g, weight * blur.b));
  }
}

/// The last pass of a canonical result. The canonical results keep opaque alpha.
kernel void film_print_blur_vertical_to_texture(const device float*             src [[buffer(0)]],
                                                constant FilmSize&              size [[buffer(1)]],
                                                constant FilmBlurKernel&        taps [[buffer(2)]],
                                                texture2d<float, access::write> dst [[texture(0)]],
                                                uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const FilmKernelRgb blur = FilmBlurColumn(src, int(gid.x), int(gid.y), size, taps);
  dst.write(float4(blur.r, blur.g, blur.b, 1.0f), gid);
}

/// FP2 in one vertical dispatch: `dst = wide_weight * wide + narrow_weight * narrow`. @p middle holds
/// the horizontal pass of the wide Gaussian. The narrow Gaussian reads @p exposure directly.
kernel void film_print_spread_vertical(const device float*        middle [[buffer(0)]],
                                       const device float*        exposure [[buffer(1)]],
                                       device float*              dst [[buffer(2)]],
                                       constant FilmSize&         size [[buffer(3)]],
                                       constant FilmBlurKernel&   wide [[buffer(4)]],
                                       constant float&            wide_weight [[buffer(5)]],
                                       constant FilmNarrowKernel& narrow [[buffer(6)]],
                                       constant float&            narrow_weight [[buffer(7)]],
                                       uint2                      gid [[thread_position_in_grid]]) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  if (x >= size.width || y >= size.height) return;
  const FilmKernelRgb wide_sum = FilmBlurColumn(middle, x, y, size, wide);
  float               nr       = 0.0f;
  float               ng       = 0.0f;
  float               nb       = 0.0f;
  const int           nradius  = int(narrow.radius);
  for (int dy = -nradius; dy <= nradius; ++dy) {
    const uint row = uint(FilmReflect(y + dy, size.height) * size.width);
    const int  ay  = dy < 0 ? -dy : dy;
    for (int dx = -nradius; dx <= nradius; ++dx) {
      const FilmKernelRgb value = FilmLoad(exposure, row + uint(FilmReflect(x + dx, size.width)));
      const int           ax    = dx < 0 ? -dx : dx;
      nr += narrow.y[0][ay] * narrow.x[0][ax] * value.r;
      ng += narrow.y[1][ay] * narrow.x[1][ax] * value.g;
      nb += narrow.y[2][ay] * narrow.x[2][ax] * value.b;
    }
  }
  FilmStore(dst, uint(y * size.width + x),
            FilmKernelMakeRgb(wide_weight * wide_sum.r + narrow_weight * nr,
                              wide_weight * wide_sum.g + narrow_weight * ng,
                              wide_weight * wide_sum.b + narrow_weight * nb));
}

/// Box mean with cells that cover the image exactly, as the lab reduces an image.
kernel void film_print_box_reduce(const device float* src [[buffer(0)]],
                                  device float*       dst [[buffer(1)]],
                                  constant FilmSize&  src_size [[buffer(2)]],
                                  constant FilmSize&  cells [[buffer(3)]],
                                  uint2               gid [[thread_position_in_grid]]) {
  const int j = int(gid.x);
  const int i = int(gid.y);
  if (i >= cells.height || j >= cells.width) return;
  const int y0 = FilmBoxCellStart(i, src_size.height, cells.height);
  const int y1 = FilmBoxCellStart(i + 1, src_size.height, cells.height);
  const int x0 = FilmBoxCellStart(j, src_size.width, cells.width);
  const int x1 = FilmBoxCellStart(j + 1, src_size.width, cells.width);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const FilmKernelRgb value = FilmLoad(src, uint(y * src_size.width + x));
      r += value.r;
      g += value.g;
      b += value.b;
    }
  }
  const float scale = 1.0f / float((y1 - y0) * (x1 - x0));
  FilmStore(dst, uint(i * cells.width + j), FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

/// Canvas from the render: the mean of @p samples x @p samples render pixels in the footprint of one
/// texel. With @p transmittance, the values are amounts and the mean is of 10^-a.
kernel void film_print_canvas_reduce(const device float*  src [[buffer(0)]],
                                     device float*        dst [[buffer(1)]],
                                     constant FilmSize&   size [[buffer(2)]],
                                     constant FilmSize&   dst_size [[buffer(3)]],
                                     constant FilmMatrix& canvas_to_render [[buffer(4)]],
                                     constant int&        samples [[buffer(5)]],
                                     constant int&        transmittance [[buffer(6)]],
                                     uint2                gid [[thread_position_in_grid]]) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  if (x >= dst_size.width || y >= dst_size.height) return;
  const float step = 1.0f / float(samples);
  float       r    = 0.0f;
  float       g    = 0.0f;
  float       b    = 0.0f;
  for (int j = 0; j < samples; ++j) {
    const float cy = float(y) + (float(j) + 0.5f) * step;
    for (int i = 0; i < samples; ++i) {
      const float         cx     = float(x) + (float(i) + 0.5f) * step;
      const float2        render = FilmTransform(canvas_to_render, cx, cy);
      const int           sx     = min(max(int(floor(render.x)), 0), size.width - 1);
      const int           sy     = min(max(int(floor(render.y)), 0), size.height - 1);
      const FilmKernelRgb value  = FilmLoad(src, uint(sy * size.width + sx));
      if (transmittance != 0) {
        r += FilmKernelTransmittance(value.r);
        g += FilmKernelTransmittance(value.g);
        b += FilmKernelTransmittance(value.b);
      } else {
        r += value.r;
        g += value.g;
        b += value.b;
      }
    }
  }
  const float scale = step * step;
  FilmStore(dst, uint(y * dst_size.width + x), FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

/// Rule 'negative_show_grade': the box mean of the amounts on the half-size image. One thread covers
/// one cell.
kernel void film_print_amounts_reduce(const device float* amounts [[buffer(0)]],
                                      device float*       reduced [[buffer(1)]],
                                      constant FilmSize&  size [[buffer(2)]],
                                      constant FilmSize&  cells [[buffer(3)]],
                                      uint2               gid [[thread_position_in_grid]]) {
  const int j = int(gid.x);
  const int i = int(gid.y);
  if (i >= cells.height || j >= cells.width) return;
  const int y0 = FilmHalfCellStart(i, size.height, cells.height);
  const int y1 = FilmHalfCellStart(i + 1, size.height, cells.height);
  const int x0 = FilmHalfCellStart(j, size.width, cells.width);
  const int x1 = FilmHalfCellStart(j + 1, size.width, cells.width);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const FilmKernelRgb value = FilmLoad(amounts, uint(y * size.width + x));
      r += value.r;
      g += value.g;
      b += value.b;
    }
  }
  const float scale = 1.0f / float((y1 - y0) * (x1 - x0));
  FilmStore(reduced, uint(i * cells.width + j), FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

/// The values of FP6 that change per render, apart from the fixed values of the selection.
struct FilmDisplayArguments {
  FilmMatrix render_to_flare;
  float      flare_strength;
  int        has_flare;       ///< 0: the Enlarger flare is off
  int        has_soft;        ///< 0: no image plane; the graded record reads itself
  int        soft_half_size;  ///< Not 0: @p soft has the half-size cells of the inhibitor
  FilmSize   soft_cells;
};
static_assert(sizeof(FilmDisplayArguments) == 60, "FilmDisplayArguments differs from the host");

/// FP6, fused. Alpha comes from the scene.
kernel void film_print_display(const device float*             amounts [[buffer(0)]],
                               const device float*             print_rgba [[buffer(1)]],
                               const device float*             soft [[buffer(2)]],
                               constant FilmKernelParams&      params [[buffer(3)]],
                               constant FilmSize&              size [[buffer(4)]],
                               constant FilmDisplayArguments&  arguments [[buffer(5)]],
                               texture2d<float, access::read>  scene [[texture(0)]],
                               texture2d<float, access::write> display [[texture(1)]],
                               texture2d<float, access::read>  flare [[texture(2)]],
                               uint2                           gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint    index       = gid.y * uint(size.width) + gid.x;
  FilmKernelRgb record      = FilmLoad(amounts, index);
  FilmKernelRgb soft_record = record;
  if (params.negative_grade != 0U && arguments.has_soft != 0) {
    soft_record = arguments.soft_half_size != 0
                      ? FilmReadBilinear(soft, arguments.soft_cells.width,
                                         arguments.soft_cells.height, (float(gid.x) + 0.5f) * 0.5f,
                                         (float(gid.y) + 0.5f) * 0.5f)
                      : FilmLoad(soft, index);
  }
  if (arguments.has_flare != 0) {
    const float2 position =
        FilmTransform(arguments.render_to_flare, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
    record = FilmKernelFlareAmounts(record, FilmReadBilinearTexture(flare, position.x, position.y),
                                    arguments.flare_strength);
  }
  if (params.negative_grade != 0U) {
    if (arguments.has_soft == 0) soft_record = record;
    record = FilmKernelNegativeGrade(&params, record, soft_record);
  }
  const FilmKernelRgb finished =
      FilmKernelFinishDisplay(&params, FilmKernelPrintDisplay(&params, print_rgba, record));
  const FilmKernelRgb linear = params.cinema_look != 0U ? FilmKernelCinemaLook(finished) : finished;
  const int           tf     = int(params.display_encoding);
  display.write(float4(CeDisplayEncodeChannel(tf, linear.r, 1.0f),
                       CeDisplayEncodeChannel(tf, linear.g, 1.0f),
                       CeDisplayEncodeChannel(tf, linear.b, 1.0f), scene.read(gid).w),
                gid);
}
