//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//
// VX3 experiment: the synthetic film selection of VX1. Experiment code: it does not merge.
#pragma once

#include <vector>

#include "color/color_encoding_math.h"
#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/film/film_print_math.h"

namespace vx3 {

struct FilmTables {
  FilmKernelParams   params{};
  std::vector<float> values;
  std::vector<float> print_rgba;
};

FilmTables MakeFilmTables();

}  // namespace vx3
