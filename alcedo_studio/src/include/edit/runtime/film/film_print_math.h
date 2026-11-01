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
  FilmKernelReal   locus_keep;   // 1 - the inset of the hull
  FilmKernelReal   uv_scale[2];  // (uv_nodes - 1) / uv_max
  FilmKernelReal   layer_gain[3];
  FilmKernelReal   curve_offset[3];  // Criterion log H - the log H of the first curve value
  FilmKernelReal   curve_step;
  FilmKernelReal   inhibition_k[9];       // Row major
  FilmKernelReal   inhibition_offset[3];  // Criterion log H - the log H of the first F_i value
  FilmKernelReal   inhibition_step;
  FilmKernelReal   print_start[3];
  FilmKernelReal   print_step[3];
  FilmKernelReal   xyz_to_display[9];  // Row major
  FilmKernelReal   gamut_threshold[3];
  FilmKernelReal   gamut_scale[3];
  FilmKernelReal   gamut_power;
  FILM_KERNEL_UINT locus_facets;
  FILM_KERNEL_UINT uv_nodes;
  FILM_KERNEL_UINT curve_stored;
  FILM_KERNEL_UINT curve_lengths[3];
  FILM_KERNEL_UINT inhibition_stored;  // 0: this negative has no inhibition
  FILM_KERNEL_UINT inhibition_lengths[3];
  FILM_KERNEL_UINT print_edge;
  FILM_KERNEL_UINT curves_offset;
  FILM_KERNEL_UINT inhibition_values_offset;
  FILM_KERNEL_UINT normals_offset;
  FILM_KERNEL_UINT room_offset;
  FILM_KERNEL_UINT display_encoding;  // CE_TF_* of the display curve. Read by the backend kernel.
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
  for (FILM_KERNEL_UINT facet = 0U; facet < params->locus_facets; ++facet) {
    const FilmKernelReal rate = dx * normals[2U * facet] + dz * normals[2U * facet + 1U];
    if (rate > FILM_KERNEL_REAL(0.0)) {
      t = FilmKernelMin(t, room[facet] * params->locus_keep * y / rate);
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

/** FP4: the exposure Q to the first dye amounts, on the curves of rule 'dye_amount_curves'. */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelFirstAmounts(FILM_KERNEL_PARAMS FilmKernelParams* params,
                                                        FILM_KERNEL_VALUES FilmKernelReal*   values,
                                                        FilmKernelRgb                        q) {
  FILM_KERNEL_VALUES FilmKernelReal* curves = values + params->curves_offset;
  const FILM_KERNEL_UINT             stored = params->curve_stored;
  const FilmKernelReal               step   = params->curve_step;
  return FilmKernelMakeRgb(
      FilmKernelReadCurve(curves, params->curve_lengths[0],
                          (params->curve_offset[0] + FilmKernelLogExposure(q.r)) / step),
      FilmKernelReadCurve(curves + stored, params->curve_lengths[1],
                          (params->curve_offset[1] + FilmKernelLogExposure(q.g)) / step),
      FilmKernelReadCurve(curves + 2U * stored, params->curve_lengths[2],
                          (params->curve_offset[2] + FilmKernelLogExposure(q.b)) / step));
}

/**
 * FP5: one step of rule 'development_inhibition': a_i = F_i(log H_i - sum_j K_ij I_j).
 * @p inhibitor is the blurred amounts (phase F4), or the amounts themselves with no image plane.
 * Call this only when `params->inhibition_stored` is not 0.
 */
FILM_KERNEL_INLINE FilmKernelRgb FilmKernelInhibitedStep(
    FILM_KERNEL_PARAMS FilmKernelParams* params, FILM_KERNEL_VALUES FilmKernelReal* values,
    FilmKernelRgb q, FilmKernelRgb inhibitor) {
  FILM_KERNEL_VALUES FilmKernelReal* free_curves = values + params->inhibition_values_offset;
  FILM_KERNEL_PARAMS FilmKernelReal* k           = params->inhibition_k;
  const FILM_KERNEL_UINT             stored      = params->inhibition_stored;
  const FilmKernelReal               step        = params->inhibition_step;
  const FilmKernelReal t0 = (params->inhibition_offset[0] + FilmKernelLogExposure(q.r) -
                             k[0] * inhibitor.r - k[1] * inhibitor.g - k[2] * inhibitor.b) /
                            step;
  const FilmKernelReal t1 = (params->inhibition_offset[1] + FilmKernelLogExposure(q.g) -
                             k[3] * inhibitor.r - k[4] * inhibitor.g - k[5] * inhibitor.b) /
                            step;
  const FilmKernelReal t2 = (params->inhibition_offset[2] + FilmKernelLogExposure(q.b) -
                             k[6] * inhibitor.r - k[7] * inhibitor.g - k[8] * inhibitor.b) /
                            step;
  return FilmKernelMakeRgb(
      FilmKernelReadCurve(free_curves, params->inhibition_lengths[0], t0),
      FilmKernelReadCurve(free_curves + stored, params->inhibition_lengths[1], t1),
      FilmKernelReadCurve(free_curves + 2U * stored, params->inhibition_lengths[2], t2));
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

#undef FILM_KERNEL_INLINE
#undef FILM_KERNEL_LOG10
#undef FILM_KERNEL_POW
#undef FILM_KERNEL_EXP
#undef FILM_KERNEL_REAL
#undef FILM_KERNEL_VALUES
#undef FILM_KERNEL_PARAMS
#undef FILM_KERNEL_UINT
#endif
