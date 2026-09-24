//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include "edit/operators/basic/color_temp_op.hpp"

namespace alcedo {
namespace {

TEST(ColorTempOpParamsTest, GetParamsExportsAsShotAndCustomKeys) {
  ColorTempOp op;
  op.SetParams({{"color_temp",
                 {{"mode", "custom"},
                  {"custom_cct", 7200.0},
                  {"custom_tint", 8.0},
                  {"as_shot_cct", 5100.0},
                  {"as_shot_tint", -4.0}}}});

  const auto params = op.GetParams()["color_temp"];
  EXPECT_EQ(params.value("mode", std::string{}), "custom");
  EXPECT_DOUBLE_EQ(params.value("custom_cct", 0.0), 7200.0);
  EXPECT_DOUBLE_EQ(params.value("custom_tint", 0.0), 8.0);
  EXPECT_DOUBLE_EQ(params.value("as_shot_cct", 0.0), 5100.0);
  EXPECT_DOUBLE_EQ(params.value("as_shot_tint", 0.0), -4.0);
  EXPECT_FALSE(params.contains("resolved_cct"));
  EXPECT_FALSE(params.contains("cct"));
}

TEST(ColorTempOpParamsTest, SetParamsAcceptsLegacyResolvedAndCctAliases) {
  ColorTempOp op;
  op.SetParams({{"color_temp",
                 {{"mode", "custom"},
                  {"cct", 6800.0},
                  {"tint", 3.0},
                  {"resolved_cct", 4900.0},
                  {"resolved_tint", -6.0}}}});

  const auto params = op.GetParams()["color_temp"];
  EXPECT_DOUBLE_EQ(params.value("custom_cct", 0.0), 6800.0);
  EXPECT_DOUBLE_EQ(params.value("custom_tint", 0.0), 3.0);
  EXPECT_DOUBLE_EQ(params.value("as_shot_cct", 0.0), 4900.0);
  EXPECT_DOUBLE_EQ(params.value("as_shot_tint", 0.0), -6.0);
}

TEST(ColorTempOpParamsTest, RoundTripPreservesAsShotBaselineAcrossModeChange) {
  ColorTempOp op;
  op.SetParams({{"color_temp",
                 {{"mode", "custom"},
                  {"custom_cct", 7000.0},
                  {"custom_tint", 10.0},
                  {"as_shot_cct", 4550.0},
                  {"as_shot_tint", -2.0}}}});
  op.SetParams({{"color_temp", {{"mode", "as_shot"}}}});

  const auto params = op.GetParams()["color_temp"];
  EXPECT_EQ(params.value("mode", std::string{}), "as_shot");
  EXPECT_DOUBLE_EQ(params.value("as_shot_cct", 0.0), 4550.0);
  EXPECT_DOUBLE_EQ(params.value("as_shot_tint", 0.0), -2.0);
  EXPECT_DOUBLE_EQ(params.value("custom_cct", 0.0), 7000.0);
}

}  // namespace
}  // namespace alcedo
