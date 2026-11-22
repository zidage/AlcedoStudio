//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX3 experiment: the synthetic film selection of VX1 (vx1_runner.cpp, MakeFilmInput, variant
// film_blur): print with the negative grade and the cinema look on sRGB, inhibitor blur on.
// Experiment code: it does not merge.
#include "vx3_film_tables.hpp"

#include <algorithm>
#include <cmath>

namespace vx3 {
namespace {

float Logistic(float t, float centre, float width) {
  return 1.0f / (1.0f + std::exp(-(t - centre) / width));
}

}  // namespace

FilmTables MakeFilmTables() {
  FilmTables        in;
  FilmKernelParams& p = in.params;
  const float ap1_to_xyz[9] = {0.6624542f,  0.1340042f, 0.1561877f, 0.2722287f, 0.6740818f,
                               0.0536895f, -0.0055746f, 0.0040607f, 1.0103391f};
  std::copy(ap1_to_xyz, ap1_to_xyz + 9, p.ap1_to_xyz);
  p.locus_white[0] = 0.95047f;
  p.locus_white[1] = 1.08883f;
  p.locus_keep     = 0.98f;

  const std::uint32_t nodes = 64, facets = 24, curve_stored = 280, inhibition_stored = 300;
  p.uv_nodes                = nodes;
  p.uv_scale[0]             = float(nodes - 1) / 0.65f;
  p.uv_scale[1]             = float(nodes - 1) / 0.62f;
  const std::size_t uv_values = std::size_t(nodes) * nodes * 3;
  p.curves_offset             = static_cast<std::uint32_t>(uv_values);
  p.inhibition_values_offset  = p.curves_offset + 3 * curve_stored;
  p.normals_offset            = p.inhibition_values_offset + 3 * inhibition_stored;
  p.room_offset               = p.normals_offset + 2 * facets;
  in.values.resize(p.room_offset + facets);

  const float centres[3][2] = {{0.45f, 0.52f}, {0.12f, 0.57f}, {0.17f, 0.16f}};
  for (std::uint32_t iu = 0; iu < nodes; ++iu)
    for (std::uint32_t iv = 0; iv < nodes; ++iv) {
      const float u = float(iu) * 0.65f / float(nodes - 1);
      const float v = float(iv) * 0.62f / float(nodes - 1);
      for (int k = 0; k < 3; ++k) {
        const float du = u - centres[k][0], dv = v - centres[k][1];
        in.values[(std::size_t(iu) * nodes + iv) * 3 + k] =
            0.6f + 0.4f * std::exp(-(du * du + dv * dv) / 0.05f);
      }
    }
  const float layer_gain[3] = {1.1f, 1.0f, 0.9f};
  std::copy(layer_gain, layer_gain + 3, p.layer_gain);

  p.curve_stored     = curve_stored;
  p.curve_lengths[0] = 280;
  p.curve_lengths[1] = 270;
  p.curve_lengths[2] = 260;
  p.curve_step       = 0.025f;
  for (int k = 0; k < 3; ++k) {
    p.curve_offset[k] = 5.0f + 0.1f * float(k);
    for (std::uint32_t i = 0; i < curve_stored; ++i) {
      in.values[p.curves_offset + k * curve_stored + i] =
          0.15f + 2.6f * Logistic(float(i), 140.0f + 5.0f * float(k), 30.0f);
    }
  }
  p.inhibition_stored = inhibition_stored;
  for (int k = 0; k < 3; ++k) {
    p.inhibition_lengths[k] = inhibition_stored;
    p.inhibition_offset[k]  = 5.5f;
    for (std::uint32_t i = 0; i < inhibition_stored; ++i) {
      in.values[p.inhibition_values_offset + k * inhibition_stored + i] =
          0.12f + 2.8f * Logistic(float(i), 150.0f, 32.0f);
    }
  }
  p.inhibition_step = 0.025f;
  const float k_total[9] = {0.25f, 0.05f, 0.03f, 0.04f, 0.22f, 0.05f, 0.03f, 0.06f, 0.2f};
  for (int i = 0; i < 9; ++i) {
    p.inhibition_k[i]       = k_total[i];
    p.inhibition_image_k[i] = 0.4f * k_total[i];
    p.inhibition_point_k[i] = 0.6f * k_total[i];
  }
  p.inhibition_has_image = 1;
  p.inhibition_has_point = 1;

  float min_room = 1e9f;
  p.locus_facets = facets;
  for (std::uint32_t f = 0; f < facets; ++f) {
    const float angle = 6.2831853f * float(f) / float(facets);
    in.values[p.normals_offset + 2 * f]     = std::cos(angle);
    in.values[p.normals_offset + 2 * f + 1] = std::sin(angle);
    const float room = 1.2f + 0.5f * std::sin(3.0f * angle + 0.4f);
    in.values[p.room_offset + f]            = room;
    min_room                                = std::min(min_room, room);
  }
  p.locus_inner_squared = 0.9f * min_room * min_room;

  const std::uint32_t edge = 33;
  p.print_edge             = edge;
  for (int k = 0; k < 3; ++k) {
    p.print_start[k] = 0.1f;
    p.print_step[k]  = 3.0f / float(edge - 1);
  }
  in.print_rgba.resize(std::size_t(edge) * edge * edge * 4);
  for (std::uint32_t ib = 0; ib < edge; ++ib)
    for (std::uint32_t ig = 0; ig < edge; ++ig)
      for (std::uint32_t ir = 0; ir < edge; ++ir) {
        const float tr = std::pow(10.0f, -(0.1f + p.print_step[0] * float(ir)));
        const float tg = std::pow(10.0f, -(0.1f + p.print_step[1] * float(ig)));
        const float tb = std::pow(10.0f, -(0.1f + p.print_step[2] * float(ib)));
        const float x  = 0.40f * tr + 0.35f * tg + 0.18f * tb;
        const float y  = 0.20f * tr + 0.70f * tg + 0.08f * tb;
        const float z  = 0.02f * tr + 0.10f * tg + 0.95f * tb;
        float*      n  = &in.print_rgba[((std::size_t(ib) * edge + ig) * edge + ir) * 4];
        n[0]           = std::cbrt(x);
        n[1]           = std::cbrt(y);
        n[2]           = std::cbrt(z);
        n[3]           = 1.0f;
      }
  const float xyz_to_display[9] = {3.2404542f,  -1.5371385f, -0.4985314f, -0.9692660f, 1.8760108f,
                                   0.0415560f,  0.0556434f,  -0.2040259f, 1.0572252f};
  std::copy(xyz_to_display, xyz_to_display + 9, p.xyz_to_display);
  for (int k = 0; k < 3; ++k) {
    p.gamut_threshold[k] = 0.75f;
    p.gamut_scale[k]     = 0.2f;
  }
  p.gamut_power            = 1.2f;
  p.negative_grade_grey[0] = 1.0f;
  p.negative_grade_grey[1] = 1.05f;
  p.negative_grade_grey[2] = 1.1f;
  p.display_luminance[0]   = 0.2126f;
  p.display_luminance[1]   = 0.7152f;
  p.display_luminance[2]   = 0.0722f;
  p.cinema_look            = 1;
  p.negative_grade         = 1;
  p.display_encoding       = CE_TF_SRGB;
  return in;
}

}  // namespace vx3
