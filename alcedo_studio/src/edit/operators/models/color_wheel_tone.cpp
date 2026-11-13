//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/color_wheel_tone.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

#include "color/color_encoding_math.h"

namespace alcedo::color_wheel {
namespace {

constexpr double kToneRangeEv     = static_cast<double>(kToneWhiteEv - kToneBlackEv);
/// The smallest 0.5 + K / R in the Gamma exponent, so that p stays finite and positive.
constexpr double kMinGammaBase    = 0.05;
constexpr int    kFitSampleCount  = 25;
constexpr int    kFitMaxIteration = 20;

auto             HueToUnitRgb(float h) -> std::array<float, 3> {
  h                  = h - std::floor(h);
  const float scaled = h * 6.0f;
  const int   sector = static_cast<int>(std::floor(scaled)) % 6;
  const float f      = scaled - std::floor(scaled);
  switch (sector) {
    case 0:
      return {1.0f, f, 0.0f};
    case 1:
      return {1.0f - f, 1.0f, 0.0f};
    case 2:
      return {0.0f, 1.0f, f};
    case 3:
      return {0.0f, 1.0f - f, 1.0f};
    case 4:
      return {f, 0.0f, 1.0f};
    case 5:
    default:
      return {1.0f, 0.0f, 1.0f - f};
  }
}

/// HSV hue in [0, 1) of @p rgb. The hue does not change when a constant is added to every channel
/// or when the channels are multiplied by a positive value.
auto RgbHue(const std::array<double, 3>& rgb) -> double {
  const double maximum = std::max({rgb[0], rgb[1], rgb[2]});
  const double minimum = std::min({rgb[0], rgb[1], rgb[2]});
  const double chroma  = maximum - minimum;
  if (chroma <= 0.0) {
    return 0.0;
  }
  double sector = 0.0;
  if (maximum == rgb[0]) {
    sector = std::fmod((rgb[1] - rgb[2]) / chroma + 6.0, 6.0);
  } else if (maximum == rgb[1]) {
    sector = (rgb[2] - rgb[0]) / chroma + 2.0;
  } else {
    sector = (rgb[0] - rgb[1]) / chroma + 4.0;
  }
  return sector / 6.0;
}

auto GammaExponent(double stops) -> double {
  const double base = std::max(0.5 + stops / kToneRangeEv, kMinGammaBase);
  return std::log(base) / std::log(0.5);
}

/// Lift, Gamma, and Gain on one ACEScc value. Offset is applied before this function.
template <typename T>
auto ApplyLiftGammaGain(T acescc, T black, T range, T lift_scale, T gamma_exponent, T gain_scale)
    -> T {
  T t = (acescc - black) / range;
  t += lift_scale * std::clamp(T{1} - t, T{0}, T{1});
  if (t > T{0} && t < T{1}) {
    t = std::pow(t, gamma_exponent);
  }
  t += gain_scale * std::max(t, T{0});
  return black + t * range;
}

auto WheelControlFromStops(const std::array<double, 3>& stops) -> ColorWheelControl {
  const double      master = (stops[0] + stops[1] + stops[2]) / 3.0;
  ColorWheelControl control;
  control.strength         = kDiscMaxStops;
  control.luminance_offset = static_cast<float>(master);
  control.color_offset =
      Vec3f{static_cast<float>(stops[0] - master), static_cast<float>(stops[1] - master),
            static_cast<float>(stops[2] - master)};
  const auto disc = ChannelStopsToDisc(
      {control.color_offset.x, control.color_offset.y, control.color_offset.z}, control.strength);
  control.disc = Vec2f{disc[0], disc[1]};
  return control;
}

/// Solves the 3x3 system @p a * x = @p b with Gaussian elimination and partial pivoting.
auto Solve3(std::array<std::array<double, 3>, 3> a, std::array<double, 3> b)
    -> std::array<double, 3> {
  for (int col = 0; col < 3; ++col) {
    int pivot = col;
    for (int row = col + 1; row < 3; ++row) {
      if (std::abs(a[row][col]) > std::abs(a[pivot][col])) {
        pivot = row;
      }
    }
    std::swap(a[col], a[pivot]);
    std::swap(b[col], b[pivot]);
    if (std::abs(a[col][col]) < 1e-15) {
      return {0.0, 0.0, 0.0};
    }
    for (int row = col + 1; row < 3; ++row) {
      const double factor = a[row][col] / a[col][col];
      for (int k = col; k < 3; ++k) {
        a[row][k] -= factor * a[col][k];
      }
      b[row] -= factor * b[col];
    }
  }
  std::array<double, 3> x{};
  for (int row = 2; row >= 0; --row) {
    double sum = b[row];
    for (int k = row + 1; k < 3; ++k) {
      sum -= a[row][k] * x[k];
    }
    x[row] = sum / a[row][row];
  }
  return x;
}

/// Lift, Gamma, and Gain stops that best match the version 1 operation
/// c' = sign(c) * |c + lift|^gamma * gain on one channel, in stops, for inputs c from kToneBlackEv
/// to kToneWhiteEv. The version 1 lift is the Offset, which the tone wheels apply first.
auto FitLegacyGammaGain(double legacy_lift, double legacy_gamma, double legacy_gain)
    -> std::array<double, 3> {
  const double                        grey  = static_cast<double>(CE_ACESCC_MIDDLE_GREY);
  const double                        b     = static_cast<double>(CE_ACESCC_B);
  const double                        black = grey + static_cast<double>(kToneBlackEv) / b;
  const double                        range = kToneRangeEv / b;

  std::array<double, kFitSampleCount> input{};
  std::array<double, kFitSampleCount> target{};
  for (int i = 0; i < kFitSampleCount; ++i) {
    const double ev = static_cast<double>(kToneBlackEv) + kToneRangeEv * i / (kFitSampleCount - 1);
    const double c  = grey + ev / b;
    input[i]        = c + legacy_lift;
    target[i]       = std::copysign(std::pow(std::abs(input[i]), legacy_gamma), c) * legacy_gain;
  }
  // Residuals in stops of the Lift / Gamma / Gain stops @p k.
  const auto residuals = [&](const std::array<double, 3>& k) {
    std::array<double, kFitSampleCount> r{};
    for (int i = 0; i < kFitSampleCount; ++i) {
      const double out = ApplyLiftGammaGain(input[i], black, range, k[0] / kToneRangeEv,
                                            GammaExponent(k[1]), k[2] / kToneRangeEv);
      r[i]             = (out - target[i]) * b;
    }
    return r;
  };

  std::array<double, 3> k{};
  for (int iteration = 0; iteration < kFitMaxIteration; ++iteration) {
    const auto                                         r = residuals(k);
    std::array<std::array<double, kFitSampleCount>, 3> jacobian{};
    constexpr double                                   kStep = 1e-4;
    for (int p = 0; p < 3; ++p) {
      auto shifted = k;
      shifted[p] += kStep;
      const auto rs = residuals(shifted);
      for (int i = 0; i < kFitSampleCount; ++i) {
        jacobian[p][i] = (rs[i] - r[i]) / kStep;
      }
    }
    std::array<std::array<double, 3>, 3> normal{};
    std::array<double, 3>                gradient{};
    for (int p = 0; p < 3; ++p) {
      for (int q = 0; q < 3; ++q) {
        for (int i = 0; i < kFitSampleCount; ++i) {
          normal[p][q] += jacobian[p][i] * jacobian[q][i];
        }
      }
      for (int i = 0; i < kFitSampleCount; ++i) {
        gradient[p] -= jacobian[p][i] * r[i];
      }
    }
    const auto delta = Solve3(normal, gradient);
    k[0] += delta[0];
    k[1] += delta[1];
    k[2] += delta[2];
    if (std::abs(delta[0]) + std::abs(delta[1]) + std::abs(delta[2]) < 1e-9) {
      break;
    }
  }
  return k;
}

}  // namespace

auto ClampDiscPoint(float x, float y) -> std::array<float, 2> {
  if (!std::isfinite(x) || !std::isfinite(y)) {
    return {0.0f, 0.0f};
  }
  const float r = std::sqrt(x * x + y * y);
  if (r <= 1.0f || r <= kEpsilon) {
    return {x, y};
  }
  const float inv_r = 1.0f / r;
  return {x * inv_r, y * inv_r};
}

auto DiscToChannelStops(float x, float y, float strength) -> std::array<float, 3> {
  const auto  p = ClampDiscPoint(x, y);
  const float r = std::clamp(std::sqrt(p[0] * p[0] + p[1] * p[1]), 0.0f, 1.0f);
  float       h = std::atan2(p[1], p[0]) / (2.0f * std::numbers::pi_v<float>);
  if (h < 0.0f) {
    h += 1.0f;
  }
  const auto  c   = HueToUnitRgb(h);
  const float m   = (c[0] + c[1] + c[2]) / 3.0f;
  float       dr  = c[0] - m;
  float       dg  = c[1] - m;
  float       db  = c[2] - m;
  const float len = std::sqrt(dr * dr + dg * dg + db * db);
  const float inv = 1.0f / (len + kEpsilon);
  dr *= inv;
  dg *= inv;
  db *= inv;
  const float scale = std::max(strength, 0.0f) * r;
  return {dr * scale, dg * scale, db * scale};
}

auto ChannelStopsToDisc(const std::array<float, 3>& stops, float strength) -> std::array<float, 2> {
  const double                mean = (static_cast<double>(stops[0]) + stops[1] + stops[2]) / 3.0;
  const std::array<double, 3> v    = {stops[0] - mean, stops[1] - mean, stops[2] - mean};
  const double                len  = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len <= static_cast<double>(kEpsilon) || strength <= kEpsilon) {
    return {0.0f, 0.0f};
  }
  const double r     = std::min(len / static_cast<double>(strength), 1.0);
  const double angle = RgbHue(v) * 2.0 * std::numbers::pi;
  return {static_cast<float>(r * std::cos(angle)), static_cast<float>(r * std::sin(angle))};
}

auto ControlChannelStops(const ColorWheelControl& control) -> std::array<float, 3> {
  return {control.color_offset.x + control.luminance_offset,
          control.color_offset.y + control.luminance_offset,
          control.color_offset.z + control.luminance_offset};
}

auto ResolveToneWheelKernelParams(const ColorWheelPayload& payload) -> ToneWheelKernelParams {
  ToneWheelKernelParams params;
  const auto            offset = ControlChannelStops(payload.offset);
  const auto            lift   = ControlChannelStops(payload.lift);
  const auto            gamma  = ControlChannelStops(payload.gamma);
  const auto            gain   = ControlChannelStops(payload.gain);
  for (std::size_t ch = 0; ch < 3; ++ch) {
    params.offset_acescc_[ch]  = offset[ch] / CE_ACESCC_B;
    params.lift_scale_[ch]     = lift[ch] / static_cast<float>(kToneRangeEv);
    params.gamma_exponent_[ch] = static_cast<float>(GammaExponent(gamma[ch]));
    params.gain_scale_[ch]     = gain[ch] / static_cast<float>(kToneRangeEv);
  }
  params.tone_black_acescc_ = CE_ACESCC_MIDDLE_GREY + kToneBlackEv / CE_ACESCC_B;
  params.tone_range_acescc_ = static_cast<float>(kToneRangeEv) / CE_ACESCC_B;
  return params;
}

auto ApplyToneWheels(float acescc, int channel, const ToneWheelKernelParams& params) -> float {
  const auto ch = static_cast<std::size_t>(channel);
  // Neutral Lift, Gamma, and Gain leave the value exact (only the Offset applies), as on the GPU.
  if (params.lift_scale_[ch] == 0.0f && params.gamma_exponent_[ch] == 1.0f &&
      params.gain_scale_[ch] == 0.0f) {
    return acescc + params.offset_acescc_[ch];
  }
  return ApplyLiftGammaGain(acescc + params.offset_acescc_[ch], params.tone_black_acescc_,
                            params.tone_range_acescc_, params.lift_scale_[ch],
                            params.gamma_exponent_[ch], params.gain_scale_[ch]);
}

auto ConvertLegacyColorWheel(const LegacyColorWheelValues& legacy) -> ColorWheelPayload {
  const auto            lift  = ControlChannelStops(legacy.lift_);
  const auto            gamma = ControlChannelStops(legacy.gamma_);
  const auto            gain  = ControlChannelStops(legacy.gain_);

  std::array<double, 3> offset_stops{};
  std::array<double, 3> lift_stops{};
  std::array<double, 3> gamma_stops{};
  std::array<double, 3> gain_stops{};
  for (std::size_t ch = 0; ch < 3; ++ch) {
    // A version 1 lift adds to ACEScc: the same change in stops for every tone.
    const double legacy_lift  = static_cast<double>(lift[ch]);
    const double legacy_gamma = std::max(static_cast<double>(gamma[ch]), 1e-4);
    const double legacy_gain  = static_cast<double>(gain[ch]);
    offset_stops[ch]          = legacy_lift * static_cast<double>(CE_ACESCC_B);
    if (legacy_gamma == 1.0 && legacy_gain == 1.0) {
      continue;
    }
    const auto fit  = FitLegacyGammaGain(legacy_lift, legacy_gamma, legacy_gain);
    lift_stops[ch]  = fit[0];
    gamma_stops[ch] = fit[1];
    gain_stops[ch]  = fit[2];
  }

  ColorWheelPayload payload;
  payload.offset = WheelControlFromStops(offset_stops);
  payload.lift   = WheelControlFromStops(lift_stops);
  payload.gamma  = WheelControlFromStops(gamma_stops);
  payload.gain   = WheelControlFromStops(gain_stops);
  return payload;
}

}  // namespace alcedo::color_wheel
