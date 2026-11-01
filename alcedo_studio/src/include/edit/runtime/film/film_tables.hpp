//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Construction of the spectral tables of the film model (alcedo-film-lab, ROADMAP U68). This is a
// copy of the lab library, and it adds TableBandIntegral. The Film Print DRT Plan records the lab
// commit. It has no dependency on Qt or on a GPU.
//
// The print table has three stages, so that a print control does not repeat a spectral integral:
//   A. negative dye amounts -> log10 of the unbalanced paper exposure (Log10BandResponse)
//   B. paper Status A -> print XYZ, by the dye solve of the paper (BuildPaperTable)
//   C. stage A values -> printer balance, print contrast, flash, paper curves -> stage B table
//   (ComposePrintTable)
//
// All values are float64. Array layouts are row major. Densities are decadic.
#pragma once

#include <cstdint>

namespace alcedo::film_tables {

inline constexpr int kLayers = 3;

// A material and three integration weights, on one set of wavelengths.
struct SpectralBand {
  int64_t       wavelengths;
  const double* base;     // (wavelengths): density with no dye
  const double* dyes;     // (3, wavelengths): density per unit of dye amount
  const double* weights;  // (3, wavelengths): integration weight of each band
};

// The characteristic curves of the paper on one uniform log H step. A shorter curve keeps its last
// value in the stored array.
struct PaperCurves {
  int64_t        stored_length;
  const double*  density;      // (3, stored_length)
  const int64_t* lengths;      // (3)
  const double*  log_h_start;  // (3)
  double         log_h_step;
};

// The axes of the paper table: the same node count on each of the three Status A axes. Node k of an
// axis has the density start + range * u^power, with u = k / (nodes - 1). A power above 1 puts more
// nodes at a low density.
struct StatusAxes {
  int64_t       nodes;
  const double* start;  // (3)
  const double* range;  // (3)
  double        power;
};

// The values of the print that do not need a spectral integral.
struct PrintControls {
  const double* balance_log10;  // (3): printer lights, added to the stage A value
  const double* aim_log_h;      // (3): paper log exposure about which the contrast acts
  const double* contrast;       // (3): print contrast times the neutral printing gain
  double        flash;          // uniform paper exposure, as a share of the exposure at the aim
  const double* surface_xyz;    // (3): reflection of the print surface, added to the print XYZ
};

struct ComposeCounts {
  int64_t outside_curve;      // layer values of paper exposure outside the characteristic curve
  int64_t print_not_reached;  // table nodes that read a paper node where the dye solve did not
                              // reach the target
};

// threads <= 0 uses the hardware thread count in each function.

// log10(sum over wavelengths of T * weight) for each band, with T = 10^-(base + amounts @ dyes).
// amounts: (count, 3). out: (count, 3).
void Log10BandResponse(const SpectralBand& band, const double* amounts, int64_t count, double* out,
                       int threads = 0);

// Projected Newton: amounts >= 0 with band density(base + amounts @ dyes) = targets. iterations and
// the stop rule are those of the host evaluator. targets, amounts: (count, 3). Returns the count
// with a largest residual above reached_limit.
int64_t SolveDyeAmounts(const SpectralBand& status, const double* targets, int64_t count,
                        int iterations, double reached_limit, double* amounts, int threads = 0);

// Stage B on all nodes of the axes. status_a and viewing share the dyes of the paper, on their own
// wavelengths. cube_root_xyz: (nodes, nodes, nodes, 3), the cube root of xyz_scale * sum(T *
// weight), with no surface reflection. not_reached: (nodes, nodes, nodes), 1 where the largest
// residual of the solve is above reached_limit.
void    BuildPaperTable(const SpectralBand& status_a, const SpectralBand& viewing, double xyz_scale,
                        const StatusAxes& axes, int iterations, double reached_limit,
                        double* cube_root_xyz, uint8_t* not_reached, int threads = 0);

// The integral of each row of a float32 table over wavelength, for three bands.
// table: (count, wavelengths). weights: (3, wavelengths). out: (count, 3).
void    TableBandIntegral(const float* table, int64_t count, int64_t wavelengths,
                          const double* weights, double* out, int threads = 0);

// Stage C. paper_log: (count, 3), stage A values. paper_table, not_reached: the output of
// BuildPaperTable. cube_root_xyz: (count, 3), the cube root of the print XYZ with the surface
// reflection.
ComposeCounts ComposePrintTable(const double* paper_log, int64_t count,
                                const PrintControls& controls, const PaperCurves& curves,
                                const StatusAxes& axes, const double* paper_table,
                                const uint8_t* not_reached, double* cube_root_xyz, int threads = 0);

}  // namespace alcedo::film_tables
