//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/operators/models/curve_model.hpp"

#include <gtest/gtest.h>

#include <vector>

#include "json.hpp"

namespace alcedo {
namespace {

const std::vector<CurvePoint> kRgbCurve{{0.0f, 0.0f}, {0.25f, 0.18f}, {0.6f, 0.7f}, {1.0f, 1.0f}};
const std::vector<CurvePoint> kBlueCurve{{0.0f, 0.05f}, {0.5f, 0.42f}, {1.0f, 0.95f}};

TEST(CurveModelTest, IdentityChannelCurvesEncodeWithoutTheChannelKeys) {
  CurveModel model;
  model.SetPoints(kRgbCurve);
  const auto json = model.ToJson();
  EXPECT_TRUE(json.contains("points"));
  EXPECT_FALSE(json.contains("red"));
  EXPECT_FALSE(json.contains("green"));
  EXPECT_FALSE(json.contains("blue"));
  EXPECT_EQ(json.size(), 1U);
}

TEST(CurveModelTest, PayloadWithBlueCurveDecodesAndEncodesToTheSameBytes) {
  CurveModel  source;
  CurveUpdate update;
  update.points = kRgbCurve;
  update.blue   = kBlueCurve;
  source.ApplyUpdate(update);
  const auto bytes = source.ToJson().dump();
  EXPECT_FALSE(source.ToJson().contains("red"));
  ASSERT_TRUE(source.ToJson().contains("blue"));

  CurveModel decoded;
  decoded.LoadJson(nlohmann::json::parse(bytes));
  EXPECT_EQ(decoded.ToJson().dump(), bytes);
  EXPECT_EQ(decoded.ChannelPoints(CurveChannel::Rgb), kRgbCurve);
  EXPECT_EQ(decoded.ChannelPoints(CurveChannel::Blue), kBlueCurve);
  EXPECT_EQ(decoded.ChannelPoints(CurveChannel::Red), IdentityCurvePoints());
  EXPECT_EQ(decoded.ChannelPoints(CurveChannel::Green), IdentityCurvePoints());
}

TEST(CurveModelTest, LoadJsonWithoutAChannelKeyResetsThatChannelToIdentity) {
  // LoadJson reads the complete state (project load, adjustment transfer): a channel key that the
  // JSON does not have is the identity, also when the Model had another curve there.
  CurveModel  model;
  CurveUpdate update;
  update.red = kBlueCurve;
  model.ApplyUpdate(update);
  ASSERT_FALSE(model.IsDefault());

  model.LoadJson(nlohmann::json::parse(R"({"points":[{"x":0.0,"y":0.0},{"x":1.0,"y":1.0}]})"));
  EXPECT_EQ(model.ChannelPoints(CurveChannel::Red), IdentityCurvePoints());
  EXPECT_TRUE(model.IsDefault());
}

TEST(CurveModelTest, IsDefaultOnlyWhenTheFourCurvesAreTheIdentity) {
  CurveModel model;
  EXPECT_TRUE(model.IsDefault());
  for (const auto channel : kCurveChannels) {
    CurveModel  edited;
    CurveUpdate update;
    update.Channel(channel) = kBlueCurve;
    edited.ApplyUpdate(update);
    EXPECT_FALSE(edited.IsDefault()) << CurveChannelKey(channel);
  }
}

TEST(CurveModelTest, ApplyChangesOnlyTheGivenCurvesInOneRevision) {
  CurveModel  model;
  CurveUpdate first;
  first.points = kRgbCurve;
  model.ApplyUpdate(first);
  const auto  before = model.Revision();

  CurveUpdate blue_only;
  blue_only.blue = kBlueCurve;
  model.ApplyUpdate(blue_only);
  EXPECT_NE(model.Revision(), before);
  EXPECT_EQ(model.Points(), kRgbCurve);
  EXPECT_EQ(model.ChannelPoints(CurveChannel::Blue), kBlueCurve);

  // A curve with the same points is no change: the revision stays.
  const auto after_blue = model.Revision();
  model.ApplyUpdate(blue_only);
  EXPECT_EQ(model.Revision(), after_blue);

  // Fewer than two points set the identity, as SetPoints does for the RGB curve.
  CurveUpdate reset_blue;
  reset_blue.blue = std::vector<CurvePoint>{};
  model.ApplyUpdate(reset_blue);
  EXPECT_EQ(model.ChannelPoints(CurveChannel::Blue), IdentityCurvePoints());
}

}  // namespace
}  // namespace alcedo
