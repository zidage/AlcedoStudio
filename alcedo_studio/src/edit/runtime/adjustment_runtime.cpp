//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/runtime/adjustment_runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

#include "edit/geometry/resolved_render_geometry.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/color_wheel_tone.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/i_operator_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "edit/operators/models/tone_curve_spline.hpp"

namespace alcedo {
namespace {

constexpr std::size_t   kCurvePointCapacity       = tone_curve::kMaxControlPoints;
constexpr float         kHalationSigma            = 7.0f;
constexpr float         kHalationStrengthScale    = 2.0f;
constexpr float         kHalationRedshift[3]      = {1.0f, 0.05f, 0.02f};
constexpr float         kClarityNeighborhoodSigma = 15.0f;
constexpr std::uint32_t kSharpenMaxRadius         = 15U;
constexpr std::uint32_t kClarityMaxRadius         = 60U;
static_assert(3 * kCurvePointCapacity * sizeof(float) == sizeof(GradeAdjustmentParams::values),
              "The Curve slot holds x, y, and slope of every control point");

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

/** @brief Mean of the X/Y render-to-reference scales, clamped like the per-axis helper. */
auto NeighborhoodRenderScale(const ResolvedRenderGeometry& geometry) -> float {
  return 0.5f * (RenderAxisScale(geometry, true) + RenderAxisScale(geometry, false));
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

[[noreturn]] void ThrowModelMismatch() {
  throw std::runtime_error("Primary grade: Model type does not match adjustment behavior");
}

template <class Model>
auto RequireModel(const IOperatorModel& model) -> const Model& {
  const auto* typed = dynamic_cast<const Model*>(&model);
  if (typed == nullptr) {
    ThrowModelMismatch();
  }
  return *typed;
}

template <class Model>
void PackScalarValue(const IOperatorModel& model, GradeAdjustmentParams& result) {
  result.values[0] = RequireModel<Model>(model).Read(
      [](const ScalarFloatPayload& payload) { return payload.value; });
}

auto MakeGradeRuntimeParams(const IOperatorModel& model, AdjustmentBehavior behavior)
    -> GradeAdjustmentParams {
  const auto resolved = TryResolveAdjustmentBehavior(model.Type());
  if (!resolved.has_value() || *resolved != behavior) {
    ThrowModelMismatch();
  }

  GradeAdjustmentParams result;
  result.behavior = static_cast<std::uint32_t>(behavior);

  switch (behavior) {
    case AdjustmentBehavior::Exposure:
      PackScalarValue<ExposureModel>(model, result);
      return result;
    case AdjustmentBehavior::Contrast:
      PackScalarValue<ContrastModel>(model, result);
      return result;
    case AdjustmentBehavior::White:
      PackScalarValue<WhiteModel>(model, result);
      return result;
    case AdjustmentBehavior::Black:
      PackScalarValue<BlackModel>(model, result);
      return result;
    case AdjustmentBehavior::Shadows:
      PackScalarValue<ShadowsModel>(model, result);
      return result;
    case AdjustmentBehavior::Highlights:
      PackScalarValue<HighlightsModel>(model, result);
      return result;
    case AdjustmentBehavior::Saturation:
      PackScalarValue<SaturationModel>(model, result);
      return result;
    case AdjustmentBehavior::Vibrance:
      PackScalarValue<VibranceModel>(model, result);
      return result;
    case AdjustmentBehavior::Clarity:
      PackScalarValue<ClarityModel>(model, result);
      return result;
    case AdjustmentBehavior::Halation:
      PackScalarValue<HalationModel>(model, result);
      return result;
    case AdjustmentBehavior::FilmGrain:
      PackScalarValue<FilmGrainModel>(model, result);
      return result;
    case AdjustmentBehavior::Cat02WhiteBalance:
      return RequireModel<Cat02WhiteBalanceModel>(model).Read(
          [behavior](const Cat02WhiteBalancePayload& payload) {
            GradeAdjustmentParams packed;
            packed.behavior = static_cast<std::uint32_t>(behavior);
            // values[0] != 0 selects the kernel branch; the AP1 white default is identity.
            const bool identity = payload.temperature == kCat02DefaultTemperature &&
                                  payload.tint == kCat02DefaultTint;
            if (!payload.enabled || identity) {
              return packed;
            }
            const auto matrix = BuildAp1Cat02WhiteBalanceMatrix(payload.temperature, payload.tint);
            packed.values[0]  = 1.0f;
            std::copy(matrix.begin(), matrix.end(), packed.values + 1);
            return packed;
          });
    case AdjustmentBehavior::Curve:
      throw std::runtime_error(
          "Primary grade: the Curve slot has four records; use MakeCurveGradeRuntimeParams");
    case AdjustmentBehavior::Hls:
      return RequireModel<HlsModel>(model).Read([behavior](const HlsPayload& payload) {
        GradeAdjustmentParams packed;
        packed.behavior = static_cast<std::uint32_t>(behavior);
        packed.count    = kHlsHueBinCount;
        for (int i = 0; i < kHlsHueBinCount; ++i) {
          const auto index             = static_cast<std::size_t>(i);
          packed.values[i]             = payload.hue_bins[index];
          packed.values[8 + i * 3]     = payload.hls_adj_table[index].h;
          packed.values[8 + i * 3 + 1] = payload.hls_adj_table[index].l;
          packed.values[8 + i * 3 + 2] = payload.hls_adj_table[index].s;
          packed.values[32 + i]        = payload.h_range_table[index];
        }
        return packed;
      });
    case AdjustmentBehavior::ColorWheel:
      return RequireModel<ColorWheelModel>(model).Read(
          [behavior](const ColorWheelPayload& payload) {
            // Layout read by ApplyToneWheel in the CUDA, OpenCL, and Metal grade kernels.
            GradeAdjustmentParams packed;
            packed.behavior   = static_cast<std::uint32_t>(behavior);
            const auto params = color_wheel::ResolveToneWheelKernelParams(payload);
            for (std::size_t ch = 0; ch < 3; ++ch) {
              packed.values[ch]     = params.offset_acescc_[ch];
              packed.values[3 + ch] = params.lift_scale_[ch];
              packed.values[6 + ch] = params.gamma_exponent_[ch];
              packed.values[9 + ch] = params.gain_scale_[ch];
            }
            packed.values[12] = params.tone_black_acescc_;
            packed.values[13] = params.tone_range_acescc_;
            return packed;
          });
    case AdjustmentBehavior::Sharpen:
      return RequireModel<SharpenModel>(model).Read([behavior](const SharpenPayload& payload) {
        GradeAdjustmentParams packed;
        packed.behavior  = static_cast<std::uint32_t>(behavior);
        packed.values[0] = payload.amount;
        packed.values[1] = payload.radius;
        packed.values[2] = payload.threshold;
        return packed;
      });
    case AdjustmentBehavior::Lmt:
      return RequireModel<LmtModel>(model).Read([behavior](const LmtPayload& payload) {
        // values[0]: configured LUT strength, 0 when no LUT is referenced. Whether the
        // resource is available is expressed by the bound LUT: a missing file binds an
        // empty (edge 0) LUT and the kernels skip the operation without a parameter change.
        GradeAdjustmentParams packed;
        packed.behavior  = static_cast<std::uint32_t>(behavior);
        packed.values[0] = IsEmptyLutReference(payload.reference) ? 0.0f : payload.strength;
        return packed;
      });
    default:
      throw std::runtime_error("Primary grade: adjustment behavior is not supported");
  }
}

auto MakeCurveGradeRuntimeParams(const IOperatorModel& model) -> CurveGradeRuntimeParams {
  return RequireModel<CurveModel>(model).Read([](const CurvePayload& payload) {
    // Layout read by ApplyCurve in the CUDA, OpenCL, and Metal grade kernels: one record for each
    // curve in CurveChannel order, x in values[0..15], y in values[16..31], slope in
    // values[32..47].
    CurveGradeRuntimeParams records;
    for (std::size_t index = 0; index < kCurveChannels.size(); ++index) {
      const auto channel = kCurveChannels[index];
      auto&      packed  = records[index];
      packed.behavior    = static_cast<std::uint32_t>(AdjustmentBehavior::Curve);
      const auto& points = CurveChannelPoints(payload, channel);
      if (channel != CurveChannel::Rgb && IsIdentityCurve(points)) {
        continue;  // count 0: the kernel leaves the channel unchanged.
      }
      const auto spline = tone_curve::SolveToneCurveSpline(points);
      packed.count      = spline.count;
      std::copy(spline.x.begin(), spline.x.end(), packed.values);
      std::copy(spline.y.begin(), spline.y.end(), packed.values + kCurvePointCapacity);
      std::copy(spline.slope.begin(), spline.slope.end(), packed.values + 2 * kCurvePointCapacity);
    }
    return records;
  });
}

auto MakeGradeNeighborParams(const IOperatorModel& model, AdjustmentBehavior behavior,
                             const ResolvedRenderGeometry& geometry) -> GradeNeighborParams {
  if (!IsNeighborhoodBehavior(behavior)) {
    throw std::runtime_error("Primary grade: adjustment is not a neighborhood operator");
  }
  const auto resolved = TryResolveAdjustmentBehavior(model.Type());
  if (!resolved.has_value() || *resolved != behavior) {
    ThrowModelMismatch();
  }

  GradeNeighborParams result;
  result.behavior = static_cast<std::uint32_t>(behavior);
  const float render_scale = NeighborhoodRenderScale(geometry);

  if (behavior == AdjustmentBehavior::Sharpen) {
    RequireModel<SharpenModel>(model).Read([&](const SharpenPayload& sharpen) {
      result.amount    = std::clamp(sharpen.amount / 100.0f, 0.0f, 1.0f);
      result.threshold = std::clamp(sharpen.threshold, 0.0f, 1.0f);
      result.enabled   = result.amount > 0.0f ? 1U : 0U;
      result.sigma_x   = sharpen.radius * render_scale;
      result.sigma_y   = result.sigma_x;
    });
    BuildGaussianWeights(result.sigma_x, kSharpenMaxRadius, result);
    return result;
  }

  const float scalar = [&]() {
    switch (behavior) {
      case AdjustmentBehavior::Clarity:
        return RequireModel<ClarityModel>(model).Read(
            [](const ScalarFloatPayload& payload) { return payload.value; });
      case AdjustmentBehavior::Halation:
        return RequireModel<HalationModel>(model).Read(
            [](const ScalarFloatPayload& payload) { return payload.value; });
      case AdjustmentBehavior::FilmGrain:
        return RequireModel<FilmGrainModel>(model).Read(
            [](const ScalarFloatPayload& payload) { return payload.value; });
      default:
        throw std::runtime_error("Primary grade: neighborhood scalar payload is invalid");
    }
  }();

  if (behavior == AdjustmentBehavior::Clarity) {
    result.amount  = std::clamp(scalar / 100.0f, -1.0f, 1.0f);
    result.enabled = result.amount != 0.0f ? 1U : 0U;
    result.sigma_x = kClarityNeighborhoodSigma * render_scale;
    result.sigma_y = result.sigma_x;
    BuildGaussianWeights(result.sigma_x, kClarityMaxRadius, result);
    return result;
  }

  if (behavior == AdjustmentBehavior::Halation) {
    result.amount  = std::clamp(scalar, 0.0f, 1.0f) * kHalationStrengthScale;
    result.enabled = result.amount > 0.0f ? 1U : 0U;
    result.sigma_x = kHalationSigma * RenderAxisScale(geometry, true);
    result.sigma_y = kHalationSigma * RenderAxisScale(geometry, false);
    std::copy_n(kHalationRedshift, 3, result.redshift);
    return result;
  }

  // Film Grain carries only the strength here, for the DRT/Post schedule. Its size and its kernel
  // come from the grain plan (drt_post_film_grain.hpp, film_grain_plan.hpp).
  result.amount  = std::clamp(scalar, FilmGrainTraits::kMin, FilmGrainTraits::kMax);
  result.enabled = result.amount > 0.0f ? 1U : 0U;
  return result;
}

auto NeighborhoodVerticalRadius(const GradeNeighborParams& params) -> std::uint32_t {
  const auto behavior = static_cast<AdjustmentBehavior>(params.behavior);
  if (behavior == AdjustmentBehavior::Halation) {
    return std::clamp(static_cast<std::uint32_t>(std::ceil(params.sigma_y * 3.0f)), 1U,
                      kGradeNeighborMaxTapCount - 1U);
  }
  return params.radius;
}

}  // namespace alcedo
