//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// EditorParameterCatalog: UI ranges of the editor fields, UI-to-Model conversions, and range
// rejection. The converted Model JSON must parse through ParseEditorParameterWrite, the parser
// that history replay and the QML collection boundary use.

#include <gtest/gtest.h>

#include <cmath>
#include <json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>

#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_parameter_catalog.hpp"
#include "app/editor_parameter_write.hpp"

namespace alcedo {
namespace {

constexpr double kRoundTripTolerance = 1e-6;

/// The Model value that a parsed write carries: the scalar, or the Sharpen amount.
auto WrittenModelValue(const EditorParameterWrite& write) -> double {
  if (const auto* scalar = std::get_if<EditorScalarWrite>(&write)) {
    return scalar->value;
  }
  if (const auto* sharpen = std::get_if<SharpenUpdate>(&write)) {
    return sharpen->amount.value_or(-1.0f);
  }
  ADD_FAILURE() << "write is not a scalar or Sharpen operation";
  return 0.0;
}

auto UiToModelJson(std::string_view field, double ui_value, std::string* error = nullptr)
    -> std::optional<nlohmann::json> {
  return EditorParameterCatalog::ToModelJson(field, nlohmann::json(ui_value), nlohmann::json{},
                                             error);
}

TEST(EditorParameterCatalogTest, EveryScalarFieldIsAKnownFieldKey) {
  const std::set<std::string> expected = {
      "exposure",   "contrast", "highlights", "shadows", "white",   "black",      "saturation",
      "vibrance",   "clarity",  "sharpen",    "diffusion", "film_grain", "halation"};
  std::set<std::string> listed;
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    listed.insert(std::string{entry.field});
    const auto resolved = ResolveEditorAdjustmentField(std::string{entry.field});
    ASSERT_TRUE(resolved.has_value()) << entry.field;
    EXPECT_EQ(*resolved, entry.adjustment) << entry.field;
    if (!entry.alias.empty()) {
      const auto alias = ResolveEditorAdjustmentField(std::string{entry.alias});
      ASSERT_TRUE(alias.has_value()) << entry.alias;
      EXPECT_EQ(*alias, entry.adjustment) << entry.alias;
      EXPECT_EQ(EditorParameterCatalog::Find(entry.alias), &entry) << entry.alias;
    }
    EXPECT_EQ(EditorParameterCatalog::Find(entry.field), &entry) << entry.field;
    EXPECT_LT(entry.range.minimum, entry.range.maximum) << entry.field;
    EXPECT_GE(entry.range.default_value, entry.range.minimum) << entry.field;
    EXPECT_LE(entry.range.default_value, entry.range.maximum) << entry.field;
    EXPECT_GT(entry.range.step, 0.0) << entry.field;
  }
  EXPECT_EQ(listed, expected);
  EXPECT_EQ(EditorParameterCatalog::Find("not_a_field"), nullptr);
}

TEST(EditorParameterCatalogTest, ScalarRangesMatchPreviousPanelSliders) {
  struct Case {
    const char* field;
    const char* panel;
    double      minimum;
    double      maximum;
    double      step;
    int         decimals;
  };
  const Case cases[] = {
      {"exposure", "tone", -10.0, 10.0, 0.01, 2},  {"contrast", "tone", -100.0, 100.0, 1.0, 0},
      {"highlights", "tone", -100.0, 100.0, 1.0, 0}, {"shadows", "tone", -100.0, 100.0, 1.0, 0},
      {"white", "tone", -100.0, 100.0, 1.0, 0},      {"black", "tone", -100.0, 100.0, 1.0, 0},
      {"saturation", "look", -100.0, 100.0, 1.0, 0}, {"vibrance", "look", -100.0, 100.0, 1.0, 0},
      {"diffusion", "post", 0.0, 100.0, 1.0, 0},     {"clarity", "post", -100.0, 100.0, 1.0, 0},
      {"sharpen", "post", 0.0, 100.0, 1.0, 0},       {"film_grain", "post", 0.0, 100.0, 1.0, 0},
      {"halation", "post", 0.0, 100.0, 1.0, 0},
  };
  for (const auto& test_case : cases) {
    const auto* entry = EditorParameterCatalog::Find(test_case.field);
    ASSERT_NE(entry, nullptr) << test_case.field;
    EXPECT_EQ(entry->panel, test_case.panel) << test_case.field;
    EXPECT_DOUBLE_EQ(entry->range.minimum, test_case.minimum) << test_case.field;
    EXPECT_DOUBLE_EQ(entry->range.maximum, test_case.maximum) << test_case.field;
    EXPECT_DOUBLE_EQ(entry->range.default_value, 0.0) << test_case.field;
    EXPECT_DOUBLE_EQ(entry->range.step, test_case.step) << test_case.field;
    EXPECT_EQ(entry->range.decimals, test_case.decimals) << test_case.field;
  }
}

TEST(EditorParameterCatalogTest, SaturationUiValueRoundTrips) {
  for (const double ui_value : {-100.0, 0.0, 37.0, 100.0}) {
    const auto model = UiToModelJson("saturation", ui_value);
    ASSERT_TRUE(model.has_value()) << ui_value;
    EXPECT_NEAR(model->at("saturation").get<double>(), 1.0 + ui_value / 100.0,
                kRoundTripTolerance);
    std::string error;
    const auto  back = EditorParameterCatalog::ToUiValue("saturation", *model, &error);
    ASSERT_TRUE(back.has_value()) << error;
    EXPECT_NEAR(back->get<double>(), ui_value, kRoundTripTolerance);
  }
}

TEST(EditorParameterCatalogTest, EveryStepValueRoundTripsThroughModelJson) {
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    const int steps = static_cast<int>(
        std::lround((entry.range.maximum - entry.range.minimum) / entry.range.step));
    for (int index = 0; index <= steps; ++index) {
      const double ui_value = entry.range.minimum + index * entry.range.step;
      if (ui_value > entry.range.maximum) {
        break;
      }
      std::string error;
      const auto  model = UiToModelJson(entry.field, ui_value, &error);
      ASSERT_TRUE(model.has_value()) << entry.field << " " << ui_value << ": " << error;
      const auto back = EditorParameterCatalog::ToUiValue(entry.field, *model, &error);
      ASSERT_TRUE(back.has_value()) << entry.field << ": " << error;
      EXPECT_NEAR(back->get<double>(), ui_value, kRoundTripTolerance) << entry.field;
    }
  }
}

TEST(EditorParameterCatalogTest, OutOfRangeValueIsRejectedWithRange) {
  std::string error;
  EXPECT_FALSE(UiToModelJson("saturation", 150.0, &error).has_value());
  EXPECT_EQ(error, "saturation must be in [-100, 100]");

  error.clear();
  EXPECT_FALSE(UiToModelJson("exposure", -10.5, &error).has_value());
  EXPECT_EQ(error, "exposure must be in [-10, 10]");

  error.clear();
  EXPECT_FALSE(UiToModelJson("film_grain", -1.0, &error).has_value());
  EXPECT_EQ(error, "film_grain must be in [0, 100]");

  // The range limits themselves are valid values.
  EXPECT_TRUE(UiToModelJson("saturation", -100.0).has_value());
  EXPECT_TRUE(UiToModelJson("saturation", 100.0).has_value());
}

TEST(EditorParameterCatalogTest, NonNumericOrUnknownFieldValueIsRejected) {
  std::string error;
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("contrast", nlohmann::json("20"),
                                                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "contrast must be a number in [-100, 100]");

  error.clear();
  EXPECT_FALSE(UiToModelJson("not_a_field", 1.0, &error).has_value());
  EXPECT_EQ(error, "Unknown editor parameter field: not_a_field");

  error.clear();
  EXPECT_FALSE(
      EditorParameterCatalog::ToUiValue("contrast", nlohmann::json{{"other", 1.0}}, &error)
          .has_value());
  EXPECT_FALSE(error.empty());
}

TEST(EditorParameterCatalogTest, ModelJsonParsesThroughParameterWriteParser) {
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    for (const double ui_value :
         {entry.range.default_value, entry.range.minimum, entry.range.maximum}) {
      std::string error;
      const auto  model = UiToModelJson(entry.field, ui_value, &error);
      ASSERT_TRUE(model.has_value()) << entry.field << ": " << error;
      const auto write = ParseEditorParameterWrite(entry.field, *model, &error);
      ASSERT_TRUE(write.has_value()) << entry.field << " " << model->dump() << ": " << error;
      EXPECT_NEAR(WrittenModelValue(*write),
                  EditorParameterCatalog::ScalarUiToModel(entry, ui_value), 1e-5)
          << entry.field;
    }
  }
}

TEST(EditorParameterCatalogTest, ConversionsMatchPreviousPanelUnits) {
  const auto* saturation = EditorParameterCatalog::Find("saturation");
  const auto* film_grain = EditorParameterCatalog::Find("film_grain");
  const auto* diffusion  = EditorParameterCatalog::Find("diffusion");
  const auto* halation   = EditorParameterCatalog::Find("halation");
  const auto* sharpen    = EditorParameterCatalog::Find("sharpen");
  const auto* exposure   = EditorParameterCatalog::Find("exposure");
  ASSERT_NE(saturation, nullptr);
  ASSERT_NE(film_grain, nullptr);
  ASSERT_NE(diffusion, nullptr);
  ASSERT_NE(halation, nullptr);
  ASSERT_NE(sharpen, nullptr);
  ASSERT_NE(exposure, nullptr);

  EXPECT_DOUBLE_EQ(EditorParameterCatalog::ScalarUiToModel(*saturation, 40.0), 1.4);
  EXPECT_DOUBLE_EQ(EditorParameterCatalog::ScalarUiToModel(*saturation, -100.0), 0.0);
  EXPECT_DOUBLE_EQ(EditorParameterCatalog::ScalarUiToModel(*film_grain, 35.0), 0.35);
  EXPECT_DOUBLE_EQ(EditorParameterCatalog::ScalarUiToModel(*diffusion, 50.0), 0.5);
  EXPECT_DOUBLE_EQ(EditorParameterCatalog::ScalarUiToModel(*halation, 80.0), 0.8);
  EXPECT_DOUBLE_EQ(EditorParameterCatalog::ScalarUiToModel(*sharpen, 65.0), 65.0);
  EXPECT_DOUBLE_EQ(EditorParameterCatalog::ScalarUiToModel(*exposure, 1.25), 1.25);

  const auto sharpen_json = UiToModelJson("sharpen", 65.0);
  ASSERT_TRUE(sharpen_json.has_value());
  EXPECT_EQ(*sharpen_json, (nlohmann::json{{"offset", 65.0}}));
  const auto exposure_json = UiToModelJson("exposure", 1.25);
  ASSERT_TRUE(exposure_json.has_value());
  EXPECT_EQ(*exposure_json, (nlohmann::json{{"exposure_ev", 1.25}}));
  const auto grain_json = UiToModelJson("film_grain", 35.0);
  ASSERT_TRUE(grain_json.has_value());
  EXPECT_NEAR(grain_json->at("strength").get<double>(), 0.35, kRoundTripTolerance);
}

TEST(EditorParameterCatalogTest, UiValueReadsPanelProjectionForms) {
  std::string error;
  // Flat scalar under the field key: {"exposure": 1.5}.
  auto value = EditorParameterCatalog::ToUiValue("exposure", {{"exposure", 1.5}}, &error);
  ASSERT_TRUE(value.has_value()) << error;
  EXPECT_DOUBLE_EQ(value->get<double>(), 1.5);
  // Nested strength: {"film_grain": {"strength": 0.35}}.
  value = EditorParameterCatalog::ToUiValue("film_grain", {{"film_grain", {{"strength", 0.35}}}},
                                            &error);
  ASSERT_TRUE(value.has_value()) << error;
  EXPECT_NEAR(value->get<double>(), 35.0, kRoundTripTolerance);
  // Nested offset: {"sharpen": {"offset": 65}}.
  value = EditorParameterCatalog::ToUiValue("sharpen", {{"sharpen", {{"offset", 65.0}}}}, &error);
  ASSERT_TRUE(value.has_value()) << error;
  EXPECT_DOUBLE_EQ(value->get<double>(), 65.0);
  // The alias resolves to the same entry: {"value": 12}.
  value = EditorParameterCatalog::ToUiValue("whites", {{"value", 12.0}}, &error);
  ASSERT_TRUE(value.has_value()) << error;
  EXPECT_DOUBLE_EQ(value->get<double>(), 12.0);
}

TEST(EditorParameterCatalogTest, CatalogJsonListsEveryEntryWithUiRange) {
  const auto catalog = EditorParameterCatalog::CatalogJson();
  ASSERT_TRUE(catalog.is_array());
  ASSERT_EQ(catalog.size(), EditorParameterCatalog::Entries().size());
  const nlohmann::json* saturation = nullptr;
  const nlohmann::json* white      = nullptr;
  for (const auto& item : catalog) {
    EXPECT_EQ(item.at("kind"), "scalar");
    if (item.at("field") == "saturation") saturation = &item;
    if (item.at("field") == "white") white = &item;
  }
  ASSERT_NE(saturation, nullptr);
  EXPECT_EQ(saturation->at("panel"), "look");
  EXPECT_DOUBLE_EQ(saturation->at("ui_min").get<double>(), -100.0);
  EXPECT_DOUBLE_EQ(saturation->at("ui_max").get<double>(), 100.0);
  EXPECT_DOUBLE_EQ(saturation->at("ui_default").get<double>(), 0.0);
  EXPECT_DOUBLE_EQ(saturation->at("ui_step").get<double>(), 1.0);
  EXPECT_FALSE(saturation->contains("aliases"));
  ASSERT_NE(white, nullptr);
  EXPECT_EQ(white->at("aliases"), nlohmann::json::array({"whites"}));
}

}  // namespace
}  // namespace alcedo
