//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Film print display transform on Metal: steps FP1 to FP6 of the Film Print DRT Plan, with the same
// rules as cuda_film_print_pass.cu and film_print.cl. This file has its own AIR file, and the DRT
// metallib links it.
//
// The images between the steps are row-major buffers in the transient arena of the render, and
// `setBuffer` gives the offset of each one. The form of a texel follows the cost on Apple GPUs,
// where most passes are limited by the bytes that they move:
// - Images that a later point stage reads at their own pixel (Q, the spread Q, log Q, and the final
//   dye amounts) are packed_float3, 12 bytes. Q has no upper limit and the log of a small Q needs
//   its precision, and the print table reads the amounts directly, so they keep float32.
// - Images that only a blur or a bilinear read uses (the amounts before the last step of the
//   inhibitor, the middle image and the half-size cells of the inhibitor, the blurred amounts of
//   the negative grade, and the Halation, flare, and loupe flare canvases before their blur) are
//   half4, 8 bytes. They hold densities of a few units, a transmittance in 0 to 1, the linear
//   display light of a light table in 0 to about 1, or the exposure that Halation scales by its
//   strength, and the blur averages their rounding.
// The canonical Halation, flare, and loupe flare results are RGBA32F textures, because they have a
// place in the result cache. The scene and the display are scene textures.
//
// A colour reversal film (`params.light_table`) has the same FP1 to FP5. Its FP6 reads the light
// table in place of the print table, and adds the loupe flare and the surround in display light.
//
// A blur pass gives each thread FILM_SLIDE outputs along the blur. A window of FILM_SLIDE values in
// registers moves one tap at a time, so a pass loads 2 R + FILM_SLIDE values for FILM_SLIDE outputs
// in place of 2 R + 1 values for each output. The tap sums are those of the CUDA kernels, in
// another order.
//
// The fixed values and the blur weights are `constant` arguments from `setBytes`, which allows
// 4 KB. metal_film_print_pass.mm binds the argument indices that this file states.

#include <metal_stdlib>

using namespace metal;

#include "../../../../include/color/color_encoding_math.h"
#include "../../../../include/edit/runtime/aces_reference_gamut_compression.h"
#include "../../../../include/edit/runtime/film/film_print_math.h"

// kFilmBlurMaxRadius + 1 and kFilmNarrowSpreadMaxRadius + 1 of film_print_plan.hpp.
#define FILM_BLUR_TAPS   129
#define FILM_NARROW_TAPS 3
// Outputs of one thread of a blur pass, and taps of one chunk of its loop.
// metal_film_print_pass.mm has the same value.
#define FILM_SLIDE        8
// The last chunk of a blur loop reads up to FILM_SLIDE - 1 taps past the radius.
#define FILM_BLUR_WEIGHTS (FILM_BLUR_TAPS + FILM_SLIDE - 1)

/// FilmBlurKernel of the host, with the weights of the three layers of one tap in one float4. The
/// host writes 0 past the radius.
struct FilmBlurWeights {
  uint   radius;
  uint   unused[3];
  float4 w[FILM_BLUR_WEIGHTS];
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
static_assert(sizeof(FilmBlurWeights) == 2192, "FilmBlurWeights differs from the host");
static_assert(sizeof(FilmNarrowKernel) == 76, "FilmNarrowKernel differs from the host");
static_assert(sizeof(FilmKernelParams) == 412, "FilmKernelParams differs from the host");
static_assert(sizeof(FilmMatrix) == 36, "FilmMatrix differs from the host");
static_assert(sizeof(FilmKernelRgb) == 12, "FilmKernelRgb differs from the host");

static inline float3 FilmLoad3(const device packed_float3* image, uint index) {
  return float3(image[index]);
}

static inline float3 FilmLoad3(const device half4* image, uint index) {
  return float3(image[index].xyz);
}

template <typename Texel>
static inline FilmKernelRgb FilmLoad(const device Texel* image, uint index) {
  const float3 v = FilmLoad3(image, index);
  return FilmKernelMakeRgb(v.x, v.y, v.z);
}

static inline void FilmStore3(device packed_float3* image, uint index, float3 value) {
  image[index] = packed_float3(value);
}

static inline void FilmStore3(device half4* image, uint index, float3 value) {
  image[index] = half4(half3(value), 0.0h);
}

template <typename Texel>
static inline void FilmStore(device Texel* image, uint index, FilmKernelRgb value) {
  FilmStore3(image, index, float3(value.r, value.g, value.b));
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
template <typename Texel>
static inline FilmKernelRgb FilmReadBilinear(const device Texel* image, int width, int height,
                                             float px, float py) {
  const float  x  = fmin(fmax(px - 0.5f, 0.0f), float(width - 1));
  const float  y  = fmin(fmax(py - 0.5f, 0.0f), float(height - 1));
  const int    x0 = int(x);
  const int    y0 = int(y);
  const int    x1 = min(x0 + 1, width - 1);
  const int    y1 = min(y0 + 1, height - 1);
  const float  fx = x - float(x0);
  const float  fy = y - float(y0);
  const float3 a  = FilmLoad3(image, uint(y0 * width + x0));
  const float3 b  = FilmLoad3(image, uint(y0 * width + x1));
  const float3 c  = FilmLoad3(image, uint(y1 * width + x0));
  const float3 d  = FilmLoad3(image, uint(y1 * width + x1));
  const float3 v  = (1.0f - fy) * ((1.0f - fx) * a + fx * b) + fy * ((1.0f - fx) * c + fx * d);
  return FilmKernelMakeRgb(v.x, v.y, v.z);
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
  const float3 a      = image.read(uint2(x0, y0)).xyz;
  const float3 b      = image.read(uint2(x1, y0)).xyz;
  const float3 c      = image.read(uint2(x0, y1)).xyz;
  const float3 d      = image.read(uint2(x1, y1)).xyz;
  const float3 v      = (1.0f - fy) * ((1.0f - fx) * a + fx * b) + fy * ((1.0f - fx) * c + fx * d);
  return FilmKernelMakeRgb(v.x, v.y, v.z);
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
                                                          const device half4*        inhibitor,
                                                          FilmSize                   cells,
                                                          const device float*        values,
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
template <typename Amounts>
static inline void FilmFirstStage(FilmKernelRgb value, device packed_float3* q, device Amounts* a,
                                  uint index, bool inhibitor_blur, const device float* values,
                                  constant FilmKernelParams& params) {
  if (inhibitor_blur) {
    const FilmKernelFirstStage first = FilmKernelFirstAmountsWithPointPart(&params, values, value);
    FilmStore(q, index, first.log_q);
    FilmStore(a, index, first.amounts);
  } else {
    FilmStore(a, index, FilmKernelPointAmounts(&params, values, value));
  }
}

/// Tap sums of FILM_SLIDE outputs of one blur pass.
struct FilmSlideSums {
  float3 v[FILM_SLIDE];
};

/**
 * Tap sums of FILM_SLIDE outputs at positions p0 to p0 + FILM_SLIDE - 1 of one line, where
 * @p load(p) reads position p. The loop takes FILM_SLIDE taps per chunk. In a chunk that starts at
 * tap t0, output k at tap t0 + j reads position p0 + t0 + j + k, so the chunk reads 2 FILM_SLIDE -
 * 1 positions from the banks `low` (t0 to t0 + 7) and `high` (t0 + 8 to t0 + 15). The indices are
 * constant, and the banks move once per chunk. Taps past the radius have the weight 0.
 */
template <typename Load>
static inline FilmSlideSums FilmBlurLine(Load load, int p0, constant FilmBlurWeights& taps) {
  using Value          = decltype(load(0));
  const int     radius = int(taps.radius);
  FilmSlideSums sums;
  Value         low[FILM_SLIDE];
  Value         high[FILM_SLIDE];
  for (int k = 0; k < FILM_SLIDE; ++k) {
    sums.v[k] = 0.0f;
    low[k]    = load(p0 - radius + k);
    high[k]   = load(p0 - radius + FILM_SLIDE + k);
  }
  for (int t0 = -radius; t0 <= radius; t0 += FILM_SLIDE) {
    for (int j = 0; j < FILM_SLIDE; ++j) {
      const int    t = t0 + j;
      const float3 w = taps.w[t < 0 ? -t : t].xyz;
      for (int k = 0; k < FILM_SLIDE; ++k) {
        const int m = j + k;
        sums.v[k] += w * float3(m < FILM_SLIDE ? low[m] : high[m - FILM_SLIDE]);
      }
    }
    for (int k = 0; k < FILM_SLIDE; ++k) {
      low[k]  = high[k];
      high[k] = load(p0 + t0 + 2 * FILM_SLIDE + k);
    }
  }
  return sums;
}

/// A texel in the window of a blur: half texels stay half, which uses half the registers and loses
/// nothing. The tap sums are float.
static inline float3 FilmWindowValue(const device packed_float3* image, uint index) {
  return float3(image[index]);
}

static inline half3 FilmWindowValue(const device half4* image, uint index) {
  return image[index].xyz;
}

/// Reads position p of a line at `base + p * step`, with p kept inside the line by `min` or by
/// rule 'reflect'.
template <typename Texel, bool Interior>
struct FilmLineLoad {
  const device Texel* base;
  int                 step;
  int                 size;

  auto                operator()(int p) const -> decltype(FilmWindowValue(base, 0U)) {
    const int q = Interior ? min(p, size - 1) : FilmReflect(p, size);
    return FilmWindowValue(base, uint(q * step));
  }
};

/**
 * The tap sums of FilmBlurLine with one bank: at tap t the window holds positions p0 + k + t, and
 * it moves one position per tap. It uses fewer registers than FilmBlurLine, so a kernel that does
 * more work after its blur keeps more threads in flight.
 */
template <typename Load>
static inline FilmSlideSums FilmBlurSlide(Load load, int p0, constant FilmBlurWeights& taps) {
  using Value          = decltype(load(0));
  const int     radius = int(taps.radius);
  FilmSlideSums sums;
  Value         window[FILM_SLIDE];
  for (int k = 0; k < FILM_SLIDE; ++k) {
    sums.v[k] = 0.0f;
    window[k] = load(p0 - radius + k);
  }
  for (int t = -radius; t <= radius; ++t) {
    const float3 w = taps.w[t < 0 ? -t : t].xyz;
    for (int k = 0; k < FILM_SLIDE; ++k) sums.v[k] += w * float3(window[k]);
    for (int k = 0; k + 1 < FILM_SLIDE; ++k) window[k] = window[k + 1];
    window[FILM_SLIDE - 1] = load(p0 + t + FILM_SLIDE);
  }
  return sums;
}

/// The vertical form of FilmBlurSlide, for column @p x at rows @p y0 to @p y0 + FILM_SLIDE - 1.
template <typename Texel>
static inline FilmSlideSums FilmBlurDownSlide(const device Texel* src, int x, int y0, FilmSize size,
                                              constant FilmBlurWeights& taps) {
  const int radius = int(taps.radius);
  if (y0 - radius >= 0 && y0 + FILM_SLIDE + radius <= size.height) {
    return FilmBlurSlide(FilmLineLoad<Texel, true>{src + x, size.width, size.height}, y0, taps);
  }
  return FilmBlurSlide(FilmLineLoad<Texel, false>{src + x, size.width, size.height}, y0, taps);
}

/**
 * The vertical blur of column @p x at rows @p y0 to @p y0 + FILM_SLIDE - 1. A block that the reach
 * of the blur keeps inside the image has no reflect; its reads past that reach have the weight 0
 * and stay inside the image.
 */
template <typename Texel>
static inline FilmSlideSums FilmBlurDown(const device Texel* src, int x, int y0, FilmSize size,
                                         constant FilmBlurWeights& taps) {
  const int radius = int(taps.radius);
  if (y0 - radius >= 0 && y0 + FILM_SLIDE + radius <= size.height) {
    return FilmBlurLine(FilmLineLoad<Texel, true>{src + x, size.width, size.height}, y0, taps);
  }
  return FilmBlurLine(FilmLineLoad<Texel, false>{src + x, size.width, size.height}, y0, taps);
}

/// The horizontal form of FilmBlurDown, for columns @p x0 to @p x0 + FILM_SLIDE - 1 of a row.
template <typename Texel>
static inline FilmSlideSums FilmBlurAcross(const device Texel* row, int x0, int width,
                                           constant FilmBlurWeights& taps) {
  const int radius = int(taps.radius);
  if (x0 - radius >= 0 && x0 + FILM_SLIDE + radius <= width) {
    return FilmBlurLine(FilmLineLoad<Texel, true>{row, 1, width}, x0, taps);
  }
  return FilmBlurLine(FilmLineLoad<Texel, false>{row, 1, width}, x0, taps);
}

/// FP1. Input is linear AP1. A colour that a grade moved out of AP1 comes back at this point.
kernel void film_print_exposure(
    texture2d<float, access::read> scene [[texture(0)]], device packed_float3* q [[buffer(0)]],
    const device float* values [[buffer(1)]], constant FilmKernelParams& params [[buffer(2)]],
    constant FilmSize& size [[buffer(3)]], device atomic_uint* alpha_mark [[buffer(4)]],
    constant uint& stamp [[buffer(5)]], uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const float4 source = scene.read(gid);
  // See film_print_display. One lane of a SIMD group writes the mark.
  if (simd_any(source.w != 1.0f) && simd_is_first()) {
    atomic_store_explicit(alpha_mark, stamp, memory_order_relaxed);
  }
  const AcesRgcRgb    compressed = AcesReferenceGamutCompress(source.x, source.y, source.z);
  const FilmKernelRgb exposure =
      FilmKernelLayerExposure(&params, values, compressed.r, compressed.g, compressed.b);
  FilmStore(q, gid.y * uint(size.width) + gid.x, exposure);
}

/// FP4. The `_half` form writes first amounts that only the inhibitor blur reads.
template <typename Amounts>
kernel void film_print_first_amounts(device packed_float3*      q [[buffer(0)]],
                                     device Amounts*            a [[buffer(1)]],
                                     const device float*        values [[buffer(2)]],
                                     constant FilmKernelParams& params [[buffer(3)]],
                                     constant FilmSize&         size [[buffer(4)]],
                                     constant int&              inhibitor_blur [[buffer(5)]],
                                     uint2                      gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint index = gid.y * uint(size.width) + gid.x;
  FilmFirstStage(FilmLoad(q, index), q, a, index, inhibitor_blur != 0, values, params);
}

template [[host_name("film_print_first_amounts_float")]] kernel void
film_print_first_amounts<packed_float3>(device packed_float3*, device packed_float3*,
                                        const device float*, constant FilmKernelParams&,
                                        constant FilmSize&, constant int&, uint2);
template [[host_name("film_print_first_amounts_half")]] kernel void film_print_first_amounts<half4>(
    device packed_float3*, device half4*, const device float*, constant FilmKernelParams&,
    constant FilmSize&, constant int&, uint2);

/// FP3 and FP4 in one dispatch. See `FilmFirstStage` for what @p q and @p a take, and
/// film_print_first_amounts for the forms.
template <typename Amounts>
kernel void film_print_halation_first_amounts(
    device packed_float3* q [[buffer(0)]], device Amounts* a [[buffer(1)]],
    const device float* values [[buffer(2)]], constant FilmKernelParams& params [[buffer(3)]],
    constant FilmSize& size [[buffer(4)]], constant int& inhibitor_blur [[buffer(5)]],
    constant FilmMatrix&           render_to_canvas [[buffer(6)]],
    constant FilmKernelRgb&        strength [[buffer(7)]],
    texture2d<float, access::read> canvas [[texture(0)]], uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint   index    = gid.y * uint(size.width) + gid.x;
  const float2 position = FilmTransform(render_to_canvas, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
  const FilmKernelRgb blurred = FilmReadBilinearTexture(canvas, position.x, position.y);
  const FilmKernelRgb mix     = FilmMixHalation(FilmLoad(q, index), blurred, strength);
  FilmFirstStage(mix, q, a, index, inhibitor_blur != 0, values, params);
}

template [[host_name("film_print_halation_first_amounts_float")]] kernel void
film_print_halation_first_amounts<packed_float3>(device packed_float3*, device packed_float3*,
                                                 const device float*, constant FilmKernelParams&,
                                                 constant FilmSize&, constant int&,
                                                 constant FilmMatrix&, constant FilmKernelRgb&,
                                                 texture2d<float, access::read>, uint2);
template [[host_name("film_print_halation_first_amounts_half")]] kernel void
film_print_halation_first_amounts<half4>(device packed_float3*, device half4*, const device float*,
                                         constant FilmKernelParams&, constant FilmSize&,
                                         constant int&, constant FilmMatrix&,
                                         constant FilmKernelRgb&, texture2d<float, access::read>,
                                         uint2);

/// FP4 for a half-size inhibitor, with FP3 when @p has_canvas is not 0. The amounts are only the
/// input of the first blur, so this form writes their box mean. @p q takes the log exposure of the
/// later steps. One thread covers one cell.
kernel void film_print_reduced_first_amounts(
    device packed_float3* q [[buffer(0)]], device half4* reduced [[buffer(1)]],
    const device float* values [[buffer(2)]], constant FilmKernelParams& params [[buffer(3)]],
    constant FilmSize& size [[buffer(4)]], constant FilmSize& cells [[buffer(5)]],
    constant int& has_canvas [[buffer(6)]], constant FilmMatrix& render_to_canvas [[buffer(7)]],
    constant FilmKernelRgb&        strength [[buffer(8)]],
    texture2d<float, access::read> canvas [[texture(0)]], uint2 gid [[thread_position_in_grid]]) {
  const int j = int(gid.x);
  const int i = int(gid.y);
  if (i >= cells.height || j >= cells.width) return;
  const int y0  = FilmHalfCellStart(i, size.height, cells.height);
  const int y1  = FilmHalfCellStart(i + 1, size.height, cells.height);
  const int x0  = FilmHalfCellStart(j, size.width, cells.width);
  const int x1  = FilmHalfCellStart(j + 1, size.width, cells.width);
  float3    sum = 0.0f;
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
      sum += float3(first.amounts.r, first.amounts.g, first.amounts.b);
    }
  }
  FilmStore3(reduced, uint(i * cells.width + j), sum / float((y1 - y0) * (x1 - x0)));
}

/// The last step with a half-size inhibitor.
kernel void film_print_step_half_size(
    const device packed_float3* log_q [[buffer(0)]], device packed_float3* amounts [[buffer(1)]],
    const device half4* inhibitor [[buffer(2)]], const device float* values [[buffer(3)]],
    constant FilmKernelParams& params [[buffer(4)]], constant FilmSize& size [[buffer(5)]],
    constant FilmSize& cells [[buffer(6)]], uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint index = gid.y * uint(size.width) + gid.x;
  FilmStore(amounts, index,
            FilmStepWithHalfSizeInhibitor(FilmLoad(log_q, index), int(gid.x), int(gid.y),
                                          inhibitor, cells, values, params));
}

/// A step with a half-size inhibitor before the last step: the output is only the input of the next
/// blur, so this form writes its box mean. One thread covers one cell.
kernel void film_print_step_to_reduced(
    const device packed_float3* log_q [[buffer(0)]], const device half4* inhibitor [[buffer(1)]],
    device half4* reduced [[buffer(2)]], const device float* values [[buffer(3)]],
    constant FilmKernelParams& params [[buffer(4)]], constant FilmSize& size [[buffer(5)]],
    constant FilmSize& cells [[buffer(6)]], uint2 gid [[thread_position_in_grid]]) {
  const int j = int(gid.x);
  const int i = int(gid.y);
  if (i >= cells.height || j >= cells.width) return;
  const int y0  = FilmHalfCellStart(i, size.height, cells.height);
  const int y1  = FilmHalfCellStart(i + 1, size.height, cells.height);
  const int x0  = FilmHalfCellStart(j, size.width, cells.width);
  const int x1  = FilmHalfCellStart(j + 1, size.width, cells.width);
  float3    sum = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const FilmKernelRgb a = FilmStepWithHalfSizeInhibitor(
          FilmLoad(log_q, uint(y * size.width + x)), x, y, inhibitor, cells, values, params);
      sum += float3(a.r, a.g, a.b);
    }
  }
  FilmStore3(reduced, uint(i * cells.width + j), sum / float((y1 - y0) * (x1 - x0)));
}

/// FP4 and FP5 in one dispatch at full size: the vertical pass of the inhibitor blur, then one
/// step.
/// @p middle holds the horizontal pass. One thread covers FILM_SLIDE rows of a column. The `_half`
/// form is for a step before the last, whose amounts only the next blur reads.
template <typename Amounts>
kernel void film_print_step_vertical(
    const device half4* middle [[buffer(0)]], const device packed_float3* log_q [[buffer(1)]],
    device Amounts* amounts [[buffer(2)]], const device float* values [[buffer(3)]],
    constant FilmKernelParams& params [[buffer(4)]], constant FilmSize& size [[buffer(5)]],
    constant FilmBlurWeights& taps [[buffer(6)]], uint2 gid [[thread_position_in_grid]]) {
  const int x  = int(gid.x);
  const int y0 = int(gid.y) * FILM_SLIDE;
  if (x >= size.width || y0 >= size.height) return;
  const FilmSlideSums sums = FilmBlurDownSlide(middle, x, y0, size, taps);
  for (int k = 0; k < FILM_SLIDE && y0 + k < size.height; ++k) {
    const uint index = uint((y0 + k) * size.width + x);
    FilmStore(amounts, index,
              FilmKernelStep(&params, values, params.inhibition_image_k, FilmLoad(log_q, index),
                             FilmKernelMakeRgb(sums.v[k].x, sums.v[k].y, sums.v[k].z)));
  }
}

template [[host_name("film_print_step_vertical_float")]] kernel void
film_print_step_vertical<packed_float3>(const device half4*, const device packed_float3*,
                                        device packed_float3*, const device float*,
                                        constant FilmKernelParams&, constant FilmSize&,
                                        constant FilmBlurWeights&, uint2);
template [[host_name("film_print_step_vertical_half")]] kernel void film_print_step_vertical<half4>(
    const device half4*, const device packed_float3*, device half4*, const device float*,
    constant FilmKernelParams&, constant FilmSize&, constant FilmBlurWeights&, uint2);

/// One thread covers FILM_SLIDE columns of a row.
template <typename Source, typename Texel>
kernel void film_print_blur_horizontal(const device Source*      src [[buffer(0)]],
                                       device Texel*             dst [[buffer(1)]],
                                       constant FilmSize&        size [[buffer(2)]],
                                       constant FilmBlurWeights& taps [[buffer(3)]],
                                       uint2                     gid [[thread_position_in_grid]]) {
  const int x0 = int(gid.x) * FILM_SLIDE;
  const int y  = int(gid.y);
  if (x0 >= size.width || y >= size.height) return;
  const uint          row  = uint(y * size.width);
  const FilmSlideSums sums = FilmBlurAcross(src + row, x0, size.width, taps);
  for (int k = 0; k < FILM_SLIDE && x0 + k < size.width; ++k) {
    FilmStore3(dst, row + uint(x0 + k), sums.v[k]);
  }
}

/// `dst = weight * blur`, or with @p accumulate, `dst += weight * blur`. The first form must not
/// read the output, because a new transient allocation can hold NaN. One thread covers FILM_SLIDE
/// rows.
template <typename Source, typename Texel>
kernel void film_print_blur_vertical(const device Source*      src [[buffer(0)]],
                                     device Texel*             dst [[buffer(1)]],
                                     constant FilmSize&        size [[buffer(2)]],
                                     constant FilmBlurWeights& taps [[buffer(3)]],
                                     constant float&           weight [[buffer(4)]],
                                     constant int&             accumulate [[buffer(5)]],
                                     uint2                     gid [[thread_position_in_grid]]) {
  const int x  = int(gid.x);
  const int y0 = int(gid.y) * FILM_SLIDE;
  if (x >= size.width || y0 >= size.height) return;
  const FilmSlideSums sums = FilmBlurDown(src, x, y0, size, taps);
  for (int k = 0; k < FILM_SLIDE && y0 + k < size.height; ++k) {
    const uint index = uint((y0 + k) * size.width + x);
    if (accumulate != 0) {
      FilmStore3(dst, index, FilmLoad3(dst, index) + weight * sums.v[k]);
    } else {
      FilmStore3(dst, index, weight * sums.v[k]);
    }
  }
}

/// The last pass of a canonical result, from half texels. The canonical results keep opaque alpha.
kernel void film_print_blur_vertical_to_texture(const device half4*             src [[buffer(0)]],
                                                constant FilmSize&              size [[buffer(1)]],
                                                constant FilmBlurWeights&       taps [[buffer(2)]],
                                                texture2d<float, access::write> dst [[texture(0)]],
                                                uint2 gid [[thread_position_in_grid]]) {
  const int x  = int(gid.x);
  const int y0 = int(gid.y) * FILM_SLIDE;
  if (x >= size.width || y0 >= size.height) return;
  const FilmSlideSums sums = FilmBlurDown(src, x, y0, size, taps);
  for (int k = 0; k < FILM_SLIDE && y0 + k < size.height; ++k) {
    dst.write(float4(sums.v[k], 1.0f), uint2(x, y0 + k));
  }
}

template [[host_name("film_print_blur_horizontal_float_float")]] kernel void
film_print_blur_horizontal<packed_float3, packed_float3>(const device packed_float3*,
                                                         device packed_float3*, constant FilmSize&,
                                                         constant FilmBlurWeights&, uint2);
template [[host_name("film_print_blur_horizontal_float_half")]] kernel void
film_print_blur_horizontal<packed_float3, half4>(const device packed_float3*, device half4*,
                                                 constant FilmSize&, constant FilmBlurWeights&,
                                                 uint2);
template [[host_name("film_print_blur_horizontal_half_half")]] kernel void
film_print_blur_horizontal<half4, half4>(const device half4*, device half4*, constant FilmSize&,
                                         constant FilmBlurWeights&, uint2);
template [[host_name("film_print_blur_vertical_float_float")]] kernel void
film_print_blur_vertical<packed_float3, packed_float3>(const device packed_float3*,
                                                       device packed_float3*, constant FilmSize&,
                                                       constant FilmBlurWeights&, constant float&,
                                                       constant int&, uint2);
template [[host_name("film_print_blur_vertical_half_half")]] kernel void
film_print_blur_vertical<half4, half4>(const device half4*, device half4*, constant FilmSize&,
                                       constant FilmBlurWeights&, constant float&, constant int&,
                                       uint2);

/// FP2 in one vertical dispatch: `dst = wide_weight * wide + narrow_weight * narrow`. @p middle
/// holds the horizontal pass of the wide Gaussian. The narrow Gaussian reads @p exposure directly.
/// One thread covers FILM_SLIDE rows of a column.
kernel void film_print_spread_vertical(
    const device packed_float3* middle [[buffer(0)]],
    const device packed_float3* exposure [[buffer(1)]], device packed_float3* dst [[buffer(2)]],
    constant FilmSize& size [[buffer(3)]], constant FilmBlurWeights& wide [[buffer(4)]],
    constant float& wide_weight [[buffer(5)]], constant FilmNarrowKernel& narrow [[buffer(6)]],
    constant float& narrow_weight [[buffer(7)]], uint2 gid [[thread_position_in_grid]]) {
  const int x  = int(gid.x);
  const int y0 = int(gid.y) * FILM_SLIDE;
  if (x >= size.width || y0 >= size.height) return;
  const FilmSlideSums wide_sums = FilmBlurDownSlide(middle, x, y0, size, wide);
  const int           nradius   = int(narrow.radius);
  for (int k = 0; k < FILM_SLIDE && y0 + k < size.height; ++k) {
    const int y  = y0 + k;
    float3    nb = 0.0f;
    for (int dy = -nradius; dy <= nradius; ++dy) {
      const uint   row = uint(FilmReflect(y + dy, size.height) * size.width);
      const int    ay  = dy < 0 ? -dy : dy;
      const float3 wy  = float3(narrow.y[0][ay], narrow.y[1][ay], narrow.y[2][ay]);
      for (int dx = -nradius; dx <= nradius; ++dx) {
        const int ax = dx < 0 ? -dx : dx;
        nb += wy * float3(narrow.x[0][ax], narrow.x[1][ax], narrow.x[2][ax]) *
              FilmLoad3(exposure, row + uint(FilmReflect(x + dx, size.width)));
      }
    }
    FilmStore3(dst, uint(y * size.width + x), wide_weight * wide_sums.v[k] + narrow_weight * nb);
  }
}

/// Box mean with cells that cover the image exactly, as the lab reduces an image.
kernel void film_print_box_reduce(const device half4* src [[buffer(0)]],
                                  device half4*       dst [[buffer(1)]],
                                  constant FilmSize&  src_size [[buffer(2)]],
                                  constant FilmSize&  cells [[buffer(3)]],
                                  uint2               gid [[thread_position_in_grid]]) {
  const int j = int(gid.x);
  const int i = int(gid.y);
  if (i >= cells.height || j >= cells.width) return;
  const int y0  = FilmBoxCellStart(i, src_size.height, cells.height);
  const int y1  = FilmBoxCellStart(i + 1, src_size.height, cells.height);
  const int x0  = FilmBoxCellStart(j, src_size.width, cells.width);
  const int x1  = FilmBoxCellStart(j + 1, src_size.width, cells.width);
  float3    sum = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) sum += FilmLoad3(src, uint(y * src_size.width + x));
  }
  FilmStore3(dst, uint(i * cells.width + j), sum / float((y1 - y0) * (x1 - x0)));
}

/// The value of a canvas texel: the exposure, for Halation.
struct FilmCanvasExposure {
  float3 operator()(float3 value) const { return value; }
};

/// 10^-a of the amounts, for the Enlarger flare of a print.
struct FilmCanvasTransmittance {
  float3 operator()(float3 value) const {
    return float3(FilmKernelTransmittance(value.x), FilmKernelTransmittance(value.y),
                  FilmKernelTransmittance(value.z));
  }
};

/// The linear display light of the light table, for the loupe flare of a colour reversal film: FP6
/// to the display matrix, the same value that film_print_display mixes with the blurred canvas.
struct FilmCanvasDisplayLight {
  constant FilmKernelParams* params;
  const device float*        print_rgba;
  float3                     operator()(float3 value) const {
    const FilmKernelRgb light =
        FilmKernelPrintDisplay(params, print_rgba, FilmKernelMakeRgb(value.x, value.y, value.z));
    return float3(light.r, light.g, light.b);
  }
};

/// Canvas from the render: the mean of @p samples x @p samples render pixels in the footprint of
/// one texel, of the value that @p texel_value gives for a source pixel.
template <typename Source, typename Texel, typename Value>
static inline void FilmCanvasReduce(const device Source* src, device Texel* dst, FilmSize size,
                                    FilmSize dst_size, constant FilmMatrix& canvas_to_render,
                                    int samples, Value texel_value, uint2 gid) {
  const int x = int(gid.x);
  const int y = int(gid.y);
  if (x >= dst_size.width || y >= dst_size.height) return;
  const float step = 1.0f / float(samples);
  float3      sum  = 0.0f;
  for (int j = 0; j < samples; ++j) {
    const float cy = float(y) + (float(j) + 0.5f) * step;
    for (int i = 0; i < samples; ++i) {
      const float  cx     = float(x) + (float(i) + 0.5f) * step;
      const float2 render = FilmTransform(canvas_to_render, cx, cy);
      const int    sx     = min(max(int(floor(render.x)), 0), size.width - 1);
      const int    sy     = min(max(int(floor(render.y)), 0), size.height - 1);
      sum += texel_value(FilmLoad3(src, uint(sy * size.width + sx)));
    }
  }
  // The texels are half. An exposure above the largest finite half keeps that value, so the blur
  // has no infinity; it is more than 16 stops above the criterion of the curves.
  FilmStore3(dst, uint(y * dst_size.width + x), fmin(sum * (step * step), float3(HALF_MAX)));
}

kernel void film_print_canvas_reduce_exposure(const device packed_float3* src [[buffer(0)]],
                                              device half4*               dst [[buffer(1)]],
                                              constant FilmSize&          size [[buffer(2)]],
                                              constant FilmSize&          dst_size [[buffer(3)]],
                                              constant FilmMatrix& canvas_to_render [[buffer(4)]],
                                              constant int&        samples [[buffer(5)]],
                                              uint2 gid [[thread_position_in_grid]]) {
  FilmCanvasReduce(src, dst, size, dst_size, canvas_to_render, samples, FilmCanvasExposure{}, gid);
}

kernel void film_print_canvas_reduce_transmittance(
    const device packed_float3* src [[buffer(0)]], device half4* dst [[buffer(1)]],
    constant FilmSize& size [[buffer(2)]], constant FilmSize& dst_size [[buffer(3)]],
    constant FilmMatrix& canvas_to_render [[buffer(4)]], constant int& samples [[buffer(5)]],
    uint2 gid [[thread_position_in_grid]]) {
  FilmCanvasReduce(src, dst, size, dst_size, canvas_to_render, samples, FilmCanvasTransmittance{},
                   gid);
}

/// The loupe flare canvas of a colour reversal film. @p print_rgba is the light table.
kernel void film_print_canvas_reduce_display_light(
    const device packed_float3* src [[buffer(0)]], device half4* dst [[buffer(1)]],
    constant FilmSize& size [[buffer(2)]], constant FilmSize& dst_size [[buffer(3)]],
    constant FilmMatrix& canvas_to_render [[buffer(4)]], constant int& samples [[buffer(5)]],
    const device float* print_rgba [[buffer(6)]], constant FilmKernelParams& params [[buffer(7)]],
    uint2 gid [[thread_position_in_grid]]) {
  FilmCanvasReduce(src, dst, size, dst_size, canvas_to_render, samples,
                   FilmCanvasDisplayLight{&params, print_rgba}, gid);
}

/// Rule 'negative_show_grade': the box mean of the amounts on the half-size image. One thread covers
/// one cell.
kernel void film_print_amounts_reduce(const device packed_float3* amounts [[buffer(0)]],
                                      device half4*               reduced [[buffer(1)]],
                                      constant FilmSize&          size [[buffer(2)]],
                                      constant FilmSize&          cells [[buffer(3)]],
                                      uint2                       gid [[thread_position_in_grid]]) {
  const int j = int(gid.x);
  const int i = int(gid.y);
  if (i >= cells.height || j >= cells.width) return;
  const int y0  = FilmHalfCellStart(i, size.height, cells.height);
  const int y1  = FilmHalfCellStart(i + 1, size.height, cells.height);
  const int x0  = FilmHalfCellStart(j, size.width, cells.width);
  const int x1  = FilmHalfCellStart(j + 1, size.width, cells.width);
  float3    sum = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) sum += FilmLoad3(amounts, uint(y * size.width + x));
  }
  FilmStore3(reduced, uint(i * cells.width + j), sum / float((y1 - y0) * (x1 - x0)));
}

/// The values of FP6 that change per render, apart from the fixed values of the selection.
struct FilmDisplayArguments {
  FilmMatrix render_to_flare;
  float      flare_strength;
  int        has_flare;       ///< 0: the Enlarger flare, or the loupe flare, is off
  int        has_soft;        ///< 0: no image plane; the graded record reads itself
  int        soft_half_size;  ///< Not 0: @p soft has the half-size cells of the inhibitor
  FilmSize   soft_cells;
};
static_assert(sizeof(FilmDisplayArguments) == 60, "FilmDisplayArguments differs from the host");

/// FP6 of a print: the Enlarger flare in density, the negative grade, the print table, the display
/// compression, and the cinema look. Linear Rec. 709 in 0 to 1.
static inline FilmKernelRgb FilmPrintFinishedLinear(
    constant FilmKernelParams& params, FilmKernelRgb record, uint2 gid, uint index,
    const device float* print_rgba, const device half4* soft,
    constant FilmDisplayArguments& arguments, texture2d<float, access::read> flare) {
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
  return params.cinema_look != 0U ? FilmKernelCinemaLook(finished) : finished;
}

/// FP6, fused. Alpha comes from the scene. On a light table, @p print_rgba is the light table,
/// @p flare is the blurred display light of the loupe flare, and @p soft is not read.
kernel void film_print_display(
    const device packed_float3* amounts [[buffer(0)]], const device float* print_rgba [[buffer(1)]],
    const device half4* soft [[buffer(2)]], constant FilmKernelParams& params [[buffer(3)]],
    constant FilmSize& size [[buffer(4)]], constant FilmDisplayArguments& arguments [[buffer(5)]],
    texture2d<float, access::read>  scene [[texture(0)]],
    texture2d<float, access::write> display [[texture(1)]],
    texture2d<float, access::read>  flare [[texture(2)]],
    const device uint* alpha_mark [[buffer(6)]], constant uint& stamp [[buffer(7)]],
    uint2 gid [[thread_position_in_grid]]) {
  if (int(gid.x) >= size.width || int(gid.y) >= size.height) return;
  const uint          index  = gid.y * uint(size.width) + gid.x;
  const FilmKernelRgb record = FilmLoad(amounts, index);
  FilmKernelRgb       linear;
  if (params.light_table != 0U) {
    // Lab rules 'light_table_view', 'loupe_flare', and 'dark_surround_on_display' in display light.
    FilmKernelRgb light = FilmKernelPrintDisplay(&params, print_rgba, record);
    if (arguments.has_flare != 0) {
      const float2 position =
          FilmTransform(arguments.render_to_flare, float(gid.x) + 0.5f, float(gid.y) + 0.5f);
      light = FilmKernelLoupeFlare(light, FilmReadBilinearTexture(flare, position.x, position.y),
                                   arguments.flare_strength);
    }
    linear = FilmKernelFinishDisplay(&params, FilmKernelDisplaySurround(&params, light));
  } else {
    linear =
        FilmPrintFinishedLinear(params, record, gid, index, print_rgba, soft, arguments, flare);
  }
  const int tf = int(params.display_encoding);
  // FP1 writes the stamp of this render to the mark when a pixel has an alpha other than 1. A mark
  // with a different value means that each alpha is 1, so the scene, 16 bytes a pixel, is not read
  // again. The mark is not cleared: when old memory holds the stamp, the stage reads the scene,
  // which gives the same alpha.
  const float         alpha  = alpha_mark[0] == stamp ? scene.read(gid).w : 1.0f;
  display.write(
      float4(CeDisplayEncodeChannel(tf, linear.r, 1.0f), CeDisplayEncodeChannel(tf, linear.g, 1.0f),
             CeDisplayEncodeChannel(tf, linear.b, 1.0f), alpha),
      gid);
}
