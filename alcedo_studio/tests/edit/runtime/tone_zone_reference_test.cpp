//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "tone_zone_reference.hpp"

#include <gtest/gtest.h>

#include <cmath>

namespace alcedo {
namespace {

namespace ref = tone_zone_reference;

/// Stops from grey after Whites or Blacks of @p value on a neutral at @p x stops.
auto Mapped(float x, float value, bool whites) -> float {
  const float c   = ref::kAcesccMidGrey + x / ref::kAcesccB;
  const auto  out = ref::ApplyAcescc({c, c, c}, value, whites);
  return (out[0] - ref::kAcesccMidGrey) * ref::kAcesccB;
}

TEST(ToneZoneReferenceTest, GreyAndTheOtherSideOfGreyStayExact) {
  for (const float value : {-100.0f, -40.0f, 40.0f, 100.0f}) {
    EXPECT_EQ(Mapped(0.0f, value, true), 0.0f);
    EXPECT_EQ(Mapped(0.0f, value, false), 0.0f);
    for (const float x : {-8.0f, -3.0f, -0.5f}) {
      EXPECT_FLOAT_EQ(Mapped(x, value, true), x) << "Whites " << value << " at " << x;
    }
    for (const float x : {0.5f, 3.0f, 8.0f}) {
      EXPECT_FLOAT_EQ(Mapped(x, value, false), x) << "Blacks " << value << " at " << x;
    }
  }
}

TEST(ToneZoneReferenceTest, FullSliderShiftsTheFarTonesByThreeStops) {
  EXPECT_NEAR(Mapped(3.0f, 100.0f, true), 6.0f, 1.0e-5f);
  EXPECT_NEAR(Mapped(8.0f, 100.0f, true), 11.0f, 1.0e-5f);
  EXPECT_NEAR(Mapped(8.0f, -100.0f, true), 5.0f, 1.0e-5f);
  EXPECT_NEAR(Mapped(-3.0f, -100.0f, false), -6.0f, 1.0e-5f);
  EXPECT_NEAR(Mapped(-8.0f, 100.0f, false), -5.0f, 1.0e-5f);
  EXPECT_NEAR(Mapped(-8.0f, -100.0f, false), -11.0f, 1.0e-5f);
}

TEST(ToneZoneReferenceTest, CurvesAreMonotonicAndBoundedByTheSliderShift) {
  for (const bool whites : {true, false}) {
    for (const float value : {-100.0f, -60.0f, -10.0f, 10.0f, 60.0f, 100.0f}) {
      const float limit    = std::fabs(value) * ref::kEvPerUnit;
      float       previous = Mapped(-12.0f, value, whites);
      for (float x = -11.95f; x <= 12.0f; x += 0.05f) {
        const float y = Mapped(x, value, whites);
        EXPECT_GT(y, previous) << (whites ? "Whites " : "Blacks ") << value << " at " << x;
        EXPECT_LE(std::fabs(y - x), limit + 1.0e-4f);
        previous = y;
      }
    }
  }
}

TEST(ToneZoneReferenceTest, OppositeValuesUndoEachOther) {
  for (const bool whites : {true, false}) {
    for (const float value : {25.0f, 100.0f}) {
      for (float x = -9.0f; x <= 9.0f; x += 0.25f) {
        const float there = Mapped(x, value, whites);
        EXPECT_NEAR(Mapped(there, -value, whites), x, 1.0e-4f)
            << (whites ? "Whites " : "Blacks ") << value << " at " << x;
      }
    }
  }
}

TEST(ToneZoneReferenceTest, EveryChannelMovesByTheSameAcesccOffset) {
  const auto out = ref::ApplyAcescc({0.62f, 0.55f, 0.40f}, 70.0f, true);
  EXPECT_NEAR(out[0] - 0.62f, out[1] - 0.55f, 1.0e-6f);
  EXPECT_NEAR(out[0] - 0.62f, out[2] - 0.40f, 1.0e-6f);
  EXPECT_GT(out[0], 0.62f);
}

}  // namespace
}  // namespace alcedo
