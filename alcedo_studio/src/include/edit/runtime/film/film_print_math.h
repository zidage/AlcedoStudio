// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#ifndef ALCEDO_FILM_PRINT_MATH_H
#define ALCEDO_FILM_PRINT_MATH_H

// Point stages of the film print, shared by the host, CUDA, OpenCL C, and Metal. Plan: Film Print
// DRT Plan, section 3, steps FP1, FP4, FP5, and FP6. The stages are those of `fast_kernels.py` of
// alcedo-film-lab. The product arithmetic is float32. A host file can define FILM_KERNEL_DOUBLE
// before the first include, inside its own namespace, for a double-precision form of the same math.
#if defined(__OPENCL_VERSION__) || defined(__OPENCL_C_VERSION__)
#define FILM_KERNEL_INLINE static inline
#define FILM_KERNEL_LOG10  log10
#define FILM_KERNEL_POW    pow
#define FILM_KERNEL_EXP    exp
#define FILM_KERNEL_VALUES __global const
#define FILM_KERNEL_PARAMS __global const
#define FILM_KERNEL_UINT   uint
#elif defined(__METAL_VERSION__)
#define FILM_KERNEL_INLINE static inline
#define FILM_KERNEL_LOG10  log10
#define FILM_KERNEL_POW    pow
#define FILM_KERNEL_EXP    exp
#define FILM_KERNEL_VALUES const device
#define FILM_KERNEL_PARAMS constant
#define FILM_KERNEL_UINT   uint
#else
#include <cmath>
#include <cstdint>
#ifdef __CUDACC__
#define FILM_KERNEL_INLINE static __host__ __device__ inline
#else
#define FILM_KERNEL_INLINE static inline
#endif
#define FILM_KERNEL_VALUES const
#define FILM_KERNEL_PARAMS const
#define FILM_KERNEL_UINT   std::uint32_t
#ifdef FILM_KERNEL_DOUBLE
#define FILM_KERNEL_LOG10 log10
#define FILM_KERNEL_POW   pow
#define FILM_KERNEL_EXP   exp
#else
#define FILM_KERNEL_LOG10 log10f
#define FILM_KERNEL_POW   powf
#define FILM_KERNEL_EXP   expf
#endif
#endif

#ifdef FILM_KERNEL_DOUBLE
typedef double FilmKernelReal;
#define FILM_KERNEL_REAL(value) value
#else
typedef float FilmKernelReal;
#define FILM_KERNEL_REAL(value) value##f
#endif

typedef struct {
  FilmKernelReal r, g, b;
} FilmKernelRgb;

/**
 * Fixed values of one selection. The struct has only 4-byte members in the float32 form, so it has
 * one layout on each backend. `FillParams` in film_print_kernel_tables.hpp writes it.
 *
 * Offsets count values from the start of the table buffer of the selection:
 * - uv: (uv_nodes, uv_nodes, 3), u varying slowest. A node that no read can reach holds 0.
 * - curves: (3, curve_stored), the first dye amounts of each layer on log H.
 * - inhibition: (3, inhibition_stored), F_i of rule 'development_inhibition'.
 * - normals: (locus_facets, 2). room: (locus_facets). Rule 'spectral_locus_projection'.
 */
typedef struct {
  FilmKernelReal   ap1_to_xyz[9];
  FilmKernelReal   locus_white[2];
  FilmKernelReal   locus_keep;           // 1 - the inset of the hull
  FilmKernelReal   locus_inner_squared;  // Squared radius of a circle at the white inside the hull
  FilmKernelReal   uv_scale[2];          // (uv_nodes - 1) / uv_max
  FilmKernelReal   layer_gain[3];
  FilmKernelReal   curve_offset[3];  // Criterion log H - the log H of the first curve value
  FilmKernelReal   curve_step;
  FilmKernelReal   inhibition_k[9];        // Row major. The total, which sets F_i.
  FilmKernelReal   inhibition_image_k[9];  // Rule 'inhibition_image_part': the part with a blur
  FilmKernelReal   inhibition_point_k[9];  // The total minus the image part
  FilmKernelReal   inhibition_offset[3];   // Criterion log H - the log H of the first F_i value
  FilmKernelReal   inhibition_step;
  FilmKernelReal   print_start[3];
  FilmKernelReal   print_step[3];
  FilmKernelReal   xyz_to_display[9];  // Row major
  FilmKernelReal   gamut_threshold[3];
  FilmKernelReal   gamut_scale[3];
  FilmKernelReal   gamut_power;
  FilmKernelReal   negative_grade_grey[3];  // Rule 'negative_show_grade': the record of an 18 % grey
  FILM_KERNEL_UINT locus_facets;
  FILM_KERNEL_UINT uv_nodes;
  FILM_KERNEL_UINT curve_stored;
  FILM_KERNEL_UINT curve_lengths[3];
  FILM_KERNEL_UINT inhibition_stored;  // 0: this negative has no inhibition
  FILM_KERNEL_UINT inhibition_lengths[3];
  FILM_KERNEL_UINT inhibition_has_image;  // 0: no value of the image part is above 0
  FILM_KERNEL_UINT inhibition_has_point;  // 0: no value of the point part is above 0
  FILM_KERNEL_UINT print_edge;
  FILM_KERNEL_UINT curves_offset;
  FILM_KERNEL_UINT inhibition_values_offset;
  FILM_KERNEL_UINT normals_offset;
  FILM_KERNEL_UINT room_offset;
  FILM_KERNEL_UINT display_encoding;  // CE_TF_* of the display curve. Read by the backend kernel.
  FILM_KERNEL_UINT cinema_look;       // 1: rule 'print_cinema_look'. Read by the backend kernel.
  FILM_KERNEL_UINT negative_grade;    // 1: rule 'negative_show_grade'. Read by the backend kernel.
} FilmKernelParams;

FILM_KERNEL_INLINE FilmKernelRgb FilmKernelMakeRgb(FilmKernelReal r, FilmKernelReal g,
                                                   FilmKernelReal b) {
  FilmKernelRgb result;
  result.r = r;
  result.g = g;
  result.b = b;
  return result;
}

FILM_KERNEL_INLINE FilmKernelReal FilmKernelMax(FilmKernelReal a, FilmKernelReal b) {
  return a > b ? a : b;
}

FILM_KERNEL_INLINE FilmKernelReal FilmKernelMin(FilmKernelReal a, FilmKernelReal b) {
  return a < b ? a : b;
}

/**
 * Position of a read of a uniform axis, as a cell and a share. The last cell is `nodes - 2`, so the
 * read of the last node has a share of 1. The lab limits the position to `nodes - 1.000001`, which
 * is `nodes - 1` in float32 at this table size.
 */
FILM_KERNEL_INLINE FILM_KERNEL_UINT FilmKernelCell(FilmKernelReal   position,
                                                   FILM_KERNEL_UINT nodes) {
  const FILM_KERNEL_UINT cell = (FILM_KERNEL_UINT)position;
  return cell < nodes - 2U ? cell : nodes - 2U;
}

/** Value of a stored curve at the position @p t. Outside the curve, its end value. */
FILM_KERNEL_INLINE FilmKernelReal FilmKernelReadCurve(FILM_KERNEL_VALUES FilmKernelReal* row,
                                                      FILM_KERNEL_UINT length, FilmKernelReal t) {
  const FILM_KERNEL_UINT last = length - 1U;
  if (!(t > FILM_KERNEL_REAL(0.0))) return row[0];
  if (t >= (FilmKernelReal)last) return row[last];
  const FILM_KERNEL_UINT index = (FILM_KERNEL_UINT)t;
  return row[index] + (t - (FilmKernelReal)index) * (row[index + 1U] - row[index]);
}

/**
 * FP1: linear AP1 to the exposure Q of the three layers, with rule 'spectral_locus_projection' and
 * a bilinear read of the u'v' table.
 *
 * The lab divides by Y first. This form multiplies the hull test by Y, which has the same result
 * for Y above 0, because a very small Y must not move X / Y out of the float32 range.
 */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelLayerExposure(
    FILM_KERNEL_PARAMS FilmKernelParams* params, FILM_KERNEL_VALUES FilmKernelReal* values,
    FilmKernelReal r, FilmKernelReal g, FilmKernelReal b) {
  FILM_KERNEL_PARAMS FilmKernelReal* m = params->ap1_to_xyz;
  const FilmKernelReal               y = m[3] * r + m[4] * g + m[5] * b;
  if (y <= FILM_KERNEL_REAL(0.0)) {
    return FilmKernelMakeRgb(FILM_KERNEL_REAL(0.0), FILM_KERNEL_REAL(0.0), FILM_KERNEL_REAL(0.0));
  }
  const FilmKernelReal               x       = m[0] * r + m[1] * g + m[2] * b;
  const FilmKernelReal               z       = m[6] * r + m[7] * g + m[8] * b;
  const FilmKernelReal               wx      = params->locus_white[0] * y;
  const FilmKernelReal               wz      = params->locus_white[1] * y;
  const FilmKernelReal               dx      = x - wx;
  const FilmKernelReal               dz      = z - wz;
  FILM_KERNEL_VALUES FilmKernelReal* normals = values + params->normals_offset;
  FILM_KERNEL_VALUES FilmKernelReal* room    = values + params->room_offset;
  FilmKernelReal                     t       = FILM_KERNEL_REAL(1.0);
  // Inside the circle that the hull holds, no facet moves the colour, so most pixels skip the loop.
  if (dx * dx + dz * dz > params->locus_inner_squared * y * y) {
    for (FILM_KERNEL_UINT facet = 0U; facet < params->locus_facets; ++facet) {
      const FilmKernelReal rate = dx * normals[2U * facet] + dz * normals[2U * facet + 1U];
      if (rate > FILM_KERNEL_REAL(0.0)) {
        t = FilmKernelMin(t, room[facet] * params->locus_keep * y / rate);
      }
    }
  }
  const FilmKernelReal   px    = wx + t * dx;
  const FilmKernelReal   pz    = wz + t * dz;
  const FilmKernelReal   d     = px + FILM_KERNEL_REAL(15.0) * y + FILM_KERNEL_REAL(3.0) * pz;
  const FILM_KERNEL_UINT nodes = params->uv_nodes;
  const FilmKernelReal   top   = (FilmKernelReal)(nodes - 1U);
  const FilmKernelReal   cu    = FilmKernelMin(
      FilmKernelMax(FILM_KERNEL_REAL(4.0) * px / d * params->uv_scale[0], FILM_KERNEL_REAL(0.0)),
      top);
  const FilmKernelReal cv = FilmKernelMin(
      FilmKernelMax(FILM_KERNEL_REAL(9.0) * y / d * params->uv_scale[1], FILM_KERNEL_REAL(0.0)),
      top);
  const FILM_KERNEL_UINT             iu   = FilmKernelCell(cu, nodes);
  const FILM_KERNEL_UINT             iv   = FilmKernelCell(cv, nodes);
  const FilmKernelReal               fu   = cu - (FilmKernelReal)iu;
  const FilmKernelReal               fv   = cv - (FilmKernelReal)iv;
  FILM_KERNEL_VALUES FilmKernelReal* low  = values + (iu * nodes + iv) * 3U;
  FILM_KERNEL_VALUES FilmKernelReal* high = values + ((iu + 1U) * nodes + iv) * 3U;
  FilmKernelReal                     q[3];
  for (FILM_KERNEL_UINT k = 0U; k < 3U; ++k) {
    const FilmKernelReal value =
        (FILM_KERNEL_REAL(1.0) - fu) * ((FILM_KERNEL_REAL(1.0) - fv) * low[k] + fv * low[3U + k]) +
        fu * ((FILM_KERNEL_REAL(1.0) - fv) * high[k] + fv * high[3U + k]);
    q[k] = params->layer_gain[k] * y * value;
  }
  return FilmKernelMakeRgb(q[0], q[1], q[2]);
}

/** log10 of an exposure, with the limit of the lab for an exposure of 0. */
FILM_KERNEL_INLINE FilmKernelReal FilmKernelLogExposure(FilmKernelReal q) {
  return FILM_KERNEL_LOG10(FilmKernelMax(q, FILM_KERNEL_REAL(1e-30)));
}

/** log10 of the exposure Q of each layer. */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelLogExposures(FilmKernelRgb q) {
  return FilmKernelMakeRgb(FilmKernelLogExposure(q.r), FilmKernelLogExposure(q.g),
                           FilmKernelLogExposure(q.b));
}

/** FP4: log exposure to the first dye amounts, on the curves of rule 'dye_amount_curves'. */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelCurveAmounts(FILM_KERNEL_PARAMS FilmKernelParams* params,
                                                        FILM_KERNEL_VALUES FilmKernelReal*   values,
                                                        FilmKernelRgb log_q) {
  FILM_KERNEL_VALUES FilmKernelReal* curves = values + params->curves_offset;
  const FILM_KERNEL_UINT             stored = params->curve_stored;
  const FilmKernelReal               step   = params->curve_step;
  return FilmKernelMakeRgb(FilmKernelReadCurve(curves, params->curve_lengths[0],
                                               (params->curve_offset[0] + log_q.r) / step),
                           FilmKernelReadCurve(curves + stored, params->curve_lengths[1],
                                               (params->curve_offset[1] + log_q.g) / step),
                           FilmKernelReadCurve(curves + 2U * stored, params->curve_lengths[2],
                                               (params->curve_offset[2] + log_q.b) / step));
}

/**
 * FP5: one step of rule 'development_inhibition': a_i = F_i(log Q_i - sum_j K_ij I_j).
 * Call this only when `params->inhibition_stored` is not 0.
 */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelStep(FILM_KERNEL_PARAMS FilmKernelParams* params,
                                                FILM_KERNEL_VALUES FilmKernelReal*   values,
                                                FILM_KERNEL_PARAMS FilmKernelReal*   k,
                                                FilmKernelRgb log_q, FilmKernelRgb inhibitor) {
  FILM_KERNEL_VALUES FilmKernelReal* free_curves = values + params->inhibition_values_offset;
  const FILM_KERNEL_UINT             stored      = params->inhibition_stored;
  const FilmKernelReal               step        = params->inhibition_step;
  const FilmKernelReal t0 = (params->inhibition_offset[0] + log_q.r - k[0] * inhibitor.r -
                             k[1] * inhibitor.g - k[2] * inhibitor.b) /
                            step;
  const FilmKernelReal t1 = (params->inhibition_offset[1] + log_q.g - k[3] * inhibitor.r -
                             k[4] * inhibitor.g - k[5] * inhibitor.b) /
                            step;
  const FilmKernelReal t2 = (params->inhibition_offset[2] + log_q.b - k[6] * inhibitor.r -
                             k[7] * inhibitor.g - k[8] * inhibitor.b) /
                            step;
  return FilmKernelMakeRgb(
      FilmKernelReadCurve(free_curves, params->inhibition_lengths[0], t0),
      FilmKernelReadCurve(free_curves + stored, params->inhibition_lengths[1], t1),
      FilmKernelReadCurve(free_curves + 2U * stored, params->inhibition_lengths[2], t2));
}

/**
 * FP4 and FP5 with no blur: the amounts of an exposure Q, with the total K in two steps. This is
 * the form with no image plane, and the form of a negative with no image part. With no inhibition,
 * it gives the first amounts.
 */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelPointAmounts(FILM_KERNEL_PARAMS FilmKernelParams* params,
                                                        FILM_KERNEL_VALUES FilmKernelReal*   values,
                                                        FilmKernelRgb                        q) {
  const FilmKernelRgb log_q = FilmKernelLogExposures(q);
  FilmKernelRgb       a     = FilmKernelCurveAmounts(params, values, log_q);
  if (params->inhibition_stored != 0U) {
    a = FilmKernelStep(params, values, params->inhibition_k, log_q, a);
    a = FilmKernelStep(params, values, params->inhibition_k, log_q, a);
  }
  return a;
}

/** The first dye amounts, and the log exposure that the steps with a blurred inhibitor read. */
typedef struct {
  FilmKernelRgb amounts;
  FilmKernelRgb log_q;
} FilmKernelFirstStage;

/**
 * FP4 for a negative with an image part, rule 'inhibition_image_part'. The point part acts here,
 * with the amounts a1 = F(log Q - K a) of one point step: log Q - K_point a1. So the later steps
 * need no image of the amounts for this part, and they read a stored log exposure with no log10.
 */
FILM_KERNEL_INLINE FilmKernelFirstStage
FilmKernelFirstAmountsWithPointPart(FILM_KERNEL_PARAMS FilmKernelParams* params,
                                    FILM_KERNEL_VALUES FilmKernelReal* values, FilmKernelRgb q) {
  FilmKernelFirstStage first;
  first.log_q   = FilmKernelLogExposures(q);
  first.amounts = FilmKernelCurveAmounts(params, values, first.log_q);
  if (params->inhibition_has_point != 0U) {
    const FilmKernelRgb a1 =
        FilmKernelStep(params, values, params->inhibition_k, first.log_q, first.amounts);
    FILM_KERNEL_PARAMS FilmKernelReal* k = params->inhibition_point_k;
    first.log_q.r -= k[0] * a1.r + k[1] * a1.g + k[2] * a1.b;
    first.log_q.g -= k[3] * a1.r + k[4] * a1.g + k[5] * a1.b;
    first.log_q.b -= k[6] * a1.r + k[7] * a1.g + k[8] * a1.b;
  }
  return first;
}

/**
 * FP6, first part: dye amounts to linear display RGB: a trilinear read of the print table, the
 * cube, and the display matrix. @p print_rgba has 4 values per node, with red varying fastest.
 */
FILM_KERNEL_INLINE FilmKernelRgb
FilmKernelPrintDisplay(FILM_KERNEL_PARAMS FilmKernelParams* params,
                       FILM_KERNEL_VALUES FilmKernelReal* print_rgba, FilmKernelRgb amounts) {
  const FILM_KERNEL_UINT edge = params->print_edge;
  const FilmKernelReal   top  = (FilmKernelReal)(edge - 1U);
  const FilmKernelReal   cr =
      FilmKernelMin(FilmKernelMax((amounts.r - params->print_start[0]) / params->print_step[0],
                                  FILM_KERNEL_REAL(0.0)),
                    top);
  const FilmKernelReal cg =
      FilmKernelMin(FilmKernelMax((amounts.g - params->print_start[1]) / params->print_step[1],
                                  FILM_KERNEL_REAL(0.0)),
                    top);
  const FilmKernelReal cb =
      FilmKernelMin(FilmKernelMax((amounts.b - params->print_start[2]) / params->print_step[2],
                                  FILM_KERNEL_REAL(0.0)),
                    top);
  const FILM_KERNEL_UINT             ir   = FilmKernelCell(cr, edge);
  const FILM_KERNEL_UINT             ig   = FilmKernelCell(cg, edge);
  const FILM_KERNEL_UINT             ib   = FilmKernelCell(cb, edge);
  const FilmKernelReal               fr   = cr - (FilmKernelReal)ir;
  const FilmKernelReal               fg   = cg - (FilmKernelReal)ig;
  const FilmKernelReal               fb   = cb - (FilmKernelReal)ib;
  const FILM_KERNEL_UINT             row  = edge * 4U;
  const FILM_KERNEL_UINT             lay  = edge * row;
  FILM_KERNEL_VALUES FilmKernelReal* c000 = print_rgba + (ib * edge + ig) * row + ir * 4U;
  FILM_KERNEL_VALUES FilmKernelReal* c010 = c000 + row;
  FILM_KERNEL_VALUES FilmKernelReal* c001 = c000 + lay;
  FILM_KERNEL_VALUES FilmKernelReal* c011 = c001 + row;
  FilmKernelReal                     xyz[3];
  for (FILM_KERNEL_UINT k = 0U; k < 3U; ++k) {
    const FilmKernelReal v00 = c000[k] + fr * (c000[4U + k] - c000[k]);
    const FilmKernelReal v10 = c010[k] + fr * (c010[4U + k] - c010[k]);
    const FilmKernelReal v01 = c001[k] + fr * (c001[4U + k] - c001[k]);
    const FilmKernelReal v11 = c011[k] + fr * (c011[4U + k] - c011[k]);
    const FilmKernelReal v0  = v00 + fg * (v10 - v00);
    const FilmKernelReal v1  = v01 + fg * (v11 - v01);
    const FilmKernelReal v   = v0 + fb * (v1 - v0);
    xyz[k]                   = v * v * v;
  }
  FILM_KERNEL_PARAMS FilmKernelReal* m = params->xyz_to_display;
  return FilmKernelMakeRgb(m[0] * xyz[0] + m[1] * xyz[1] + m[2] * xyz[2],
                           m[3] * xyz[0] + m[4] * xyz[1] + m[5] * xyz[2],
                           m[6] * xyz[0] + m[7] * xyz[1] + m[8] * xyz[2]);
}

/** Transmittance of a dye amount, for the Enlarger flare: 10^-a. */
FILM_KERNEL_INLINE FilmKernelReal FilmKernelTransmittance(FilmKernelReal amount) {
  return FILM_KERNEL_EXP(FILM_KERNEL_REAL(-2.302585092994046) * amount);
}

/**
 * FP6, Enlarger flare: the amounts that give the transmittance
 * (1 - flare) T + flare * blurred, where @p blurred is a blurred transmittance.
 */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelFlareAmounts(FilmKernelRgb  amounts,
                                                        FilmKernelRgb  blurred,
                                                        FilmKernelReal flare) {
  const FilmKernelReal keep = FILM_KERNEL_REAL(1.0) - flare;
  return FilmKernelMakeRgb(
      -FILM_KERNEL_LOG10(keep * FilmKernelTransmittance(amounts.r) + flare * blurred.r),
      -FILM_KERNEL_LOG10(keep * FilmKernelTransmittance(amounts.g) + flare * blurred.g),
      -FILM_KERNEL_LOG10(keep * FilmKernelTransmittance(amounts.b) + flare * blurred.b));
}

/** Distance of a channel from the largest channel, after rule 'rgc_style_display_compression'. */
FILM_KERNEL_INLINE FilmKernelReal FilmKernelCompressedDistance(FilmKernelReal distance,
                                                               FilmKernelReal threshold,
                                                               FilmKernelReal scale,
                                                               FilmKernelReal power) {
  if (!(distance > threshold)) return distance;
  const FilmKernelReal normal = (distance - threshold) / scale;
  return threshold + scale * normal /
                         FILM_KERNEL_POW(FILM_KERNEL_REAL(1.0) + FILM_KERNEL_POW(normal, power),
                                         FILM_KERNEL_REAL(1.0) / power);
}

FILM_KERNEL_INLINE FilmKernelReal FilmKernelClipUnit(FilmKernelReal value) {
  return FilmKernelMin(FilmKernelMax(value, FILM_KERNEL_REAL(0.0)), FILM_KERNEL_REAL(1.0));
}

/** FP6, second part: rule 'rgc_style_display_compression' on linear Rec. 709, then the clip. */
FILM_KERNEL_INLINE FilmKernelRgb
FilmKernelFinishDisplay(FILM_KERNEL_PARAMS FilmKernelParams* params, FilmKernelRgb rgb) {
  const FilmKernelReal a = FilmKernelMax(rgb.r, FilmKernelMax(rgb.g, rgb.b));
  if (!(a > FILM_KERNEL_REAL(0.0))) {
    return FilmKernelMakeRgb(FILM_KERNEL_REAL(0.0), FILM_KERNEL_REAL(0.0), FILM_KERNEL_REAL(0.0));
  }
  const FilmKernelReal power = params->gamut_power;
  const FilmKernelReal dr    = FilmKernelCompressedDistance(
      (a - rgb.r) / a, params->gamut_threshold[0], params->gamut_scale[0], power);
  const FilmKernelReal dg = FilmKernelCompressedDistance(
      (a - rgb.g) / a, params->gamut_threshold[1], params->gamut_scale[1], power);
  const FilmKernelReal db = FilmKernelCompressedDistance(
      (a - rgb.b) / a, params->gamut_threshold[2], params->gamut_scale[2], power);
  return FilmKernelMakeRgb(FilmKernelClipUnit(a - dr * a), FilmKernelClipUnit(a - dg * a),
                           FilmKernelClipUnit(a - db * a));
}

/** Smoothstep from 0 at @p low to 1 at @p high. */
FILM_KERNEL_INLINE FilmKernelReal FilmKernelSmoothstep(FilmKernelReal low, FilmKernelReal high,
                                                       FilmKernelReal x) {
  const FilmKernelReal t = FilmKernelClipUnit((x - low) / (high - low));
  return t * t * (FILM_KERNEL_REAL(3.0) - FILM_KERNEL_REAL(2.0) * t);
}

/**
 * Rule 'negative_show_grade' (lab decisions U82, U83): a fixed creative grade of the negative
 * record, before the print table, as a DI grades a scan before the print transform. All constants
 * are creative choices. The steps and the order are those of `negative_grade.py` of
 * alcedo-film-lab. Only Kodak 5219 on print material `cine_look` has it.
 *
 * @param params The fixed values of the selection, for the record of an 18 % grey.
 * @param record Dye amounts of the negative, after the enlarger flare.
 * @return The graded dye amounts, not below 0.
 */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelNegativeGrade(FILM_KERNEL_PARAMS FilmKernelParams* params,
                                                         FilmKernelRgb record) {
  const FilmKernelReal d_r   = record.r - params->negative_grade_grey[0];
  const FilmKernelReal d_g   = record.g - params->negative_grade_grey[1];
  const FilmKernelReal d_b   = record.b - params->negative_grade_grey[2];
  const FilmKernelReal level = (d_r + d_g + d_b) / FILM_KERNEL_REAL(3.0);
  const FilmKernelReal c_r   = d_r - level;
  const FilmKernelReal c_g   = d_g - level;
  const FilmKernelReal c_b   = d_b - level;
  const FilmKernelReal shadow =
      FilmKernelSmoothstep(FILM_KERNEL_REAL(-0.05), FILM_KERNEL_REAL(0.40), -level);
  const FilmKernelReal highlight =
      FilmKernelSmoothstep(FILM_KERNEL_REAL(0.35), FILM_KERNEL_REAL(0.95), level);
  // A colour where green or blue leads: the emitters of signs lose their electric look.
  const FilmKernelReal green_lead =
      FilmKernelMax(c_g - FilmKernelMax(c_r, c_b), FILM_KERNEL_REAL(0.0));
  const FilmKernelReal blue_lead =
      FilmKernelMax(c_b - FilmKernelMax(c_r, c_g), FILM_KERNEL_REAL(0.0));
  const FilmKernelReal cool_lead = green_lead + blue_lead;
  const FilmKernelReal scale = FILM_KERNEL_REAL(0.88) *
                               (FILM_KERNEL_REAL(1.0) - FILM_KERNEL_REAL(0.35) * shadow) *
                               (FILM_KERNEL_REAL(1.0) - FILM_KERNEL_REAL(0.22) * highlight) *
                               (FILM_KERNEL_REAL(1.0) - FILM_KERNEL_REAL(0.45) * cool_lead);
  const FilmKernelReal base = level - FILM_KERNEL_REAL(0.45) * cool_lead;
  return FilmKernelMakeRgb(
      FilmKernelMax(base + scale * c_r - FILM_KERNEL_REAL(0.035) * shadow +
                        params->negative_grade_grey[0],
                    FILM_KERNEL_REAL(0.0)),
      FilmKernelMax(base + scale * c_g + FILM_KERNEL_REAL(0.02) * shadow +
                        params->negative_grade_grey[1],
                    FILM_KERNEL_REAL(0.0)),
      FilmKernelMax(base + scale * c_b + FILM_KERNEL_REAL(0.05) * shadow +
                        params->negative_grade_grey[2],
                    FILM_KERNEL_REAL(0.0)));
}

/** The sRGB code of a linear value in 0 to 1. Rule 'print_cinema_look' is defined on this code. */
FILM_KERNEL_INLINE FilmKernelReal FilmKernelSrgbEncode(FilmKernelReal linear) {
  return linear <= FILM_KERNEL_REAL(0.0031308)
             ? FILM_KERNEL_REAL(12.92) * linear
             : FILM_KERNEL_REAL(1.055) *
                       FILM_KERNEL_POW(linear, FILM_KERNEL_REAL(1.0) / FILM_KERNEL_REAL(2.4)) -
                   FILM_KERNEL_REAL(0.055);
}

FILM_KERNEL_INLINE FilmKernelReal FilmKernelSrgbDecode(FilmKernelReal code) {
  return code <= FILM_KERNEL_REAL(0.04045)
             ? code / FILM_KERNEL_REAL(12.92)
             : FILM_KERNEL_POW((code + FILM_KERNEL_REAL(0.055)) / FILM_KERNEL_REAL(1.055),
                               FILM_KERNEL_REAL(2.4));
}

/** Rule 'print_cinema_look': the curve that prints the lower mids down. Outside 0 to 1: the end
 * value. */
FILM_KERNEL_INLINE FilmKernelReal FilmKernelCinemaPrintDown(FilmKernelReal x) {
  if (x <= FILM_KERNEL_REAL(0.0)) return FILM_KERNEL_REAL(0.0);
  if (x < FILM_KERNEL_REAL(0.08)) return x * (FILM_KERNEL_REAL(0.045) / FILM_KERNEL_REAL(0.08));
  if (x < FILM_KERNEL_REAL(0.20)) {
    return FILM_KERNEL_REAL(0.045) +
           (x - FILM_KERNEL_REAL(0.08)) * (FILM_KERNEL_REAL(0.09) / FILM_KERNEL_REAL(0.12));
  }
  if (x < FILM_KERNEL_REAL(0.42)) {
    return FILM_KERNEL_REAL(0.135) +
           (x - FILM_KERNEL_REAL(0.20)) * (FILM_KERNEL_REAL(0.215) / FILM_KERNEL_REAL(0.22));
  }
  if (x < FILM_KERNEL_REAL(0.65)) {
    return FILM_KERNEL_REAL(0.35) +
           (x - FILM_KERNEL_REAL(0.42)) * (FILM_KERNEL_REAL(0.27) / FILM_KERNEL_REAL(0.23));
  }
  if (x < FILM_KERNEL_REAL(0.85)) {
    return FILM_KERNEL_REAL(0.62) +
           (x - FILM_KERNEL_REAL(0.65)) * (FILM_KERNEL_REAL(0.23) / FILM_KERNEL_REAL(0.20));
  }
  return x < FILM_KERNEL_REAL(1.0) ? x : FILM_KERNEL_REAL(1.0);
}

/**
 * Rule 'print_cinema_look' (lab decision U81): a fixed creative step on the sRGB code of a finished
 * print, which the user selected for print material `cine_look`. All constants are creative
 * choices. The density here is a density of display code, and not an optical density of a film. The steps and
 * the order are those of `cinema_look.py` of alcedo-film-lab.
 *
 * @param linear Linear Rec. 709 in 0 to 1, after FilmKernelFinishDisplay.
 * @return Linear Rec. 709 in 0 to 1.
 */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelCinemaLook(FilmKernelRgb linear) {
  const FilmKernelReal strength   = FILM_KERNEL_REAL(0.65);
  const FilmKernelReal separation = FILM_KERNEL_REAL(1.169);
  const FilmKernelReal luma_r     = FILM_KERNEL_REAL(0.2126);
  const FilmKernelReal luma_g     = FILM_KERNEL_REAL(0.7152);
  const FilmKernelReal luma_b     = FILM_KERNEL_REAL(0.0722);
  const FilmKernelReal smallest   = FILM_KERNEL_REAL(1e-6);
  const FilmKernelReal ten_to     = FILM_KERNEL_REAL(-2.302585092994046);

  // A separation and a coupling of the channels in a density of code.
  const FilmKernelReal d0_r =
      -FILM_KERNEL_LOG10(FilmKernelMax(FilmKernelSrgbEncode(linear.r), smallest));
  const FilmKernelReal d0_g =
      -FILM_KERNEL_LOG10(FilmKernelMax(FilmKernelSrgbEncode(linear.g), smallest));
  const FilmKernelReal d0_b =
      -FILM_KERNEL_LOG10(FilmKernelMax(FilmKernelSrgbEncode(linear.b), smallest));
  const FilmKernelReal mean = (d0_r + d0_g + d0_b) / FILM_KERNEL_REAL(3.0);
  const FilmKernelReal d1_r = mean + separation * (d0_r - mean);
  const FilmKernelReal d1_g = mean + separation * (d0_g - mean);
  const FilmKernelReal d1_b = mean + separation * (d0_b - mean);
  const FilmKernelReal d_r  = FILM_KERNEL_REAL(1.039) * d1_r - FILM_KERNEL_REAL(0.026) * d1_g -
                             FILM_KERNEL_REAL(0.013) * d1_b;
  const FilmKernelReal d_g = -FILM_KERNEL_REAL(0.0195) * d1_r + FILM_KERNEL_REAL(1.026) * d1_g -
                             FILM_KERNEL_REAL(0.0065) * d1_b;
  const FilmKernelReal d_b = -FILM_KERNEL_REAL(0.013) * d1_r - FILM_KERNEL_REAL(0.052) * d1_g +
                             FILM_KERNEL_REAL(1.065) * d1_b;
  const FilmKernelReal x0_r      = FILM_KERNEL_EXP(ten_to * d_r);
  const FilmKernelReal x0_g      = FILM_KERNEL_EXP(ten_to * d_g);
  const FilmKernelReal x0_b      = FILM_KERNEL_EXP(ten_to * d_b);

  // Lower mids printed down.
  const FilmKernelReal keep      = FILM_KERNEL_REAL(1.0) - strength;
  const FilmKernelReal x1_r      = strength * FilmKernelCinemaPrintDown(x0_r) + keep * x0_r;
  const FilmKernelReal x1_g      = strength * FilmKernelCinemaPrintDown(x0_g) + keep * x0_g;
  const FilmKernelReal x1_b      = strength * FilmKernelCinemaPrintDown(x0_b) + keep * x0_b;

  // Blue in the blacks and warm lit surfaces.
  const FilmKernelReal y1        = luma_r * x1_r + luma_g * x1_g + luma_b * x1_b;
  const FilmKernelReal y1_scaled = y1 / FILM_KERNEL_REAL(0.25);
  const FilmKernelReal blue_blacks =
      FILM_KERNEL_EXP(-y1_scaled * y1_scaled) *
      (FILM_KERNEL_REAL(1.0) - FILM_KERNEL_EXP(-y1 / FILM_KERNEL_REAL(0.025)));
  const FilmKernelReal warm = FILM_KERNEL_REAL(4.0) * y1 * (FILM_KERNEL_REAL(1.0) - y1);
  const FilmKernelReal x2_r = FilmKernelClipUnit(
      x1_r + strength * (-FILM_KERNEL_REAL(0.009) * blue_blacks + FILM_KERNEL_REAL(0.012) * warm));
  const FilmKernelReal x2_g = FilmKernelClipUnit(
      x1_g + strength * (FILM_KERNEL_REAL(0.002) * blue_blacks + FILM_KERNEL_REAL(0.002) * warm));
  const FilmKernelReal x2_b = FilmKernelClipUnit(
      x1_b + strength * (FILM_KERNEL_REAL(0.024) * blue_blacks - FILM_KERNEL_REAL(0.009) * warm));

  // A lower chroma, and more density on blue.
  const FilmKernelReal y2   = luma_r * x2_r + luma_g * x2_g + luma_b * x2_b;
  const FilmKernelReal x3_r = y2 + FILM_KERNEL_REAL(0.93) * (x2_r - y2);
  const FilmKernelReal x3_g = y2 + FILM_KERNEL_REAL(0.93) * (x2_g - y2);
  const FilmKernelReal x3_b = y2 + FILM_KERNEL_REAL(0.93) * (x2_b - y2);
  const FilmKernelReal blue3 =
      FilmKernelMax(x3_b - (x3_r + x3_g) / FILM_KERNEL_REAL(2.0), FILM_KERNEL_REAL(0.0));
  const FilmKernelReal chroma = FILM_KERNEL_REAL(1.0) - FILM_KERNEL_REAL(0.65) * blue3;
  const FilmKernelReal dense  = FILM_KERNEL_REAL(1.0) - FILM_KERNEL_REAL(0.70) * blue3;
  const FilmKernelReal x4_r   = (y2 + chroma * (x3_r - y2)) * dense;
  const FilmKernelReal x4_g   = (y2 + chroma * (x3_g - y2)) * dense;
  const FilmKernelReal x4_b   = (y2 + chroma * (x3_b - y2)) * dense;

  // A small shift of blue and green.
  const FilmKernelReal blue4 =
      FILM_KERNEL_REAL(0.12) *
      FilmKernelMax(x4_b - (x4_r + x4_g) / FILM_KERNEL_REAL(2.0), FILM_KERNEL_REAL(0.0));
  const FilmKernelReal green4 =
      FILM_KERNEL_REAL(0.6) *
      FilmKernelMax(x4_g - (x4_r + x4_b) / FILM_KERNEL_REAL(2.0), FILM_KERNEL_REAL(0.0));
  return FilmKernelMakeRgb(
      FilmKernelSrgbDecode(FilmKernelClipUnit(x4_r + FILM_KERNEL_REAL(0.06) * blue4 +
                                              FILM_KERNEL_REAL(0.06) * green4)),
      FilmKernelSrgbDecode(FilmKernelClipUnit(x4_g + FILM_KERNEL_REAL(0.20) * blue4)),
      FilmKernelSrgbDecode(FilmKernelClipUnit(x4_b - FILM_KERNEL_REAL(0.18) * blue4 -
                                              FILM_KERNEL_REAL(0.14) * green4)));
}

#undef FILM_KERNEL_INLINE
#undef FILM_KERNEL_LOG10
#undef FILM_KERNEL_POW
#undef FILM_KERNEL_EXP
#undef FILM_KERNEL_REAL
#undef FILM_KERNEL_VALUES
#undef FILM_KERNEL_PARAMS
#undef FILM_KERNEL_UINT
#endif
