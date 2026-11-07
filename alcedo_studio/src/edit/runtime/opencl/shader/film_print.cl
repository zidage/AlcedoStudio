//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Film print display transform on OpenCL 1.2: steps FP1 to FP6 of the Film Print DRT Plan, with the
// same rules as cuda_film_print_pass.cu. This program loads color_encoding_math.h,
// aces_reference_gamut_compression.h, film_print_math.h, film_grain_field.h, film_grain_tile.h,
// and film_grain_negative.h before this file. The last three give FP6 the grain of the negative
// (film_print_display_grain, phase G6 of the film grain plan).
//
// The images between the steps are row-major buffers with 3 floats per pixel. They are not float3:
// a float3 of OpenCL C has 16 bytes. A kernel takes each image as a buffer and a uint offset in
// floats, because all of them are allocations in the transient arena of the render. The canonical
// Halation and flare results are RGBA32F images, because they have a place in the result cache.
//
// The kernels read the fixed values through `__global` pointers. A FilmBlurKernel has more bytes
// than the 1024 of CL_DEVICE_MAX_PARAMETER_SIZE that OpenCL 1.2 requires, so it cannot be a
// by-value argument. opencl_film_print_pass.cpp uploads these values once per render.

// kFilmBlurMaxRadius + 1 and kFilmNarrowSpreadMaxRadius + 1 of film_print_plan.hpp.
#define FILM_BLUR_TAPS   129
#define FILM_NARROW_TAPS 3

typedef struct {
  uint  radius;
  float weights[3][FILM_BLUR_TAPS];
} FilmBlurKernel;

typedef struct {
  uint  radius;
  float x[3][FILM_NARROW_TAPS];
  float y[3][FILM_NARROW_TAPS];
} FilmNarrowKernel;

/// Row major, as Matrix3x3 of the host.
typedef struct {
  float m[9];
} FilmMatrix;

// A size that differs from the host stops the build of this program. opencl_film_print_pass.cpp has
// the same values in its static_assert declarations.
typedef char FilmBlurKernelSizeCheck[sizeof(FilmBlurKernel) == 1552 ? 1 : -1];
typedef char FilmNarrowKernelSizeCheck[sizeof(FilmNarrowKernel) == 76 ? 1 : -1];
typedef char FilmKernelParamsSizeCheck[sizeof(FilmKernelParams) == 416 ? 1 : -1];
typedef char FilmGrainFieldParamsSizeCheck[sizeof(FilmGrainFieldParams) == 388 ? 1 : -1];
typedef char FilmGrainNegativeParamsSizeCheck[sizeof(FilmGrainNegativeParams) == 616 ? 1 : -1];

__constant sampler_t kFilmNearestClamp =
    CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;

static inline __global const FilmKernelParams* FilmParams(__global const uchar* data, uint offset) {
  return (__global const FilmKernelParams*)(data + offset);
}

static inline __global const FilmBlurKernel* FilmBlur(__global const uchar* data, uint offset) {
  return (__global const FilmBlurKernel*)(data + offset);
}

static inline FilmKernelRgb FilmLoad(__global const float* image, uint offset, uint index) {
  __global const float* texel = image + offset + 3U * index;
  return FilmKernelMakeRgb(texel[0], texel[1], texel[2]);
}

static inline void FilmStore(__global float* image, uint offset, uint index, FilmKernelRgb value) {
  __global float* texel = image + offset + 3U * index;
  texel[0]              = value.r;
  texel[1]              = value.g;
  texel[2]              = value.b;
}

static inline float2 FilmTransform(FilmMatrix matrix, float x, float y) {
  return (float2)(matrix.m[0] * x + matrix.m[1] * y + matrix.m[2],
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
static inline FilmKernelRgb FilmReadBilinear(__global const float* image, uint offset, int width,
                                             int height, float px, float py) {
  const float         x  = fmin(fmax(px - 0.5f, 0.0f), (float)(width - 1));
  const float         y  = fmin(fmax(py - 0.5f, 0.0f), (float)(height - 1));
  const int           x0 = (int)x;
  const int           y0 = (int)y;
  const int           x1 = min(x0 + 1, width - 1);
  const int           y1 = min(y0 + 1, height - 1);
  const float         fx = x - (float)x0;
  const float         fy = y - (float)y0;
  const float         gx = 1.0f - fx;
  const float         gy = 1.0f - fy;
  const FilmKernelRgb a  = FilmLoad(image, offset, (uint)(y0 * width + x0));
  const FilmKernelRgb b  = FilmLoad(image, offset, (uint)(y0 * width + x1));
  const FilmKernelRgb c  = FilmLoad(image, offset, (uint)(y1 * width + x0));
  const FilmKernelRgb d  = FilmLoad(image, offset, (uint)(y1 * width + x1));
  return FilmKernelMakeRgb(gy * (gx * a.r + fx * b.r) + fy * (gx * c.r + fx * d.r),
                           gy * (gx * a.g + fx * b.g) + fy * (gx * c.g + fx * d.g),
                           gy * (gx * a.b + fx * b.b) + fy * (gx * c.b + fx * d.b));
}

/// The same read of a canonical result.
static inline FilmKernelRgb FilmReadBilinearImage(__read_only image2d_t image, int width, int height,
                                                  float px, float py) {
  const float  x  = fmin(fmax(px - 0.5f, 0.0f), (float)(width - 1));
  const float  y  = fmin(fmax(py - 0.5f, 0.0f), (float)(height - 1));
  const int    x0 = (int)x;
  const int    y0 = (int)y;
  const int    x1 = min(x0 + 1, width - 1);
  const int    y1 = min(y0 + 1, height - 1);
  const float  fx = x - (float)x0;
  const float  fy = y - (float)y0;
  const float  gx = 1.0f - fx;
  const float  gy = 1.0f - fy;
  const float4 a  = read_imagef(image, kFilmNearestClamp, (int2)(x0, y0));
  const float4 b  = read_imagef(image, kFilmNearestClamp, (int2)(x1, y0));
  const float4 c  = read_imagef(image, kFilmNearestClamp, (int2)(x0, y1));
  const float4 d  = read_imagef(image, kFilmNearestClamp, (int2)(x1, y1));
  return FilmKernelMakeRgb(gy * (gx * a.x + fx * b.x) + fy * (gx * c.x + fx * d.x),
                           gy * (gx * a.y + fx * b.y) + fy * (gx * c.y + fx * d.y),
                           gy * (gx * a.z + fx * b.z) + fy * (gx * c.z + fx * d.z));
}

/// The first source pixel of cell @p index, for cells that cover the image exactly.
static inline int FilmBoxCellStart(int index, int size, int count) {
  return (int)(((long)index * (long)size + (long)count - 1L) / (long)count);
}

/// The first source pixel of cell @p index of a half-size image with @p count cells: a cell has two
/// pixels, and the last cell of an odd size has three.
static inline int FilmHalfCellStart(int index, int size, int count) {
  return index == count ? size : 2 * index;
}

/// One step at pixel (@p x, @p y), with a half-size inhibitor.
static inline FilmKernelRgb FilmStepWithHalfSizeInhibitor(
    FilmKernelRgb log_q, int x, int y, __global const float* inhibitor, uint inhibitor_offset,
    int columns, int rows, __global const float* values, __global const FilmKernelParams* params) {
  const FilmKernelRgb i = FilmReadBilinear(inhibitor, inhibitor_offset, columns, rows,
                                           ((float)x + 0.5f) * 0.5f, ((float)y + 0.5f) * 0.5f);
  return FilmKernelStep(params, values, params->inhibition_image_k, log_q, i);
}

/// `(1 - strength) * value + strength * blurred`, per layer.
static inline FilmKernelRgb FilmMixHalation(FilmKernelRgb value, FilmKernelRgb blurred,
                                            FilmKernelRgb strength) {
  return FilmKernelMakeRgb((1.0f - strength.r) * value.r + strength.r * blurred.r,
                           (1.0f - strength.g) * value.g + strength.g * blurred.g,
                           (1.0f - strength.b) * value.b + strength.b * blurred.b);
}

/// FP1. Input is linear AP1. A colour that a grade moved out of AP1 comes back at this point (lab
/// rule 'entry_reference_gamut_compression'), except on Negative Scan, whose display transform has
/// its own entry compression.
__kernel void film_print_exposure(__read_only image2d_t src_image, __global const float4* src_buffer,
                                  int src_is_buffer, __global float* q, uint q_offset,
                                  __global const float* values, __global const uchar* data,
                                  uint params_offset, int width, int height) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  const float4           source = src_is_buffer != 0
                                      ? src_buffer[(uint)y * (uint)width + (uint)x]
                                      : read_imagef(src_image, kFilmNearestClamp, (int2)(x, y));
  __global const FilmKernelParams* params = FilmParams(data, params_offset);
  if (params->scan != 0U) {
    FilmStore(q, q_offset, (uint)y * (uint)width + (uint)x,
              FilmKernelLayerExposure(params, values, source.x, source.y, source.z));
    return;
  }
  const AcesRgcRgb    compressed = AcesReferenceGamutCompress(source.x, source.y, source.z);
  const FilmKernelRgb exposure =
      FilmKernelLayerExposure(params, values, compressed.r, compressed.g, compressed.b);
  FilmStore(q, q_offset, (uint)y * (uint)width + (uint)x, exposure);
}

/// FP4 at one pixel, from the exposure @p value. With a blur of the inhibitor, @p a takes the first
/// amounts and @p q the log exposure of the later steps. With no blur, @p a takes the final amounts.
static inline void FilmFirstStage(FilmKernelRgb value, __global float* q, uint q_offset,
                                  __global float* a, uint a_offset, uint index, int inhibitor_blur,
                                  __global const float* values,
                                  __global const FilmKernelParams* params) {
  if (inhibitor_blur != 0) {
    const FilmKernelFirstStage first = FilmKernelFirstAmountsWithPointPart(params, values, value);
    FilmStore(q, q_offset, index, first.log_q);
    FilmStore(a, a_offset, index, first.amounts);
  } else {
    FilmStore(a, a_offset, index, FilmKernelPointAmounts(params, values, value));
  }
}

__kernel void film_print_first_amounts(__global float* q, uint q_offset, __global float* a,
                                       uint a_offset, int inhibitor_blur,
                                       __global const float* values, __global const uchar* data,
                                       uint params_offset, int width, int height) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  const uint index = (uint)y * (uint)width + (uint)x;
  FilmFirstStage(FilmLoad(q, q_offset, index), q, q_offset, a, a_offset, index, inhibitor_blur,
                 values, FilmParams(data, params_offset));
}

/// FP3 and FP4 in one dispatch. See `FilmFirstStage` for what @p q and @p a take.
__kernel void film_print_halation_first_amounts(
    __global float* q, uint q_offset, __global float* a, uint a_offset, __read_only image2d_t canvas,
    int canvas_width, int canvas_height, FilmMatrix render_to_canvas, FilmKernelRgb strength,
    int inhibitor_blur, __global const float* values, __global const uchar* data, uint params_offset,
    int width, int height) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  const uint          index    = (uint)y * (uint)width + (uint)x;
  const float2        position = FilmTransform(render_to_canvas, (float)x + 0.5f, (float)y + 0.5f);
  const FilmKernelRgb blurred =
      FilmReadBilinearImage(canvas, canvas_width, canvas_height, position.x, position.y);
  const FilmKernelRgb mix = FilmMixHalation(FilmLoad(q, q_offset, index), blurred, strength);
  FilmFirstStage(mix, q, q_offset, a, a_offset, index, inhibitor_blur, values,
                 FilmParams(data, params_offset));
}

/// FP4 for a half-size inhibitor, with FP3 when @p has_canvas is not 0. The amounts are only the
/// input of the first blur, so this form writes their box mean. @p q takes the log exposure of the
/// later steps. One work item covers one cell.
__kernel void film_print_reduced_first_amounts(
    __global float* q, uint q_offset, int width, int height, __global float* reduced,
    uint reduced_offset, int columns, int rows, __read_only image2d_t canvas, int has_canvas,
    int canvas_width, int canvas_height, FilmMatrix render_to_canvas, FilmKernelRgb strength,
    __global const float* values, __global const uchar* data, uint params_offset) {
  const int j = (int)get_global_id(0);
  const int i = (int)get_global_id(1);
  if (i >= rows || j >= columns) return;
  __global const FilmKernelParams* params = FilmParams(data, params_offset);
  const int                        y0     = FilmHalfCellStart(i, height, rows);
  const int                        y1     = FilmHalfCellStart(i + 1, height, rows);
  const int                        x0     = FilmHalfCellStart(j, width, columns);
  const int                        x1     = FilmHalfCellStart(j + 1, width, columns);
  float                            r      = 0.0f;
  float                            g      = 0.0f;
  float                            b      = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const uint    index = (uint)y * (uint)width + (uint)x;
      FilmKernelRgb value = FilmLoad(q, q_offset, index);
      if (has_canvas != 0) {
        const float2 position = FilmTransform(render_to_canvas, (float)x + 0.5f, (float)y + 0.5f);
        value                 = FilmMixHalation(
            value,
            FilmReadBilinearImage(canvas, canvas_width, canvas_height, position.x, position.y),
            strength);
      }
      const FilmKernelFirstStage first = FilmKernelFirstAmountsWithPointPart(params, values, value);
      FilmStore(q, q_offset, index, first.log_q);
      r += first.amounts.r;
      g += first.amounts.g;
      b += first.amounts.b;
    }
  }
  const float scale = 1.0f / (float)((y1 - y0) * (x1 - x0));
  FilmStore(reduced, reduced_offset, (uint)i * (uint)columns + (uint)j,
            FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

/// The last step with a half-size inhibitor.
__kernel void film_print_step_half_size(__global const float* log_q, uint log_q_offset,
                                        __global float* amounts, uint amounts_offset, int width,
                                        int height, __global const float* inhibitor,
                                        uint inhibitor_offset, int columns, int rows,
                                        __global const float* values, __global const uchar* data,
                                        uint params_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  const uint index = (uint)y * (uint)width + (uint)x;
  FilmStore(amounts, amounts_offset, index,
            FilmStepWithHalfSizeInhibitor(FilmLoad(log_q, log_q_offset, index), x, y, inhibitor,
                                          inhibitor_offset, columns, rows, values,
                                          FilmParams(data, params_offset)));
}

/// A step with a half-size inhibitor before the last step: the output is only the input of the next
/// blur, so this form writes its box mean. One work item covers one cell.
__kernel void film_print_step_to_reduced(__global const float* log_q, uint log_q_offset, int width,
                                         int height, __global const float* inhibitor,
                                         uint inhibitor_offset, __global float* reduced,
                                         uint reduced_offset, int columns, int rows,
                                         __global const float* values, __global const uchar* data,
                                         uint params_offset) {
  const int j = (int)get_global_id(0);
  const int i = (int)get_global_id(1);
  if (i >= rows || j >= columns) return;
  __global const FilmKernelParams* params = FilmParams(data, params_offset);
  const int                        y0     = FilmHalfCellStart(i, height, rows);
  const int                        y1     = FilmHalfCellStart(i + 1, height, rows);
  const int                        x0     = FilmHalfCellStart(j, width, columns);
  const int                        x1     = FilmHalfCellStart(j + 1, width, columns);
  float                            r      = 0.0f;
  float                            g      = 0.0f;
  float                            b      = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const FilmKernelRgb a = FilmStepWithHalfSizeInhibitor(
          FilmLoad(log_q, log_q_offset, (uint)y * (uint)width + (uint)x), x, y, inhibitor,
          inhibitor_offset, columns, rows, values, params);
      r += a.r;
      g += a.g;
      b += a.b;
    }
  }
  const float scale = 1.0f / (float)((y1 - y0) * (x1 - x0));
  FilmStore(reduced, reduced_offset, (uint)i * (uint)columns + (uint)j,
            FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

/// One tap sum of a vertical pass of @p taps at column @p x of row @p y.
static inline FilmKernelRgb FilmBlurColumn(__global const float* src, uint src_offset, int x, int y,
                                           int width, int height,
                                           __global const FilmBlurKernel* taps) {
  const FilmKernelRgb center = FilmLoad(src, src_offset, (uint)y * (uint)width + (uint)x);
  float               r      = taps->weights[0][0] * center.r;
  float               g      = taps->weights[1][0] * center.g;
  float               b      = taps->weights[2][0] * center.b;
  const int           radius = (int)taps->radius;
  for (int tap = 1; tap <= radius; ++tap) {
    const FilmKernelRgb top =
        FilmLoad(src, src_offset, (uint)FilmReflect(y - tap, height) * (uint)width + (uint)x);
    const FilmKernelRgb bottom =
        FilmLoad(src, src_offset, (uint)FilmReflect(y + tap, height) * (uint)width + (uint)x);
    r += taps->weights[0][tap] * (top.r + bottom.r);
    g += taps->weights[1][tap] * (top.g + bottom.g);
    b += taps->weights[2][tap] * (top.b + bottom.b);
  }
  return FilmKernelMakeRgb(r, g, b);
}

/// FP4 and FP5 in one dispatch at full size: the vertical pass of the inhibitor blur, then one step.
/// @p middle holds the horizontal pass.
__kernel void film_print_step_vertical(__global const float* middle, uint middle_offset,
                                       __global const float* log_q, uint log_q_offset,
                                       __global float* amounts, uint amounts_offset, int width,
                                       int height, __global const uchar* data, uint blur_offset,
                                       __global const float* values, uint params_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  __global const FilmKernelParams* params = FilmParams(data, params_offset);
  const uint                       index  = (uint)y * (uint)width + (uint)x;
  const FilmKernelRgb              blurred =
      FilmBlurColumn(middle, middle_offset, x, y, width, height, FilmBlur(data, blur_offset));
  FilmStore(amounts, amounts_offset, index,
            FilmKernelStep(params, values, params->inhibition_image_k,
                           FilmLoad(log_q, log_q_offset, index), blurred));
}

__kernel void film_print_blur_horizontal(__global const float* src, uint src_offset,
                                         __global float* dst, uint dst_offset, int width, int height,
                                         __global const uchar* data, uint blur_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  __global const FilmBlurKernel* taps   = FilmBlur(data, blur_offset);
  const uint                     row    = (uint)y * (uint)width;
  const FilmKernelRgb            center = FilmLoad(src, src_offset, row + (uint)x);
  float                          r      = taps->weights[0][0] * center.r;
  float                          g      = taps->weights[1][0] * center.g;
  float                          b      = taps->weights[2][0] * center.b;
  const int                      radius = (int)taps->radius;
  for (int tap = 1; tap <= radius; ++tap) {
    const FilmKernelRgb left  = FilmLoad(src, src_offset, row + (uint)FilmReflect(x - tap, width));
    const FilmKernelRgb right = FilmLoad(src, src_offset, row + (uint)FilmReflect(x + tap, width));
    r += taps->weights[0][tap] * (left.r + right.r);
    g += taps->weights[1][tap] * (left.g + right.g);
    b += taps->weights[2][tap] * (left.b + right.b);
  }
  FilmStore(dst, dst_offset, row + (uint)x, FilmKernelMakeRgb(r, g, b));
}

/// `dst = weight * blur`, or with @p accumulate, `dst += weight * blur`. The first form must not read
/// the output, because a new transient allocation can hold NaN.
__kernel void film_print_blur_vertical(__global const float* src, uint src_offset,
                                       __global float* dst, uint dst_offset, int width, int height,
                                       __global const uchar* data, uint blur_offset, float weight,
                                       int accumulate) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  const uint          index = (uint)y * (uint)width + (uint)x;
  const FilmKernelRgb blur =
      FilmBlurColumn(src, src_offset, x, y, width, height, FilmBlur(data, blur_offset));
  if (accumulate != 0) {
    const FilmKernelRgb out = FilmLoad(dst, dst_offset, index);
    FilmStore(dst, dst_offset, index,
              FilmKernelMakeRgb(out.r + weight * blur.r, out.g + weight * blur.g,
                                out.b + weight * blur.b));
  } else {
    FilmStore(dst, dst_offset, index,
              FilmKernelMakeRgb(weight * blur.r, weight * blur.g, weight * blur.b));
  }
}

/// The last pass of a canonical result. The canonical results keep opaque alpha.
__kernel void film_print_blur_vertical_to_image(__global const float* src, uint src_offset,
                                                __write_only image2d_t dst, int width, int height,
                                                __global const uchar* data, uint blur_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  const FilmKernelRgb blur =
      FilmBlurColumn(src, src_offset, x, y, width, height, FilmBlur(data, blur_offset));
  write_imagef(dst, (int2)(x, y), (float4)(blur.r, blur.g, blur.b, 1.0f));
}

/// FP2 in one vertical dispatch: `dst = wide_weight * wide + narrow_weight * narrow`. @p middle holds
/// the horizontal pass of the wide Gaussian. The narrow Gaussian reads @p exposure directly.
__kernel void film_print_spread_vertical(__global const float* middle, uint middle_offset,
                                         __global const float* exposure, uint exposure_offset,
                                         __global float* dst, uint dst_offset, int width, int height,
                                         __global const uchar* data, uint wide_offset,
                                         float wide_weight, uint narrow_offset, float narrow_weight) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  const FilmKernelRgb wide =
      FilmBlurColumn(middle, middle_offset, x, y, width, height, FilmBlur(data, wide_offset));
  __global const FilmNarrowKernel* narrow =
      (__global const FilmNarrowKernel*)(data + narrow_offset);
  float     nr      = 0.0f;
  float     ng      = 0.0f;
  float     nb      = 0.0f;
  const int nradius = (int)narrow->radius;
  for (int dy = -nradius; dy <= nradius; ++dy) {
    const uint row = (uint)FilmReflect(y + dy, height) * (uint)width;
    const int  ay  = dy < 0 ? -dy : dy;
    for (int dx = -nradius; dx <= nradius; ++dx) {
      const FilmKernelRgb value =
          FilmLoad(exposure, exposure_offset, row + (uint)FilmReflect(x + dx, width));
      const int ax = dx < 0 ? -dx : dx;
      nr += narrow->y[0][ay] * narrow->x[0][ax] * value.r;
      ng += narrow->y[1][ay] * narrow->x[1][ax] * value.g;
      nb += narrow->y[2][ay] * narrow->x[2][ax] * value.b;
    }
  }
  FilmStore(dst, dst_offset, (uint)y * (uint)width + (uint)x,
            FilmKernelMakeRgb(wide_weight * wide.r + narrow_weight * nr,
                              wide_weight * wide.g + narrow_weight * ng,
                              wide_weight * wide.b + narrow_weight * nb));
}

/// Box mean with cells that cover the image exactly, as the lab reduces an image.
__kernel void film_print_box_reduce(__global const float* src, uint src_offset, int src_width,
                                    int src_height, __global float* dst, uint dst_offset,
                                    int columns, int rows) {
  const int j = (int)get_global_id(0);
  const int i = (int)get_global_id(1);
  if (i >= rows || j >= columns) return;
  const int y0 = FilmBoxCellStart(i, src_height, rows);
  const int y1 = FilmBoxCellStart(i + 1, src_height, rows);
  const int x0 = FilmBoxCellStart(j, src_width, columns);
  const int x1 = FilmBoxCellStart(j + 1, src_width, columns);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const FilmKernelRgb value = FilmLoad(src, src_offset, (uint)y * (uint)src_width + (uint)x);
      r += value.r;
      g += value.g;
      b += value.b;
    }
  }
  const float scale = 1.0f / (float)((y1 - y0) * (x1 - x0));
  FilmStore(dst, dst_offset, (uint)i * (uint)columns + (uint)j,
            FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

// The values of `mode` of film_print_canvas_reduce. The same as CanvasValue of
// opencl_film_print_pass.cpp.
#define FILM_CANVAS_VALUE         0  // The exposure, for Halation
#define FILM_CANVAS_TRANSMITTANCE 1  // 10^-a of the amounts, for the Enlarger flare of a print
#define FILM_CANVAS_DISPLAY_LIGHT 2  // Linear display light of the light table, for the loupe flare

/// Canvas from the render: the mean of @p samples x @p samples render pixels in the footprint of one
/// texel, of the value that @p mode selects. Only FILM_CANVAS_DISPLAY_LIGHT reads @p print_rgba and
/// the fixed values.
__kernel void film_print_canvas_reduce(__global const float* src, uint src_offset, int width,
                                       int height, __global float* dst, uint dst_offset,
                                       int dst_width, int dst_height, FilmMatrix canvas_to_render,
                                       int samples, int mode, __global const float* print_rgba,
                                       __global const uchar* data, uint params_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= dst_width || y >= dst_height) return;
  const float step = 1.0f / (float)samples;
  float       r    = 0.0f;
  float       g    = 0.0f;
  float       b    = 0.0f;
  for (int j = 0; j < samples; ++j) {
    const float cy = (float)y + ((float)j + 0.5f) * step;
    for (int i = 0; i < samples; ++i) {
      const float         cx     = (float)x + ((float)i + 0.5f) * step;
      const float2        render = FilmTransform(canvas_to_render, cx, cy);
      const int           sx     = min(max((int)floor(render.x), 0), width - 1);
      const int           sy     = min(max((int)floor(render.y), 0), height - 1);
      const FilmKernelRgb value  = FilmLoad(src, src_offset, (uint)sy * (uint)width + (uint)sx);
      if (mode == FILM_CANVAS_TRANSMITTANCE) {
        r += FilmKernelTransmittance(value.r);
        g += FilmKernelTransmittance(value.g);
        b += FilmKernelTransmittance(value.b);
      } else if (mode == FILM_CANVAS_DISPLAY_LIGHT) {
        const FilmKernelRgb light =
            FilmKernelPrintDisplay(FilmParams(data, params_offset), print_rgba, value);
        r += light.r;
        g += light.g;
        b += light.b;
      } else {
        r += value.r;
        g += value.g;
        b += value.b;
      }
    }
  }
  const float scale = step * step;
  FilmStore(dst, dst_offset, (uint)y * (uint)dst_width + (uint)x,
            FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

/// Rule 'negative_show_grade': the box mean of the amounts on the half-size image. One work item
/// covers one cell.
__kernel void film_print_amounts_reduce(__global const float* amounts, uint amounts_offset, int width,
                                        int height, __global float* reduced, uint reduced_offset,
                                        int columns, int rows) {
  const int j = (int)get_global_id(0);
  const int i = (int)get_global_id(1);
  if (i >= rows || j >= columns) return;
  const int y0 = FilmHalfCellStart(i, height, rows);
  const int y1 = FilmHalfCellStart(i + 1, height, rows);
  const int x0 = FilmHalfCellStart(j, width, columns);
  const int x1 = FilmHalfCellStart(j + 1, width, columns);
  float     r  = 0.0f;
  float     g  = 0.0f;
  float     b  = 0.0f;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      const FilmKernelRgb value = FilmLoad(amounts, amounts_offset, (uint)y * (uint)width + (uint)x);
      r += value.r;
      g += value.g;
      b += value.b;
    }
  }
  const float scale = 1.0f / (float)((y1 - y0) * (x1 - x0));
  FilmStore(reduced, reduced_offset, (uint)i * (uint)columns + (uint)j,
            FilmKernelMakeRgb(r * scale, g * scale, b * scale));
}

// Work items of one window of the horizontal prescan pass. opencl_film_print_pass.cpp has the same
// value.
#define FILM_PRESCAN_LANES 32

/// The prescan of Negative Scan, horizontal pass (`film_negative_scan::ReduceFrame`): per row of the
/// render and output column, the mean of the scan density T over the window of the column.
/// @p windows holds the windows of `film_negative_scan::PrescanPlanOf`. One work group of
/// FILM_PRESCAN_LANES covers one window (group j of dimension 0, row y of dimension 1), so the reads
/// of a group are adjacent; the group adds its sums in local memory.
__kernel void film_print_scan_prescan_rows(__global const float* amounts, uint amounts_offset,
                                           int width, int height,
                                           __global const float* scanner_table,
                                           __global const int* windows, int columns,
                                           __global float* middle, uint middle_offset,
                                           __global const uchar* data, uint params_offset) {
  __local float sums[3][FILM_PRESCAN_LANES];
  const int     lane = (int)get_local_id(0);
  const int     j    = (int)get_group_id(0);
  const int     y    = (int)get_global_id(1);
  float         r    = 0.0f;
  float         g    = 0.0f;
  float         b    = 0.0f;
  int           count = 1;
  if (j < columns && y < height) {
    __global const FilmKernelParams* params = FilmParams(data, params_offset);
    const int                        first  = windows[j];
    count                                   = windows[columns + j];
    for (int x = first + lane; x < first + count; x += FILM_PRESCAN_LANES) {
      const FilmKernelRgb t = FilmKernelReadRecordTable(
          params, scanner_table,
          FilmLoad(amounts, amounts_offset, (uint)y * (uint)width + (uint)x));
      r += t.r;
      g += t.g;
      b += t.b;
    }
  }
  sums[0][lane] = r;
  sums[1][lane] = g;
  sums[2][lane] = b;
  barrier(CLK_LOCAL_MEM_FENCE);
  for (int offset = FILM_PRESCAN_LANES / 2; offset > 0; offset /= 2) {
    if (lane < offset) {
      sums[0][lane] += sums[0][lane + offset];
      sums[1][lane] += sums[1][lane + offset];
      sums[2][lane] += sums[2][lane + offset];
    }
    barrier(CLK_LOCAL_MEM_FENCE);
  }
  if (lane == 0 && j < columns && y < height) {
    const float scale = 1.0f / (float)count;
    FilmStore(middle, middle_offset, (uint)y * (uint)columns + (uint)j,
              FilmKernelMakeRgb(sums[0][0] * scale, sums[1][0] * scale, sums[2][0] * scale));
  }
}

/// The vertical pass of the prescan: per output pixel, the mean of @p middle over the window of the
/// row, to the canonical result.
__kernel void film_print_scan_prescan_columns(__global const float* middle, uint middle_offset,
                                              int columns, int height, __global const int* windows,
                                              int rows, __write_only image2d_t prescan) {
  const int j = (int)get_global_id(0);
  const int i = (int)get_global_id(1);
  if (j >= columns || i >= rows) return;
  const int first = windows[2 * columns + i];
  const int count = windows[2 * columns + rows + i];
  float     r     = 0.0f;
  float     g     = 0.0f;
  float     b     = 0.0f;
  for (int y = first; y < first + count; ++y) {
    const FilmKernelRgb value = FilmLoad(middle, middle_offset, (uint)y * (uint)columns + (uint)j);
    r += value.r;
    g += value.g;
    b += value.b;
  }
  const float scale = 1.0f / (float)count;
  write_imagef(prescan, (int2)(j, i), (float4)(r * scale, g * scale, b * scale, 1.0f));
}

/// FP6 of a print: the Enlarger flare in density, the negative grade, the print table, the display
/// compression, and the cinema look. Linear Rec. 709 in 0 to 1.
static inline FilmKernelRgb FilmPrintFinishedLinear(
    __global const FilmKernelParams* params, FilmKernelRgb record, int x, int y, uint index,
    __global const float* print_rgba, __read_only image2d_t flare, int has_flare, int flare_width,
    int flare_height, FilmMatrix render_to_flare, float flare_strength, __global const float* soft,
    uint soft_offset, int has_soft, int soft_columns, int soft_rows, int soft_half_size) {
  FilmKernelRgb soft_record = record;
  if (params->negative_grade != 0U && has_soft != 0) {
    soft_record = soft_half_size != 0
                      ? FilmReadBilinear(soft, soft_offset, soft_columns, soft_rows,
                                         ((float)x + 0.5f) * 0.5f, ((float)y + 0.5f) * 0.5f)
                      : FilmLoad(soft, soft_offset, index);
  }
  if (has_flare != 0) {
    const float2 position = FilmTransform(render_to_flare, (float)x + 0.5f, (float)y + 0.5f);
    record                = FilmKernelFlareAmounts(
        record, FilmReadBilinearImage(flare, flare_width, flare_height, position.x, position.y),
        flare_strength);
  }
  if (params->negative_grade != 0U) {
    if (has_soft == 0) soft_record = record;
    record = FilmKernelNegativeGrade(params, record, soft_record);
  }
  const FilmKernelRgb finished =
      FilmKernelFinishDisplay(params, FilmKernelPrintDisplay(params, print_rgba, record));
  return params->cinema_look != 0U ? FilmKernelCinemaLook(finished) : finished;
}

/// FP6, fused, at pixel (@p x, @p y) with the record @p record of the negative. @p has_flare is 0
/// when the Enlarger flare is off. Alpha comes from the scene. @p has_soft is 0 with no image
/// plane; then the graded record reads itself. @p soft is half size when @p soft_half_size is not
/// 0. On a light table, @p flare is the blurred display light of the loupe flare, and @p soft is
/// not read.
static inline void FilmPrintDisplayPixel(
    __global const FilmKernelParams* params, FilmKernelRgb record, int x, int y, int width,
    __read_only image2d_t scene_image, __global const float4* scene_buffer, int scene_is_buffer,
    __write_only image2d_t dst_image, __global float4* dst_buffer, int dst_is_buffer,
    __global const float* print_rgba, __read_only image2d_t flare, int has_flare, int flare_width,
    int flare_height, FilmMatrix render_to_flare, float flare_strength, __global const float* soft,
    uint soft_offset, int has_soft, int soft_columns, int soft_rows, int soft_half_size) {
  const uint    index = (uint)y * (uint)width + (uint)x;
  FilmKernelRgb linear;
  if (params->light_table != 0U) {
    // Lab rules 'light_table_view', 'loupe_flare', and 'dark_surround_on_display' in display light.
    FilmKernelRgb light = FilmKernelPrintDisplay(params, print_rgba, record);
    if (has_flare != 0) {
      const float2 position = FilmTransform(render_to_flare, (float)x + 0.5f, (float)y + 0.5f);
      light                 = FilmKernelLoupeFlare(
          light, FilmReadBilinearImage(flare, flare_width, flare_height, position.x, position.y),
          flare_strength);
    }
    linear = FilmKernelFinishDisplay(params, FilmKernelDisplaySurround(params, light));
  } else {
    linear = FilmPrintFinishedLinear(params, record, x, y, index, print_rgba, flare, has_flare,
                                     flare_width, flare_height, render_to_flare, flare_strength,
                                     soft, soft_offset, has_soft, soft_columns, soft_rows,
                                     soft_half_size);
  }
  const float alpha  = scene_is_buffer != 0
                           ? scene_buffer[index].w
                           : read_imagef(scene_image, kFilmNearestClamp, (int2)(x, y)).w;
  const int   tf     = (int)params->display_encoding;
  const float4 output = (float4)(CeDisplayEncodeChannel(tf, linear.r, 1.0f),
                                 CeDisplayEncodeChannel(tf, linear.g, 1.0f),
                                 CeDisplayEncodeChannel(tf, linear.b, 1.0f), alpha);
  if (dst_is_buffer != 0) {
    dst_buffer[index] = output;
  } else {
    write_imagef(dst_image, (int2)(x, y), output);
  }
}

__kernel void film_print_display(
    __global const float* amounts, uint amounts_offset, __read_only image2d_t scene_image,
    __global const float4* scene_buffer, int scene_is_buffer, __write_only image2d_t dst_image,
    __global float4* dst_buffer, int dst_is_buffer, int width, int height,
    __global const float* print_rgba, __read_only image2d_t flare, int has_flare, int flare_width,
    int flare_height, FilmMatrix render_to_flare, float flare_strength, __global const float* soft,
    uint soft_offset, int has_soft, int soft_columns, int soft_rows, int soft_half_size,
    __global const uchar* data, uint params_offset) {
  const int x = (int)get_global_id(0);
  const int y = (int)get_global_id(1);
  if (x >= width || y >= height) return;
  FilmPrintDisplayPixel(FilmParams(data, params_offset),
                        FilmLoad(amounts, amounts_offset, (uint)y * (uint)width + (uint)x), x, y,
                        width, scene_image, scene_buffer, scene_is_buffer, dst_image, dst_buffer,
                        dst_is_buffer, print_rgba, flare, has_flare, flare_width, flare_height,
                        render_to_flare, flare_strength, soft, soft_offset, has_soft, soft_columns,
                        soft_rows, soft_half_size);
}

/// @p record with the grain @p grain of rule 'grain_layer_mix' (film_grain_negative.h).
static inline FilmKernelRgb FilmGrainRecord(__local const FilmGrainNegativeParams* params,
                                            FilmKernelRgb record, FilmGrainNoise3 grain) {
  FilmGrainNegativeRgb value;
  value.r                           = record.r;
  value.g                           = record.g;
  value.b                           = record.b;
  const FilmGrainNegativeRgb result = FilmGrainNegativeRecord(params, value, grain);
  return FilmKernelMakeRgb(result.r, result.g, result.b);
}

/// Phase G6: FP6 of the print and the light table with the grain of the negative, in the steps of
/// film_grain_tile.h. Each 16 x 16 work-group covers a tile of 16 x FILM_GRAIN_NEGATIVE_TILE_ROWS
/// pixels, and each lane applies the grain to two pixels. The grain goes into the record of each
/// pixel before the flare amounts, the negative grade, and the table read, so the amounts with
/// grain are never written. @p grain_data holds the FilmGrainFieldParams at @p field_offset and the
/// FilmGrainNegativeParams at @p negative_offset (opencl_film_print_pass.cpp); the work-group
/// copies them into local memory. The other arguments are those of film_print_display.
__kernel __attribute__((reqd_work_group_size(FILM_GRAIN_TILE, FILM_GRAIN_TILE, 1))) void
film_print_display_grain(
    __global const float* amounts, uint amounts_offset, __read_only image2d_t scene_image,
    __global const float4* scene_buffer, int scene_is_buffer, __write_only image2d_t dst_image,
    __global float4* dst_buffer, int dst_is_buffer, int width, int height,
    __global const float* print_rgba, __read_only image2d_t flare, int has_flare, int flare_width,
    int flare_height, FilmMatrix render_to_flare, float flare_strength, __global const float* soft,
    uint soft_offset, int has_soft, int soft_columns, int soft_rows, int soft_half_size,
    __global const uchar* data, uint params_offset, __global const uchar* grain_data,
    uint field_offset, uint negative_offset, __local float* shared) {
  const int                       local_x = (int)get_local_id(0);
  const int                       local_y = (int)get_local_id(1);
  const int                       lane    = local_y * FILM_GRAIN_TILE + local_x;
  __local FilmGrainFieldParams    field;
  __local FilmGrainNegativeParams negative;
  FilmGrainTileCopyWords(grain_data + field_offset, &field, sizeof(FilmGrainFieldParams), lane);
  FilmGrainTileCopyWords(grain_data + negative_offset, &negative, sizeof(FilmGrainNegativeParams),
                         lane);
  // Every lane reads these before the first barrier, so they come from the upload.
  __global const FilmGrainFieldParams* upload =
      (__global const FilmGrainFieldParams*)(grain_data + field_offset);
  const int halo = upload->halo;
  FilmGrainTileAcross(&field, upload->noise, halo, FILM_GRAIN_NEGATIVE_TILE_ROWS,
                      (int)get_group_id(0), (int)get_group_id(1), lane, shared);

  const int x = (int)get_group_id(0) * FILM_GRAIN_TILE + local_x;
  if (x >= width) return;
  __global const FilmKernelParams* params = FilmParams(data, params_offset);
  for (int half_index = 0; half_index < FILM_GRAIN_NEGATIVE_TILE_ROWS / FILM_GRAIN_TILE;
       ++half_index) {
    const int tile_row = local_y + half_index * FILM_GRAIN_TILE;
    const int y        = (int)get_group_id(1) * FILM_GRAIN_NEGATIVE_TILE_ROWS + tile_row;
    if (y >= height) return;
    const FilmGrainNoise3 grain =
        FilmGrainTileGrain(&field, halo, FILM_GRAIN_NEGATIVE_TILE_ROWS, tile_row, local_x, shared);
    const FilmKernelRgb record = FilmGrainRecord(
        &negative, FilmLoad(amounts, amounts_offset, (uint)y * (uint)width + (uint)x), grain);
    FilmPrintDisplayPixel(params, record, x, y, width, scene_image, scene_buffer, scene_is_buffer,
                          dst_image, dst_buffer, dst_is_buffer, print_rgba, flare, has_flare,
                          flare_width, flare_height, render_to_flare, flare_strength, soft,
                          soft_offset, has_soft, soft_columns, soft_rows, soft_half_size);
  }
}
