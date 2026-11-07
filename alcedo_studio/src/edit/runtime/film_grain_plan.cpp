//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/film_grain_plan.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>

namespace alcedo {
namespace {

/// Bisection of rule 'grain_kernel', as the lab evaluator: 60 steps of sigma_b in [1e-3, 8].
constexpr double  kSigmaBLow      = 1e-3;
constexpr double  kSigmaBHigh     = 8.0;
constexpr int     kBisectionSteps = 60;
constexpr double  kMaxStrength    = 2.0;

[[noreturn]] void FailField(std::string_view field, const std::string& reason) {
  throw std::invalid_argument("Film grain plan: " + std::string(field) + " " + reason);
}

/// The unit normal density.
auto NormalDensity(double x) -> double {
  return std::exp(-0.5 * x * x) / std::sqrt(2.0 * std::numbers::pi);
}

/// The weights of rule 'grain_kernel' for one sigma_b, one axis: exp(-i^2 / (2 sigma_b^2)) over
/// the radius min(ceil(3 sigma_b), 24), divided by their sum. Index 0 is the tap -radius.
auto GaussianKernel(double sigma_b, std::array<double, 2 * kFilmGrainMaxBlurRadius + 1>& weights)
    -> std::uint32_t {
  const auto radius = static_cast<std::uint32_t>(
      (std::min)(std::ceil(3.0 * sigma_b), static_cast<double>(kFilmGrainMaxBlurRadius)));
  double sum = 0.0;
  for (std::uint32_t index = 0; index <= 2 * radius; ++index) {
    const double tap = static_cast<double>(index) - static_cast<double>(radius);
    weights[index]   = std::exp(-tap * tap / (2.0 * sigma_b * sigma_b));
    sum += weights[index];
  }
  for (std::uint32_t index = 0; index <= 2 * radius; ++index) {
    weights[index] /= sum;
  }
  return radius;
}

auto SquareSum(std::span<const double> weights) -> double {
  double sum = 0.0;
  for (const double weight : weights) {
    sum += weight * weight;
  }
  return sum;
}

/// The lag-1 correlation of a blur of unit white noise: sum w_i w_(i+1) / sum w^2.
auto KernelLag1(std::span<const double> weights) -> double {
  double sum = 0.0;
  for (std::size_t index = 0; index + 1 < weights.size(); ++index) {
    sum += weights[index] * weights[index + 1];
  }
  return sum / SquareSum(weights);
}

/// C(d) of the continuous pixel field (lab `pixel_covariance`): the triangle of the pixel box
/// against the normal of variance 2 sigma_c^2, (1 / p) [F(-p) - 2 F(0) + F(p)] with
/// F(a) = E[(X - a)+], X ~ N(d, 2 sigma_c^2).
auto PixelCovariance(double cloud_sigma_um, double pitch_um, double lag_um) -> double {
  const double s       = std::numbers::sqrt2 * cloud_sigma_um;
  const auto   partial = [&](double a) {
    const double z = (lag_um - a) / s;
    return (lag_um - a) * 0.5 * (1.0 + std::erf(z / std::numbers::sqrt2)) + s * NormalDensity(z);
  };
  return (partial(-pitch_um) - 2.0 * partial(0.0) + partial(pitch_um)) / pitch_um;
}

/// Rule 'grain_kernel': the sigma_b whose discrete kernel has the lag-1 of the continuous field.
auto MatchedSigmaB(double cloud_sigma_um, double pitch_um) -> double {
  const double target = FilmGrainPixelLag1(cloud_sigma_um, pitch_um);
  std::array<double, 2 * kFilmGrainMaxBlurRadius + 1> weights{};
  double                                              low  = kSigmaBLow;
  double                                              high = kSigmaBHigh;
  for (int step = 0; step < kBisectionSteps; ++step) {
    const double middle = 0.5 * (low + high);
    const auto   radius = GaussianKernel(middle, weights);
    if (KernelLag1(std::span(weights).first(2 * radius + 1)) < target) {
      low = middle;
    } else {
      high = middle;
    }
  }
  return 0.5 * (low + high);
}

/// Rule 'grain_layer_mix': the lower Cholesky factor of [[1, rg, rb], [rg, 1, gb], [rb, gb, 1]].
auto CorrelationCholesky(std::span<const double> rg_gb_rb, std::string_view field)
    -> std::array<double, 9> {
  if (rg_gb_rb.size() != 3) {
    FailField(field, "does not hold 3 values");
  }
  const double rg     = rg_gb_rb[0];
  const double gb     = rg_gb_rb[1];
  const double rb     = rg_gb_rb[2];
  const double l11_sq = 1.0 - rg * rg;
  if (!(l11_sq > 0.0)) {
    FailField(field, "is not positive definite");
  }
  const double l11    = std::sqrt(l11_sq);
  const double l21    = (gb - rb * rg) / l11;
  const double l22_sq = 1.0 - rb * rb - l21 * l21;
  if (!(l22_sq > 0.0)) {
    FailField(field, "is not positive definite");
  }
  return {1.0, 0.0, 0.0, rg, l11, 0.0, rb, l21, std::sqrt(l22_sq)};
}

/// Rule 'grain_aggregate'.
auto Aggregate(double render_scale) -> std::uint32_t {
  if (render_scale >= 1.0) {
    return 1;
  }
  // Round half to even, as the lab (Python round).
  const double k = std::nearbyint(1.0 / render_scale);
  return static_cast<std::uint32_t>(
      std::clamp(k, 1.0, static_cast<double>(kFilmGrainMaxAggregate)));
}

}  // namespace

auto FilmGrainPixelIntegral(double cloud_sigma_um, double pitch_um) -> double {
  const double q = pitch_um / (std::numbers::sqrt2 * cloud_sigma_um);
  return (std::erf(q / std::numbers::sqrt2) - 2.0 * (NormalDensity(0.0) - NormalDensity(q)) / q) /
         pitch_um;
}

auto FilmGrainPixelLag1(double cloud_sigma_um, double pitch_um) -> double {
  return PixelCovariance(cloud_sigma_um, pitch_um, pitch_um) /
         PixelCovariance(cloud_sigma_um, pitch_um, 0.0);
}

auto MakeFilmGrainLayerKernel(double cloud_sigma_um, double aperture_area_um2, double pitch_um)
    -> FilmGrainLayerKernel {
  if (!std::isfinite(cloud_sigma_um) || !(cloud_sigma_um > 0.0)) {
    FailField("grain_cloud_sigma_um", "is not above 0");
  }
  if (!std::isfinite(aperture_area_um2) || !(aperture_area_um2 > 0.0)) {
    FailField("grain_aperture_area_um2", "is not above 0");
  }
  if (!std::isfinite(pitch_um) || !(pitch_um > 0.0)) {
    FailField("pitch_um", "is not above 0");
  }
  FilmGrainLayerKernel kernel;
  kernel.cloud_sigma_um = cloud_sigma_um;
  kernel.sigma_b_px     = MatchedSigmaB(cloud_sigma_um, pitch_um);
  std::array<double, 2 * kFilmGrainMaxBlurRadius + 1> full{};
  kernel.radius            = GaussianKernel(kernel.sigma_b_px, full);
  const auto taps          = std::span(full).first(2 * kernel.radius + 1);
  kernel.weight_square_sum = SquareSum(taps);
  kernel.lag1              = KernelLag1(taps);
  for (std::uint32_t tap = 0; tap <= kernel.radius; ++tap) {
    kernel.weights[tap] = full[kernel.radius + tap];
  }
  kernel.pixel_factor =
      std::sqrt(aperture_area_um2) * FilmGrainPixelIntegral(cloud_sigma_um, pitch_um);
  kernel.amplitude = kernel.pixel_factor / kernel.weight_square_sum;
  return kernel;
}

auto MakeFilmGrainCurve(const FilmGrainCurveView& curves, std::size_t layer, std::string_view field)
    -> FilmGrainCurve {
  if (layer >= curves.node_count.size()) {
    FailField(field, "has no curve " + std::to_string(layer));
  }
  const auto count = curves.node_count[layer];
  if (count < 2 || count > FILM_GRAIN_CURVE_NODES || count > curves.stored_nodes) {
    FailField(field, "curve " + std::to_string(layer) + " has " + std::to_string(count) +
                         " nodes; it needs 2 to " + std::to_string(FILM_GRAIN_CURVE_NODES));
  }
  const auto     first = static_cast<std::size_t>(curves.stored_nodes) * layer;
  const auto     x     = curves.x.subspan(first, static_cast<std::size_t>(count));
  const auto     sigma = curves.sigma.subspan(first, static_cast<std::size_t>(count));
  FilmGrainCurve curve{};
  curve.first_sigma = static_cast<float>(sigma[0]);
  for (std::size_t segment = 0; segment + 1 < x.size(); ++segment) {
    const double width = x[segment + 1] - x[segment];
    if (!(width > 0.0)) {
      FailField(field, "curve " + std::to_string(layer) + " does not rise");
    }
    curve.start[segment]         = static_cast<float>(x[segment]);
    curve.inverse_width[segment] = static_cast<float>(1.0 / width);
    curve.delta[segment]         = static_cast<float>(sigma[segment + 1] - sigma[segment]);
  }
  return curve;
}

auto MakeFilmGrainPlan(const ResolvedRenderGeometry& geometry, FilmFormat format,
                       const FilmGrainStockView& stock, FilmGrainTarget target, float strength)
    -> FilmGrainPlan {
  const double frame_mm = FilmFrameLongSideMm(format);
  if (!std::isfinite(strength) || strength < 0.0f || strength > kMaxStrength) {
    FailField("strength", "is " + std::to_string(strength) + "; it must be in [0, 2]");
  }
  const auto reference = geometry.full_reference_extent;
  if (reference.Empty()) {
    FailField("geometry.full_reference_extent", "is empty");
  }
  if (stock.common.cloud_sigma_um.size() != 3) {
    FailField("grain_cloud_sigma_um", "does not hold 3 values");
  }
  if (target != FilmGrainTarget::Post && target != FilmGrainTarget::Negative) {
    FailField("target", "is not a grain target");
  }

  FilmGrainPlan plan;
  plan.target   = target;
  plan.format   = format;
  plan.strength = strength;
  plan.enabled  = strength > 0.0f;
  plan.reference_pitch_um =
      frame_mm * 1000.0 / static_cast<double>((std::max)(reference.width, reference.height));
  plan.render_scale    = 0.5 * (static_cast<double>(RenderAxisScale(geometry, true)) +
                             static_cast<double>(RenderAxisScale(geometry, false)));
  plan.render_pitch_um = plan.reference_pitch_um / plan.render_scale;
  plan.aggregate       = Aggregate(plan.render_scale);
  for (std::size_t layer = 0; layer < 3; ++layer) {
    plan.layers[layer] = MakeFilmGrainLayerKernel(
        stock.common.cloud_sigma_um[layer], stock.common.aperture_area_um2, plan.render_pitch_um);
  }
  const bool negative = target == FilmGrainTarget::Negative;
  plan.mix =
      negative
          ? CorrelationCholesky(stock.common.layer_correlation, "grain_layer_correlation")
          : CorrelationCholesky(kFilmGrainPostScanCorrelation, "kFilmGrainPostScanCorrelation");
  const auto& curves = negative ? stock.layer : stock.channel;
  for (std::size_t layer = 0; layer < 3; ++layer) {
    plan.curves[layer] =
        MakeFilmGrainCurve(curves, layer, negative ? "grain layer curve" : "grain channel curve");
    const auto first      = static_cast<std::size_t>(curves.stored_nodes) * layer;
    plan.curve_low[layer] = curves.x[first];
    plan.curve_high[layer] =
        curves.x[first + static_cast<std::size_t>(curves.node_count[layer]) - 1];
  }
  if (!negative) {
    if (stock.common.post_display_value.size() != 3 ||
        stock.common.post_display_sigma_48.size() != 3) {
      FailField("grain_post_display_value", "does not hold 3 values");
    }
    plan.post_grey = stock.common.post_display_value[1];
    if (!(plan.post_grey > 0.0 && plan.post_grey < 1.0) ||
        !(stock.common.post_display_sigma_48[1] > 0.0)) {
      FailField("grain_post_display_value", "of green is not inside (0, 1)");
    }
    const double green = stock.common.post_display_sigma_48[1] / plan.post_grey;
    for (std::size_t channel = 0; channel < 3; ++channel) {
      plan.post_relative_sigma_48[channel] = green * kFilmGrainPostChannelRatio[channel];
    }
  }
  return plan;
}

auto MakeFilmGrainFieldParams(const FilmGrainPlan& plan, const ResolvedRenderGeometry& geometry)
    -> FilmGrainFieldParams {
  FilmGrainFieldParams params{};
  const auto&          matrix = geometry.render_to_reference.m;
  for (std::size_t entry = 0; entry < 6; ++entry) {
    params.noise.render_to_reference[entry] = matrix[entry];
  }
  params.noise.seed              = kFilmGrainSeed;
  params.noise.aggregate         = plan.aggregate;
  params.noise.inverse_aggregate = static_cast<float>(1.0 / static_cast<double>(plan.aggregate));
  params.halo                    = 0;
  for (std::size_t layer = 0; layer < 3; ++layer) {
    const auto& kernel   = plan.layers[layer];
    params.radius[layer] = static_cast<int>(kernel.radius);
    params.halo          = (std::max)(params.halo, params.radius[layer]);
    for (std::uint32_t tap = 0; tap <= kFilmGrainMaxBlurRadius; ++tap) {
      params.weights[layer][tap] = static_cast<float>(kernel.weights[tap]);
    }
    params.inverse_weight_square_sum[layer] = static_cast<float>(1.0 / kernel.weight_square_sum);
  }
  // The lower triangle of L, row major.
  params.mix[0] = static_cast<float>(plan.mix[0]);
  params.mix[1] = static_cast<float>(plan.mix[3]);
  params.mix[2] = static_cast<float>(plan.mix[4]);
  params.mix[3] = static_cast<float>(plan.mix[6]);
  params.mix[4] = static_cast<float>(plan.mix[7]);
  params.mix[5] = static_cast<float>(plan.mix[8]);
  return params;
}

auto MakeFilmGrainPostParams(const FilmGrainPlan& plan, int transfer, float decode_scale)
    -> FilmGrainPostParams {
  if (plan.target != FilmGrainTarget::Post) {
    FailField("target", "is not the post path");
  }
  if (!std::isfinite(decode_scale) || !(decode_scale > 0.0f)) {
    FailField("decode_scale", "is not above 0");
  }
  FilmGrainPostParams params{};
  params.strength     = static_cast<float>(plan.strength);
  params.transfer     = transfer;
  params.decode_scale = decode_scale;
  const float grey    = FilmGrainPostToneCoordinate(static_cast<float>(plan.post_grey));
  for (std::size_t channel = 0; channel < 3; ++channel) {
    params.pixel_factor[channel] = static_cast<float>(plan.layers[channel].pixel_factor);
    params.low[channel]          = static_cast<float>(plan.curve_low[channel]);
    params.high[channel]         = static_cast<float>(plan.curve_high[channel]);
    params.curves[channel]       = plan.curves[channel];
    const double tone =
        FilmGrainPostTone(&params.curves[channel], params.low[channel], params.high[channel], grey);
    if (!(tone > 0.0)) {
      FailField("grain channel curve", "gives no grain at the grey of the post target");
    }
    params.tone_scale[channel] = static_cast<float>(plan.post_relative_sigma_48[channel] / tone);
  }
  return params;
}

}  // namespace alcedo
