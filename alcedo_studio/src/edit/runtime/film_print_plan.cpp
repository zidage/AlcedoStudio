//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film_print_plan.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

#include "edit/runtime/diffusion_filter_plan.hpp"

namespace alcedo {
namespace {

/// Variance, in pixels of the reduced image, of a box reduction (1/12) and a bilinear enlargement
/// (1/6). The blur on the reduced image takes it out.
constexpr double kReductionVariance = 0.25;

/// Lab function `_kernel`: radius int(4 sigma + 0.5), normalized over that radius.
void             FillLayer(double sigma, std::size_t layer, FilmBlurKernel& kernel) {
  if (!(sigma > 0.0)) {
    kernel.weights[layer][0] = 1.0f;
    return;
  }
  const double extent = 4.0 * sigma + 0.5;
  if (!(extent < static_cast<double>(kFilmBlurMaxRadius + 1))) {
    throw std::runtime_error("Film print: a blur with a sigma of " + std::to_string(sigma) +
                                         " pixels needs more than " + std::to_string(kFilmBlurMaxRadius) +
                                         " taps");
  }
  const auto                                 radius = static_cast<std::uint32_t>(extent);
  double                                     total = 1.0;
  std::array<double, kFilmBlurMaxRadius + 1> weights{};
  weights[0] = 1.0;
  for (std::uint32_t tap = 1; tap <= radius; ++tap) {
    const double position = static_cast<double>(tap) / sigma;
    weights[tap]          = std::exp(-0.5 * position * position);
    total += 2.0 * weights[tap];
  }
  for (std::uint32_t tap = 0; tap <= radius; ++tap) {
    kernel.weights[layer][tap] = static_cast<float>(weights[tap] / total);
  }
  kernel.radius = (std::max)(kernel.radius, radius);
}

auto MakeKernel(const std::array<double, 3>& sigmas) -> FilmBlurKernel {
  FilmBlurKernel kernel;
  for (std::size_t layer = 0; layer < 3; ++layer) {
    FillLayer(sigmas[layer], layer, kernel);
  }
  return kernel;
}

auto MakeKernel(double sigma) -> FilmBlurKernel {
  return MakeKernel(std::array<double, 3>{sigma, sigma, sigma});
}

/// The sigma on a reduced image, with the variance of the reduction and the enlargement taken out.
auto ReducedSigma(double sigma, std::uint32_t reduced, std::uint32_t full) -> double {
  const double scaled = sigma * static_cast<double>(reduced) / static_cast<double>(full);
  return std::sqrt((std::max)(scaled * scaled - kReductionVariance, 0.0));
}

void PlanSpread(const ResolvedRenderGeometry& geometry, const FilmImagePlaneConstants& constants,
                std::span<const double> layer_spread_um, const FilmPrintParams& film,
                double pixel_um, FilmPrintImagePlan& plan) {
  if (!(film.light_spread > 0.0f) ||
      std::ranges::all_of(layer_spread_um, [](double length) { return !(length > 0.0); })) {
    return;
  }
  const double scale_x = RenderAxisScale(geometry, true);
  const double scale_y = RenderAxisScale(geometry, false);
  plan.spread_passes   = 2;
  for (std::size_t pass = 0; pass < 2; ++pass) {
    const double          ratio = constants.spread_gaussians[pass * 2 + 1];
    std::array<double, 3> sigma{};
    for (std::size_t layer = 0; layer < 3; ++layer) {
      sigma[layer] = ratio * layer_spread_um[layer] * film.light_spread / pixel_um;
    }
    plan.spread_x[pass] = MakeKernel({sigma[0] * scale_x, sigma[1] * scale_x, sigma[2] * scale_x});
    plan.spread_y[pass] = MakeKernel({sigma[0] * scale_y, sigma[1] * scale_y, sigma[2] * scale_y});
    plan.spread_weight[pass] = static_cast<float>(constants.spread_gaussians[pass * 2]);
  }
  const auto& narrow_x     = plan.spread_x[0];
  const auto& narrow_y     = plan.spread_y[0];
  const auto  radius       = (std::max)(narrow_x.radius, narrow_y.radius);
  plan.spread_narrow_fused = radius <= kFilmNarrowSpreadMaxRadius;
  if (plan.spread_narrow_fused) {
    plan.spread_narrow.radius = radius;
    for (std::size_t layer = 0; layer < 3; ++layer) {
      for (std::uint32_t tap = 0; tap <= radius; ++tap) {
        plan.spread_narrow.x[layer][tap] = narrow_x.weights[layer][tap];
        plan.spread_narrow.y[layer][tap] = narrow_y.weights[layer][tap];
      }
    }
  }
}

void PlanInhibitor(const ResolvedRenderGeometry& geometry, const FilmImagePlaneConstants& constants,
                   double pixel_um, FilmPrintImagePlan& plan) {
  const double sigma   = constants.inhibition_um / pixel_um;
  const double sigma_x = sigma * RenderAxisScale(geometry, true);
  const double sigma_y = sigma * RenderAxisScale(geometry, false);
  const auto   width   = geometry.render_extent.width;
  const auto   height  = geometry.render_extent.height;
  plan.inhibitor_half_size =
      (std::max)(sigma_x, sigma_y) >= static_cast<double>(kFilmHalfSizeSmallestSigma);
  if (!plan.inhibitor_half_size) {
    plan.inhibitor_extent = {width, height};
    plan.inhibitor_x      = MakeKernel(sigma_x);
    plan.inhibitor_y      = MakeKernel(sigma_y);
    return;
  }
  plan.inhibitor_extent = {(std::max)(width / 2U, 1U), (std::max)(height / 2U, 1U)};
  // A cell has two pixels, also on an odd size. See HalfCellStart.
  plan.inhibitor_x      = MakeKernel(ReducedSigma(sigma_x, 1U, 2U));
  plan.inhibitor_y      = MakeKernel(ReducedSigma(sigma_y, 1U, 2U));
}

void PlanHalation(const ResolvedRenderGeometry& geometry, const FilmImagePlaneConstants& constants,
                  const FilmPrintParams& film, double pixel_um, FilmPrintImagePlan& plan) {
  bool active = false;
  for (std::size_t layer = 0; layer < 3; ++layer) {
    plan.halation_strength[layer] =
        static_cast<float>(constants.halation_strength[layer] * film.halation);
    active = active || plan.halation_strength[layer] > 0.0f;
  }
  plan.halation_active = active;
  if (!active) {
    return;
  }
  const auto   reference = geometry.full_reference_extent;
  const auto   canvas    = plan.canvas_extent;
  const double sigma     = constants.halation_um / pixel_um;
  // A canvas at the size of the frame has no reduction, and a read at a texel center is exact.
  plan.halation_x        = canvas.width < reference.width
                               ? MakeKernel(ReducedSigma(sigma, canvas.width, reference.width))
                               : MakeKernel(sigma);
  plan.halation_y        = canvas.height < reference.height
                               ? MakeKernel(ReducedSigma(sigma, canvas.height, reference.height))
                               : MakeKernel(sigma);
}

void PlanFlare(const FilmImagePlaneConstants& constants, const FilmPrintParams& film,
               FilmPrintImagePlan& plan) {
  // Lab Scan has no enlarger, so it reads no Enlarger flare and builds no canvas for it.
  const bool scan   = IsFilmLabScan(film.negative, film.print_material);
  plan.flare        = scan ? 0.0f : film.enlarger_flare;
  plan.flare_active = plan.flare > 0.0f;
  if (!plan.flare_active) {
    return;
  }
  const auto   canvas = plan.canvas_extent;
  const double sigma =
      constants.enlarger_flare_size * static_cast<double>((std::max)(canvas.width, canvas.height));
  if (sigma < static_cast<double>(kFilmLargeSigma)) {
    plan.flare_extent    = canvas;
    plan.flare_x         = MakeKernel(sigma);
    plan.flare_y         = MakeKernel(sigma);
    plan.render_to_flare = plan.render_to_canvas;
    return;
  }
  const auto factor = static_cast<double>(
      static_cast<std::uint32_t>(sigma / static_cast<double>(kFilmReducedBlurSigma)));
  plan.flare_extent = {
      (std::max)(static_cast<std::uint32_t>(std::lround(canvas.width / factor)), 2U),
      (std::max)(static_cast<std::uint32_t>(std::lround(canvas.height / factor)), 2U)};
  plan.flare_x = MakeKernel(ReducedSigma(sigma, plan.flare_extent.width, canvas.width));
  plan.flare_y = MakeKernel(ReducedSigma(sigma, plan.flare_extent.height, canvas.height));
  plan.render_to_flare =
      Matrix3x3::Scale(
          static_cast<float>(plan.flare_extent.width) / static_cast<float>(canvas.width),
          static_cast<float>(plan.flare_extent.height) / static_cast<float>(canvas.height)) *
      plan.render_to_canvas;
}

}  // namespace

auto MakeFilmPrintImagePlan(const ResolvedRenderGeometry&  geometry,
                            const FilmImagePlaneConstants& constants,
                            std::span<const double> layer_spread_um, bool has_inhibitor_blur,
                            const FilmPrintParams& film) -> FilmPrintImagePlan {
  FilmPrintImagePlan plan;
  const auto         reference = geometry.full_reference_extent;
  plan.image_plane = (std::min)(reference.width, reference.height) >= kFilmImagePlaneSmallestEdge;
  if (!plan.image_plane) {
    return plan;
  }
  const double pixel_um = constants.frame_mm * 1000.0 /
                          static_cast<double>((std::max)(reference.width, reference.height));
  plan.canvas_extent    = DiffusionCanvasExtent(reference);
  const auto mapping    = MakeDiffusionScatterMapping(geometry, plan.canvas_extent);
  plan.render_to_canvas = mapping.render_to_base;
  plan.canvas_to_render = mapping.base_to_render;
  plan.canvas_samples   = mapping.reduce_samples;
  PlanSpread(geometry, constants, layer_spread_um, film, pixel_um, plan);
  if (has_inhibitor_blur) {
    PlanInhibitor(geometry, constants, pixel_um, plan);
  }
  PlanHalation(geometry, constants, film, pixel_um, plan);
  PlanFlare(constants, film, plan);
  return plan;
}

}  // namespace alcedo
