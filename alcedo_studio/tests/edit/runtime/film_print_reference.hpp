//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "edit/runtime/aces_reference_gamut_compression.h"
#include "edit/runtime/film/film_negative_scan.hpp"
#include "edit/runtime/film/film_print_kernel_tables.hpp"
#include "edit/runtime/film/film_print_tables.hpp"

namespace alcedo::film_print_reference {

#define FILM_KERNEL_DOUBLE
#include "edit/runtime/film/film_print_math.h"
#undef FILM_KERNEL_DOUBLE

struct Rgb {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
};

/**
 * @brief Double-precision form of the point stages, from the same math as the kernels.
 *
 * It runs on the host tables of one selection, with no blur and no Enlarger flare: the entry gamut
 * compression, FP1, FP4, the two steps of FP5, and FP6, then the sRGB curve. A colour reversal film
 * has the light table form of FP6. Print material Negative Scan has no entry gamut compression (the
 * lab option is off for the scan): FP1 to FP5, the scan density T of its scanner table
 * (@ref ScanDensity), and its FP6 up to linear AP1 (@ref NegativeScanLinear); the display transform
 * after it is the OpenDRT of the document.
 */
class FilmPrintPointReference {
 public:
  FilmPrintPointReference(const FilmStockBundle& bundle, const FilmPrintTableData& tables,
                          const std::string& negative, const std::string& print_material,
                          const FilmSceneLightMix& scene_light)
      : values_(
            film_print_kernel_tables::MakeValues<double>(bundle, negative, tables.uv_exposure)) {
    film_print_kernel_tables::FillParams(bundle, negative, print_material, scene_light,
                                         tables.edge, params_);
    // The host table has red varying slowest. The math reads red varying fastest.
    const std::size_t edge = tables.edge;
    print_rgba_.resize(edge * edge * edge * 4, 1.0);
    for (std::size_t b = 0; b < edge; ++b) {
      for (std::size_t g = 0; g < edge; ++g) {
        for (std::size_t r = 0; r < edge; ++r) {
          for (std::size_t k = 0; k < 3; ++k) {
            print_rgba_[((b * edge + g) * edge + r) * 4 + k] =
                tables.print_cube_root_xyz[((r * edge + g) * edge + b) * 3 + k];
          }
        }
      }
    }
  }

  FilmPrintPointReference(const FilmStockBundle& bundle, const FilmPrintTableData& tables,
                          const std::string& negative, const std::string& print_material,
                          const std::string& scene_light)
      : FilmPrintPointReference(bundle, tables, negative, print_material,
                                FilmSceneLightMix{scene_light, scene_light, 0.0}) {}

  /// Negative Scan: linear AP1 to the scan density T, a trilinear read of the scanner table at the
  /// record of the point stages. No entry gamut compression.
  [[nodiscard]] auto ScanDensity(float r, float g, float b) const -> Rgb {
    const auto q = FilmKernelLayerExposure(&params_, values_.data(), r, g, b);
    const auto a = FilmKernelPointAmounts(&params_, values_.data(), q);
    const auto t = FilmKernelReadRecordTable(&params_, print_rgba_.data(), a);
    return {t.r, t.g, t.b};
  }

  /// Negative Scan: FP6 of @p terms on the scan density @p t, linear AP1 before the display
  /// transform.
  [[nodiscard]] static auto NegativeScanLinear(const Rgb&                              t,
                                               const film_negative_scan::RenderTerms& terms)
      -> Rgb {
    FilmKernelNegativeScan kernel{};
    for (std::size_t k = 0; k < 3; ++k) {
      kernel.slope[k]  = terms.slope[k];
      kernel.offset[k] = terms.offset[k];
      kernel.scale[k]  = terms.scale[k];
      kernel.floor[k]  = terms.floor[k];
    }
    const auto linear = FilmKernelNegativeScanPixel(FilmKernelMakeRgb(t.r, t.g, t.b), &kernel);
    return {linear.r, linear.g, linear.b};
  }

  /// Linear AP1 to final sRGB. Not for Negative Scan, which has no sRGB of its own.
  [[nodiscard]] auto Srgb(float r, float g, float b) const -> Rgb {
    const auto compressed = AcesReferenceGamutCompress(r, g, b);
    const auto q =
        FilmKernelLayerExposure(&params_, values_.data(), compressed.r, compressed.g, compressed.b);
    auto a = FilmKernelPointAmounts(&params_, values_.data(), q);
    if (params_.light_table != 0U) {
      // A colour reversal film: the light table, and the surround in display light. A ramp has no
      // image plane, so it has no loupe flare.
      const auto light = FilmKernelPrintDisplay(&params_, print_rgba_.data(), a);
      const auto finished =
          FilmKernelFinishDisplay(&params_, FilmKernelDisplaySurround(&params_, light));
      return {Encode(finished.r), Encode(finished.g), Encode(finished.b)};
    }
    if (params_.negative_grade != 0U) a = FilmKernelNegativeGrade(&params_, a, a);
    const auto finished =
        FilmKernelFinishDisplay(&params_, FilmKernelPrintDisplay(&params_, print_rgba_.data(), a));
    const auto linear = params_.cinema_look != 0U ? FilmKernelCinemaLook(finished) : finished;
    return {Encode(linear.r), Encode(linear.g), Encode(linear.b)};
  }

 private:
  static auto Encode(double x) -> double {
    return x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1.0 / 2.4) - 0.055;
  }

  std::vector<double> values_;
  std::vector<double> print_rgba_;
  FilmKernelParams    params_{};
};

}  // namespace alcedo::film_print_reference
