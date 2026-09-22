//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_adjustment_pipeline.hpp"

#include <gtest/gtest.h>

#include <string>

namespace alcedo {
namespace {

TEST(EditorAdjustmentPipelineTest, WritePayloadMapsOntoDocumentModelKeys) {
  const auto from_field = EditorAdjustmentDocumentParamsFromWrite("exposure", {{"exposure", 1.25}});
  EXPECT_FLOAT_EQ(from_field.at("exposure_ev").get<float>(), 1.25f);
  EXPECT_FALSE(from_field.contains("exposure"));

  const auto from_value = EditorAdjustmentDocumentParamsFromWrite("exposure", {{"value", 0.5}});
  EXPECT_FLOAT_EQ(from_value.at("exposure_ev").get<float>(), 0.5f);
  EXPECT_FALSE(from_value.contains("value"));

  const auto from_lut =
      EditorAdjustmentDocumentParamsFromWrite("lut", {{"ocio_lmt", "looks/film.cube"}});
  EXPECT_EQ(from_lut.at("cube_path").get<std::string>(), "looks/film.cube");
  EXPECT_FALSE(from_lut.contains("ocio_lmt"));

  const auto unchanged = EditorAdjustmentDocumentParamsFromWrite("contrast", {{"contrast", 12.0}});
  EXPECT_FLOAT_EQ(unchanged.at("contrast").get<float>(), 12.0f);
}

TEST(EditorAdjustmentPipelineTest, UnknownFieldKeyHasNoAdjustmentField) {
  EXPECT_TRUE(ResolveEditorAdjustmentField("exposure").has_value());
  EXPECT_FALSE(ResolveEditorAdjustmentField("not_a_field").has_value());
}

}  // namespace
}  // namespace alcedo
