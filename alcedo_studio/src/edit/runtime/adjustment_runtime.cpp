//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/adjustment_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"

namespace alcedo {
namespace {

constexpr std::size_t   kMaxCurvePoints             = 8;
constexpr float         kHalationSigma              = 7.0f;
constexpr float         kHalationStrengthScale      = 2.0f;
constexpr float         kHalationRedshift[3]        = {1.0f, 0.05f, 0.02f};
constexpr float         kClarityNeighborhoodSigma   = 15.0f;
constexpr float         kFilmGrainDyeCloudSigma     = 0.8f;
constexpr std::uint32_t kSharpenMaxRadius           = 15U;
constexpr std::uint32_t kClarityMaxRadius           = 60U;
constexpr std::uint32_t kFilmGrainMaxRadius         = 3U;
constexpr std::uint64_t kFilmGrainSeed              = 0x6a09e667f3bcc909ULL;

void BuildGaussianWeights(float sigma, std::uint32_t max_radius, GradeNeighborParams& result) {
  if (!(sigma > 0.0f)) {
    return;
  }
  const float safe_sigma = std::max(sigma, 1.0e-4f);
  result.radius =
      std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::ceil(3.0f * safe_sigma)), 1U,
                                std::min(max_radius, kGradeNeighborMaxTapCount - 1U));
  result.tap_count = result.radius + 1U;

  const double inv_two_sigma_squared =
      0.5 / (static_cast<double>(safe_sigma) * static_cast<double>(safe_sigma));
  double full_weight = 1.0;
  result.weights[0]  = 1.0f;
  for (std::uint32_t tap = 1; tap <= result.radius; ++tap) {
    const auto   distance = static_cast<double>(tap);
    const double weight   = std::exp(-(distance * distance) * inv_two_sigma_squared);
    result.weights[tap]   = static_cast<float>(weight);
    full_weight += 2.0 * weight;
  }
  for (std::uint32_t tap = 0; tap <= result.radius; ++tap) {
    result.weights[tap] =
        static_cast<float>(static_cast<double>(result.weights[tap]) / full_weight);
  }
}

auto RenderAxisScale(const ResolvedRenderGeometry& geometry, bool horizontal) -> float {
  const auto& matrix                            = geometry.render_to_reference.m;
  const float dx                                = horizontal ? matrix[0] : matrix[1];
  const float dy                                = horizontal ? matrix[3] : matrix[4];
  const float reference_pixels_per_render_pixel = std::hypot(dx, dy);
  if (!(reference_pixels_per_render_pixel > 0.0f) ||
      !std::isfinite(reference_pixels_per_render_pixel)) {
    return 1.0f;
  }
  return std::clamp(1.0f / reference_pixels_per_render_pixel, 1.0e-4f, 1.0f);
}

/** @brief Mean of the X/Y render-to-reference scales, clamped like the per-axis helper. */
auto NeighborhoodRenderScale(const ResolvedRenderGeometry& geometry) -> float {
  return 0.5f * (RenderAxisScale(geometry, true) + RenderAxisScale(geometry, false));
}

void CopyRenderMapping(const ResolvedRenderGeometry& geometry, GradeNeighborParams& result) {
  const auto& matrix               = geometry.render_to_reference.m;
  result.render_to_reference[0]    = matrix[0];
  result.render_to_reference[1]    = matrix[1];
  result.render_to_reference[2]    = matrix[2];
  result.render_to_reference[3]    = matrix[3];
  result.render_to_reference[4]    = matrix[4];
  result.render_to_reference[5]    = matrix[5];
  result.use_reference_coordinates = CoversFullEditSpace(geometry) ? 1U : 0U;
  result.reference_width  = static_cast<std::int32_t>(geometry.full_reference_extent.width);
  result.reference_height = static_cast<std::int32_t>(geometry.full_reference_extent.height);
}

}  // namespace

auto TryResolveAdjustmentBehavior(const OperatorTypeId& type) -> std::optional<AdjustmentBehavior> {
  using enum AdjustmentBehavior;
  if (type == type_ids::Cat02WhiteBalance()) return Cat02WhiteBalance;
  if (type == type_ids::Exposure()) return Exposure;
  if (type == type_ids::Contrast()) return Contrast;
  if (type == type_ids::White()) return White;
  if (type == type_ids::Black()) return Black;
  if (type == type_ids::Shadows()) return Shadows;
  if (type == type_ids::Highlights()) return Highlights;
  if (type == type_ids::Curve()) return Curve;
  if (type == type_ids::Hls()) return Hls;
  if (type == type_ids::Saturation()) return Saturation;
  if (type == type_ids::Vibrance()) return Vibrance;
  if (type == type_ids::ColorWheel()) return ColorWheel;
  if (type == type_ids::Lmt()) return Lmt;
  if (type == type_ids::Clarity()) return Clarity;
  if (type == type_ids::Sharpen()) return Sharpen;
  if (type == type_ids::Halation()) return Halation;
  if (type == type_ids::FilmGrain()) return FilmGrain;
  return std::nullopt;
}

auto ResolveAdjustmentBehavior(const OperatorTypeId& type) -> AdjustmentBehavior {
  if (auto behavior = TryResolveAdjustmentBehavior(type)) {
    return *behavior;
  }
  throw std::runtime_error("Primary grade: unregistered adjustment type '" +
                           std::string{type.Text()} + "'");
}

auto IsLocalToneBehavior(AdjustmentBehavior behavior) -> bool {
  return behavior == AdjustmentBehavior::Shadows || behavior == AdjustmentBehavior::Highlights;
}

auto IsNeighborhoodBehavior(AdjustmentBehavior behavior) -> bool {
  return behavior == AdjustmentBehavior::Clarity || behavior == AdjustmentBehavior::Sharpen ||
         behavior == AdjustmentBehavior::Halation || behavior == AdjustmentBehavior::FilmGrain;
}

auto MakeGradeRuntimeParams(const IOperatorModel& model, AdjustmentBehavior behavior)
    -> GradeAdjustmentParams {
  GradeAdjustmentParams result;
  result.behavior = static_cast<std::uint32_t>(behavior);
  const auto dto  = model.MakeFullDto();

  if (const auto* p = PayloadAs<ScalarFloatPayload>(dto.payload.get())) {
    result.values[0] = p->value;
    return result;
  }
  if (const auto* p = PayloadAs<Cat02WhiteBalancePayload>(dto.payload.get())) {
    result.values[0] = p->enabled ? 1.0f : 0.0f;
    result.values[1] = p->temperature_offset;
    result.values[2] = p->tint_offset;
    return result;
  }
  if (const auto* p = PayloadAs<CurvePayload>(dto.payload.get())) {
    result.count = static_cast<std::uint32_t>(std::min(p->points.size(), kMaxCurvePoints));
    for (std::uint32_t i = 0; i < result.count; ++i) {
      result.values[i * 2]     = p->points[i].x;
      result.values[i * 2 + 1] = p->points[i].y;
    }
    return result;
  }
  if (const auto* p = PayloadAs<HlsPayload>(dto.payload.get())) {
    for (int i = 0; i < kHlsHueBinCount; ++i) {
      result.values[i]      = p->hls_adj_table[static_cast<std::size_t>(i)].h;
      result.values[8 + i]  = p->hls_adj_table[static_cast<std::size_t>(i)].l;
      result.values[16 + i] = p->hls_adj_table[static_cast<std::size_t>(i)].s;
    }
    return result;
  }
  if (const auto* p = PayloadAs<ColorWheelPayload>(dto.payload.get())) {
    const ColorWheelControl controls[3] = {p->lift, p->gamma, p->gain};
    for (int i = 0; i < 3; ++i) {
      result.values[i * 4]     = controls[i].color_offset.x;
      result.values[i * 4 + 1] = controls[i].color_offset.y;
      result.values[i * 4 + 2] = controls[i].color_offset.z;
      result.values[i * 4 + 3] = controls[i].luminance_offset;
    }
    return result;
  }
  if (const auto* p = PayloadAs<SharpenPayload>(dto.payload.get())) {
    result.values[0] = p->amount;
    result.values[1] = p->radius;
    result.values[2] = p->threshold;
    return result;
  }
  if (const auto* p = PayloadAs<LmtPayload>(dto.payload.get())) {
    result.values[0] = p->cube_path.empty() ? 0.0f : 1.0f;
    return result;
  }
  throw std::runtime_error("Primary grade: Model DTO is not supported");
}

auto MakeGradeNeighborParams(const IOperatorModel& model, AdjustmentBehavior behavior,
                             const ResolvedRenderGeometry& geometry) -> GradeNeighborParams {
  if (!IsNeighborhoodBehavior(behavior)) {
    throw std::runtime_error("Primary grade: adjustment is not a neighborhood operator");
  }

  GradeNeighborParams result;
  result.behavior = static_cast<std::uint32_t>(behavior);
  CopyRenderMapping(geometry, result);
  const auto dto = model.MakeFullDto();

  const float render_scale = NeighborhoodRenderScale(geometry);

  if (behavior == AdjustmentBehavior::Sharpen) {
    const auto* sharpen = PayloadAs<SharpenPayload>(dto.payload.get());
    if (sharpen == nullptr) {
      throw std::runtime_error("Primary grade: Sharpen payload is invalid");
    }
    result.amount    = std::clamp(sharpen->amount / 100.0f, 0.0f, 1.0f);
    result.threshold = std::clamp(sharpen->threshold, 0.0f, 1.0f);
    result.enabled   = result.amount > 0.0f ? 1U : 0U;
    result.sigma_x   = sharpen->radius * render_scale;
    result.sigma_y   = result.sigma_x;
    BuildGaussianWeights(result.sigma_x, kSharpenMaxRadius, result);
    return result;
  }

  const auto* scalar = PayloadAs<ScalarFloatPayload>(dto.payload.get());
  if (scalar == nullptr) {
    throw std::runtime_error("Primary grade: neighborhood scalar payload is invalid");
  }

  if (behavior == AdjustmentBehavior::Clarity) {
    result.amount  = std::clamp(scalar->value / 100.0f, -1.0f, 1.0f);
    result.enabled = result.amount != 0.0f ? 1U : 0U;
    result.sigma_x = kClarityNeighborhoodSigma * render_scale;
    result.sigma_y = result.sigma_x;
    BuildGaussianWeights(result.sigma_x, kClarityMaxRadius, result);
    return result;
  }

  if (behavior == AdjustmentBehavior::Halation) {
    result.amount  = std::clamp(scalar->value, 0.0f, 1.0f) * kHalationStrengthScale;
    result.enabled = result.amount > 0.0f ? 1U : 0U;
    result.sigma_x = kHalationSigma * RenderAxisScale(geometry, true);
    result.sigma_y = kHalationSigma * RenderAxisScale(geometry, false);
    std::copy_n(kHalationRedshift, 3, result.redshift);
    return result;
  }

  result.amount  = std::clamp(scalar->value, 0.0f, 1.0f) / 3.0f;
  result.enabled = result.amount > 0.0f ? 1U : 0U;
  result.sigma_x = kFilmGrainDyeCloudSigma * render_scale;
  result.sigma_y = result.sigma_x;
  result.seed_lo = static_cast<std::uint32_t>(kFilmGrainSeed & 0xffffffffULL);
  result.seed_hi = static_cast<std::uint32_t>((kFilmGrainSeed >> 32U) & 0xffffffffULL);
  BuildGaussianWeights(result.sigma_x, kFilmGrainMaxRadius, result);
  return result;
}

}  // namespace alcedo
