//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/color_wheel_tone.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <stdexcept>

#include "edit/operators/models/color_wheel_model.hpp"
#include "json.hpp"

namespace alcedo {
namespace {

constexpr float kAcesccB    = 17.52f;
constexpr float kAcesccGrey = 0.41358840f;

auto            AcesccOfEv(float ev) -> float { return kAcesccGrey + ev / kAcesccB; }

/// Change in stops of channel 0 at @p ev after the wheels of @p payload.
auto            DeltaEv(const ColorWheelPayload& payload, float ev) -> float {
  const auto  params = color_wheel::ResolveToneWheelKernelParams(payload);
  const float in     = AcesccOfEv(ev);
  return (color_wheel::ApplyToneWheels(in, 0, params) - in) * kAcesccB;
}

auto PayloadOf(const ColorWheelModel& model) -> ColorWheelPayload {
  ColorWheelPayload payload;
  payload.lift   = model.Lift();
  payload.gamma  = model.Gamma();
  payload.gain   = model.Gain();
  payload.offset = model.Offset();
  return payload;
}

auto LegacyControlJson(std::array<float, 3> color_offset, float luminance_offset)
    -> nlohmann::json {
  return {
      {"disc", {{"x", 0.0}, {"y", 0.0}}},
      {"strength", 0.10},
      {"color_offset", {{"x", color_offset[0]}, {"y", color_offset[1]}, {"z", color_offset[2]}}},
      {"luminance_offset", luminance_offset}};
}

/// Version 1 operation c' = sign(c) * |c + lift|^gamma * gain on one ACEScc value.
auto LegacyApply(float c, float lift, float gamma, float gain) -> float {
  return std::copysign(std::pow(std::fabs(c + lift), std::max(gamma, 1.0e-4f)), c) * gain;
}

TEST(ColorWheelTone, OneStopOnEachWheelChangesItsTargetToneByOneStop) {
  constexpr float   kTolerance = 1.0e-4f;
  ColorWheelPayload offset;
  offset.offset.luminance_offset = 1.0f;
  for (const float ev : {-8.0f, -6.0f, -2.0f, 0.0f, 2.0f, 6.0f, 8.0f}) {
    EXPECT_NEAR(DeltaEv(offset, ev), 1.0f, kTolerance) << ev;
  }

  ColorWheelPayload lift;
  lift.lift.luminance_offset = 1.0f;
  EXPECT_NEAR(DeltaEv(lift, -8.0f), 1.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(lift, -6.0f), 1.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(lift, 0.0f), 0.5f, kTolerance);
  EXPECT_NEAR(DeltaEv(lift, 6.0f), 0.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(lift, 8.0f), 0.0f, kTolerance);

  ColorWheelPayload gamma;
  gamma.gamma.luminance_offset = 1.0f;
  EXPECT_NEAR(DeltaEv(gamma, -8.0f), 0.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(gamma, -6.0f), 0.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(gamma, 0.0f), 1.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(gamma, 6.0f), 0.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(gamma, 8.0f), 0.0f, kTolerance);

  ColorWheelPayload gain;
  gain.gain.luminance_offset = 1.0f;
  EXPECT_NEAR(DeltaEv(gain, -8.0f), 0.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(gain, -6.0f), 0.0f, kTolerance);
  EXPECT_NEAR(DeltaEv(gain, 0.0f), 0.5f, kTolerance);
  EXPECT_NEAR(DeltaEv(gain, 6.0f), 1.0f, kTolerance);
}

TEST(ColorWheelTone, LiftAffectsShadowsMoreThanGainAndGainAffectsHighlightsMoreThanLift) {
  ColorWheelPayload lift;
  lift.lift.luminance_offset = 1.0f;
  ColorWheelPayload gain;
  gain.gain.luminance_offset = 1.0f;
  EXPECT_GT(DeltaEv(lift, -4.0f), 4.0f * DeltaEv(gain, -4.0f));
  EXPECT_GT(DeltaEv(gain, 4.0f), 4.0f * DeltaEv(lift, 4.0f));
}

TEST(ColorWheelTone, NeutralWheelsLeaveEveryToneUnchanged) {
  const ColorWheelPayload neutral;
  const auto              params = color_wheel::ResolveToneWheelKernelParams(neutral);
  for (int i = -40; i <= 40; ++i) {
    const float in = AcesccOfEv(static_cast<float>(i) * 0.25f);
    for (int ch = 0; ch < 3; ++ch) {
      EXPECT_NEAR(color_wheel::ApplyToneWheels(in, ch, params), in, 1.0e-6f) << i << " " << ch;
    }
  }
}

TEST(ColorWheelTone, DiscStopsHaveZeroMeanAndStrengthTimesRadiusLength) {
  const auto  stops = color_wheel::DiscToChannelStops(0.0f, 0.6f, 1.0f);
  const float mean  = (stops[0] + stops[1] + stops[2]) / 3.0f;
  const float len   = std::sqrt(stops[0] * stops[0] + stops[1] * stops[1] + stops[2] * stops[2]);
  EXPECT_NEAR(mean, 0.0f, 1.0e-6f);
  EXPECT_NEAR(len, 0.6f, 1.0e-5f);
}

TEST(ColorWheelTone, ChannelStopsToDiscInvertsDiscToChannelStops) {
  const std::array<std::array<float, 2>, 6> points = {
      {{0.5f, 0.0f}, {0.0f, 0.5f}, {-0.3f, 0.4f}, {-0.6f, -0.2f}, {0.1f, -0.7f}, {0.9f, 0.3f}}};
  for (const auto& point : points) {
    const auto stops = color_wheel::DiscToChannelStops(point[0], point[1], 1.0f);
    const auto disc  = color_wheel::ChannelStopsToDisc(stops, 1.0f);
    EXPECT_NEAR(disc[0], point[0], 1.0e-4f) << point[0] << ", " << point[1];
    EXPECT_NEAR(disc[1], point[1], 1.0e-4f) << point[0] << ", " << point[1];
  }
}

TEST(ColorWheelTone, ChannelStopsOutsideTheDiscMoveOntoTheUnitCircle) {
  const auto disc = color_wheel::ChannelStopsToDisc({2.0f, -1.0f, -1.0f}, 1.0f);
  EXPECT_NEAR(std::sqrt(disc[0] * disc[0] + disc[1] * disc[1]), 1.0f, 1.0e-5f);
}

TEST(ColorWheelModelJson, SerializedJsonHasFormatVersionTwoAndOffsetAndLoadsBack) {
  ColorWheelModel         model;
  ColorWheelUpdate        update;
  ColorWheelControlUpdate offset;
  offset.disc             = Vec2f{0.2f, 0.1f};
  offset.color_offset     = Vec3f{0.1f, -0.05f, -0.05f};
  offset.luminance_offset = 0.75f;
  update.offset           = offset;
  model.ApplyUpdate(update);

  const auto json = model.ToJson();
  EXPECT_EQ(json.at("version"), 2);
  ASSERT_TRUE(json.contains("offset"));

  ColorWheelModel loaded;
  loaded.LoadJson(json);
  EXPECT_EQ(loaded.Offset(), model.Offset());
  EXPECT_EQ(loaded.Lift(), model.Lift());
  EXPECT_FALSE(loaded.IsDefault());
}

TEST(ColorWheelModelJson, UnsupportedFormatVersionIsRejected) {
  ColorWheelModel model;
  EXPECT_THROW(model.LoadJson({{"version", 3}}), std::invalid_argument);
}

TEST(ColorWheelModelJson, NeutralVersionOneJsonLoadsAsNeutralToneWheels) {
  const nlohmann::json legacy = {{"lift", LegacyControlJson({0.0f, 0.0f, 0.0f}, 0.0f)},
                                 {"gamma", LegacyControlJson({1.0f, 1.0f, 1.0f}, 0.0f)},
                                 {"gain", LegacyControlJson({1.0f, 1.0f, 1.0f}, 0.0f)}};
  ColorWheelModel      model;
  model.LoadJson(legacy);
  EXPECT_TRUE(model.IsDefault());
  // An unedited version 1 Model writes its stored JSON back (root id and history bytes).
  EXPECT_EQ(model.ToJson().dump(), legacy.dump());
}

TEST(ColorWheelModelJson, UneditedVersionOneJsonSerializesToTheSameJson) {
  const nlohmann::json legacy = {{"lift", LegacyControlJson({0.05f, -0.02f, 0.01f}, 0.01f)},
                                 {"gamma", LegacyControlJson({0.95f, 1.0f, 1.0f}, 0.0f)},
                                 {"gain", LegacyControlJson({1.0f, 1.0f, 1.05f}, 0.0f)}};
  ColorWheelModel      model;
  model.LoadJson(legacy);
  EXPECT_EQ(model.ToJson().dump(), legacy.dump());

  // An update that changes no value keeps the version 1 JSON.
  ColorWheelUpdate        same;
  ColorWheelControlUpdate offset;
  offset.luminance_offset = model.Offset().luminance_offset;
  same.offset             = offset;
  model.ApplyUpdate(same);
  EXPECT_EQ(model.ToJson().dump(), legacy.dump());
}

TEST(ColorWheelModelJson, EditOfVersionOneValuesWritesVersionTwoWithTheOtherConvertedWheels) {
  const nlohmann::json legacy = {{"lift", LegacyControlJson({0.05f, -0.02f, 0.01f}, 0.0f)},
                                 {"gamma", LegacyControlJson({1.0f, 1.0f, 1.0f}, 0.0f)},
                                 {"gain", LegacyControlJson({1.0f, 1.0f, 1.0f}, 0.0f)}};
  ColorWheelModel      model;
  model.LoadJson(legacy);
  const auto              converted_offset = model.Offset();

  ColorWheelUpdate        update;
  ColorWheelControlUpdate gain;
  gain.luminance_offset = 0.5f;
  update.gain           = gain;
  model.ApplyUpdate(update);

  const auto json = model.ToJson();
  EXPECT_EQ(json.at("version"), 2);
  EXPECT_EQ(model.Offset(), converted_offset);
  EXPECT_EQ(model.Gain().luminance_offset, 0.5f);
}

TEST(ColorWheelModelJson, VersionOneUpdateReplacesWheelsWithTheConvertedValues) {
  const nlohmann::json legacy = {{"lift", LegacyControlJson({0.05f, 0.0f, 0.0f}, 0.0f)},
                                 {"gamma", LegacyControlJson({1.0f, 1.0f, 1.0f}, 0.0f)},
                                 {"gain", LegacyControlJson({1.0f, 1.0f, 1.0f}, 0.0f)}};
  ColorWheelModel      loaded;
  loaded.LoadJson(legacy);

  ColorWheelModel         model;
  ColorWheelUpdate        edit;
  ColorWheelControlUpdate gamma;
  gamma.luminance_offset = 0.25f;
  edit.gamma             = gamma;
  model.ApplyUpdate(edit);

  ColorWheelUpdate restore;
  restore.version_one_json = legacy;
  model.ApplyUpdate(restore);
  EXPECT_EQ(model.ToJson().dump(), legacy.dump());
  EXPECT_EQ(model.Offset(), loaded.Offset());
  EXPECT_EQ(model.Gamma(), loaded.Gamma());
}

TEST(ColorWheelModelJson, VersionOneLiftLoadsAsTheSameOffsetInStops) {
  const nlohmann::json legacy = {{"lift", LegacyControlJson({0.05f, -0.02f, 0.01f}, 0.01f)},
                                 {"gamma", LegacyControlJson({1.0f, 1.0f, 1.0f}, 0.0f)},
                                 {"gain", LegacyControlJson({1.0f, 1.0f, 1.0f}, 0.0f)}};
  ColorWheelModel      model;
  model.LoadJson(legacy);

  const auto offset = color_wheel::ControlChannelStops(model.Offset());
  EXPECT_NEAR(offset[0], 0.06f * kAcesccB, 1.0e-4f);
  EXPECT_NEAR(offset[1], -0.01f * kAcesccB, 1.0e-4f);
  EXPECT_NEAR(offset[2], 0.02f * kAcesccB, 1.0e-4f);
  for (const auto& control : {model.Lift(), model.Gamma(), model.Gain()}) {
    EXPECT_EQ(control.luminance_offset, 0.0f);
    EXPECT_EQ(control.color_offset, Vec3f{});
  }
}

TEST(ColorWheelModelJson, VersionOneValuesRenderWithinFiveHundredthsOfAStopAfterLoad) {
  constexpr std::array<float, 3> kLift  = {0.02f, -0.01f, 0.0f};
  constexpr std::array<float, 3> kGamma = {0.92f, 1.0f, 1.06f};
  constexpr std::array<float, 3> kGain  = {1.06f, 1.0f, 0.95f};
  const nlohmann::json           legacy = {{"lift", LegacyControlJson(kLift, 0.0f)},
                                           {"gamma", LegacyControlJson(kGamma, 0.0f)},
                                           {"gain", LegacyControlJson(kGain, 0.0f)}};
  ColorWheelModel                model;
  model.LoadJson(legacy);
  const auto params = color_wheel::ResolveToneWheelKernelParams(PayloadOf(model));

  for (int i = 0; i <= 24; ++i) {
    const float in = AcesccOfEv(-6.0f + 0.5f * static_cast<float>(i));
    for (std::size_t ch = 0; ch < 3; ++ch) {
      const float expected = LegacyApply(in, kLift[ch], kGamma[ch], kGain[ch]);
      const float actual   = color_wheel::ApplyToneWheels(in, static_cast<int>(ch), params);
      EXPECT_NEAR(actual * kAcesccB, expected * kAcesccB, 0.05f) << i << " " << ch;
    }
  }
}

}  // namespace
}  // namespace alcedo
