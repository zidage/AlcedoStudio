//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>

#include "edit/operators/models/color_wheel_model.hpp"

// Qt-free tone math of the Offset / Lift / Gamma / Gain color wheels. The wheels operate on the
// ACEScc grade signal through a normalized tone coordinate t: t = 0 at kToneBlackEv and t = 1 at
// kToneWhiteEv, in stops relative to middle grey. Every wheel value is a per-channel change in
// stops at the target tone of that wheel:
//   Offset: all tones.               ev' = ev + K
//   Lift:   t <= 0, none at t >= 1.  t' = t + (K / R) * clamp(1 - t, 0, 1)
//   Gamma:  t = 0.5 (middle grey).   t' = t^p, p = ln(0.5 + K / R) / ln(0.5), only for 0 < t < 1
//   Gain:   t = 1, none at t <= 0.   t' = t + (K / R) * max(t, 0)
// R is the tone range in stops. The operations run in the order Offset, Lift, Gamma, Gain.
namespace alcedo::color_wheel {

// kDiscMaxStops and kMasterMaxStops are in color_wheel_model.hpp.
constexpr float kToneBlackEv = -6.0f;
constexpr float kToneWhiteEv = 6.0f;
constexpr float kEpsilon     = 1e-6f;

/// The disc point (@p x, @p y) moved onto the unit circle when it is outside. A point that is not
/// finite is the center.
auto            ClampDiscPoint(float x, float y) -> std::array<float, 2>;

/// Per-channel stops of the disc point (@p x, @p y). The result has zero mean and the length
/// @p strength * radius. The hue of the disc angle sets the direction.
auto            DiscToChannelStops(float x, float y, float strength) -> std::array<float, 3>;

/// The disc point whose DiscToChannelStops at @p strength is the zero-mean part of @p stops. A
/// point outside the unit disc is moved onto the unit circle.
auto ChannelStopsToDisc(const std::array<float, 3>& stops, float strength) -> std::array<float, 2>;

/// Per-channel wheel value in stops: the disc stops plus the Master value.
auto ControlChannelStops(const ColorWheelControl& control) -> std::array<float, 3>;

/// GPU parameters of the four wheels, resolved per channel from the Model values.
struct ToneWheelKernelParams {
  /// Offset as an ACEScc increment.
  std::array<float, 3> offset_acescc_{};
  /// Lift K / R.
  std::array<float, 3> lift_scale_{};
  /// Gamma exponent p.
  std::array<float, 3> gamma_exponent_{1.0f, 1.0f, 1.0f};
  /// Gain K / R.
  std::array<float, 3> gain_scale_{};
  /// ACEScc value at t = 0.
  float                tone_black_acescc_ = 0.0f;
  /// ACEScc width of the tone range (t = 0 to t = 1).
  float                tone_range_acescc_ = 1.0f;
};

auto ResolveToneWheelKernelParams(const ColorWheelPayload& payload) -> ToneWheelKernelParams;

/// CPU reference of the GPU color wheel operation on one ACEScc value of channel @p channel.
auto ApplyToneWheels(float acescc, int channel, const ToneWheelKernelParams& params) -> float;

/// Color wheel values of a project saved before the tone wheels (persisted format version 1).
/// Version 1 applied out = ((c + lift)^gamma) * gain directly to ACEScc; the neutral value is 0
/// for lift and 1 for gamma and gain. Only color_offset and luminance_offset set the rendering.
struct LegacyColorWheelValues {
  ColorWheelControl lift_{};
  ColorWheelControl gamma_{Vec2f{}, 0.0f, Vec3f{1.0f, 1.0f, 1.0f}, 0.0f};
  ColorWheelControl gain_{Vec2f{}, 0.0f, Vec3f{1.0f, 1.0f, 1.0f}, 0.0f};
};

/// Converts version 1 values to tone wheel values. The version 1 lift becomes the Offset exactly.
/// The version 1 gamma and gain become Lift / Gamma / Gain values from a least-squares fit of the
/// per-channel change in stops from kToneBlackEv to kToneWhiteEv.
auto ConvertLegacyColorWheel(const LegacyColorWheelValues& legacy) -> ColorWheelPayload;

}  // namespace alcedo::color_wheel
