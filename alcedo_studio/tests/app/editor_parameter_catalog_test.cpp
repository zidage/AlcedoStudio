//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// EditorParameterCatalog: UI ranges of the editor fields, UI-to-Model conversions, range
// rejection, the object fields of the RAW Decode, Geometry, Display Transform, white balance,
// HLS, and color wheel controls, and the Model JSON fields curve and lut. The converted Model
// JSON must parse through ParseEditorParameterWrite, the parser that history replay and the QML
// collection boundary use.

#include "app/editor_parameter_catalog.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "app/editor_adjustment_context.hpp"
#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_parameter_write.hpp"
#include "app/film_stock_presentation.hpp"
#include "edit/geometry/crop_frame.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/color_wheel_tone.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/tone_curve_presets.hpp"
#include "ui/alcedo_main/editor_support/modules/hls_math.hpp"

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

auto IsScalar(const EditorParameterCatalogEntry& entry) -> bool {
  return entry.kind == EditorParameterValueKind::Scalar;
}

auto IsObject(const EditorParameterCatalogEntry& entry) -> bool {
  return entry.kind == EditorParameterValueKind::Object;
}

auto KindName(const EditorParameterCatalogEntry& entry) -> std::string {
  switch (entry.kind) {
    case EditorParameterValueKind::Object:
      return "object";
    case EditorParameterValueKind::Model:
      return "model";
    case EditorParameterValueKind::Scalar:
    default:
      return "scalar";
  }
}

/// Crop projection form of the Geometry panel snapshot.
auto CropProjection(double x, double y, double w, double h, double angle, const char* preset,
                    double aspect_width, double aspect_height) -> nlohmann::json {
  return {{"crop_rotate",
           {{"crop_rect", {{"x", x}, {"y", y}, {"w", w}, {"h", h}}},
            {"angle_degrees", angle},
            {"aspect_ratio_preset", preset},
            {"aspect_ratio", {{"width", aspect_width}, {"height", aspect_height}}}}}};
}

/// Width / height of the crop frame in source pixels.
auto FramePixelAspect(const nlohmann::json& crop_model, double source_w, double source_h)
    -> double {
  const auto& rect = crop_model.at("crop_rotate").at("crop_rect");
  return rect.at("w").get<double>() * source_w / (rect.at("h").get<double>() * source_h);
}

TEST(EditorParameterCatalogTest, EveryScalarFieldIsAKnownFieldKey) {
  const std::set<std::string> expected = {
      "exposure",   "contrast", "highlights", "shadows", "white",   "black",      "saturation",
      "vibrance",   "clarity",  "sharpen",    "diffusion", "film_grain", "halation"};
  std::set<std::string> listed;
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    if (!IsScalar(entry)) {
      continue;
    }
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
      {"sharpen", "post", 0.0, 100.0, 1.0, 0},       {"film_grain", "post", 0.0, 200.0, 1.0, 0},
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
    if (!IsScalar(entry)) {
      continue;
    }
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
  EXPECT_EQ(error, "film_grain must be in [0, 200]");

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
    if (!IsScalar(entry)) {
      std::string error;
      const auto  model = EditorParameterCatalog::ToModelJson(
          entry.field, EditorParameterCatalog::UiDefault(entry), nlohmann::json{}, &error);
      ASSERT_TRUE(model.has_value()) << entry.field << ": " << error;
      EXPECT_TRUE(ParseEditorParameterWrite(entry.field, *model, &error).has_value())
          << entry.field << " " << model->dump() << ": " << error;
      continue;
    }
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
  const nlohmann::json* crop       = nullptr;
  for (const auto& item : catalog) {
    const auto* entry = EditorParameterCatalog::Find(item.at("field").get<std::string>());
    ASSERT_NE(entry, nullptr) << item.dump();
    EXPECT_EQ(item.at("kind"), KindName(*entry));
    if (item.at("field") == "saturation") saturation = &item;
    if (item.at("field") == "white") white = &item;
    if (item.at("field") == "crop_rotate") crop = &item;
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
  ASSERT_NE(crop, nullptr);
  EXPECT_EQ(crop->at("panel"), "geometry");
  EXPECT_EQ(crop->at("ui_default").at("aspect_preset"), "free");
  const auto& properties = crop->at("properties");
  ASSERT_EQ(properties.size(), 8U);
  EXPECT_EQ(properties.at(2).at("name"), "width");
  EXPECT_DOUBLE_EQ(properties.at(2).at("ui_min").get<double>(), 0.0001);
  EXPECT_DOUBLE_EQ(properties.at(2).at("ui_step").get<double>(), 0.001);
  EXPECT_EQ(properties.at(2).at("ui_decimals"), 3);
  EXPECT_EQ(properties.at(5).at("name"), "aspect_preset");
  EXPECT_EQ(properties.at(5).at("options").size(), 12U);
  EXPECT_EQ(properties.at(5).at("options").at(4),
            (nlohmann::json{{"value", "ratio_16_9"}, {"label", "16:9"}}));
}

TEST(EditorParameterCatalogTest, ObjectFieldsAreKnownFieldKeysWithPanels) {
  const std::set<std::pair<std::string, std::string>> expected = {
      {"raw_decode", "raw"},
      {"input_profile", "raw"},
      {"lens_calib", "raw"},
      {"crop_rotate", "geometry"},
      {"odt", "display"},
      {"color_temp", "raw"},
      {"grade_white_balance", "look"},
      {"hls", "look"},
      {"color_wheel", "look"},
      {"film_format", "post"},
  };
  std::set<std::pair<std::string, std::string>> listed;
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    if (!IsObject(entry)) {
      continue;
    }
    listed.insert({std::string{entry.field}, std::string{entry.panel}});
    const auto resolved = ResolveEditorAdjustmentField(std::string{entry.field});
    ASSERT_TRUE(resolved.has_value()) << entry.field;
    EXPECT_EQ(*resolved, entry.adjustment) << entry.field;
    std::string error;
    EXPECT_TRUE(EditorParameterCatalog::ValidateObjectUiValue(
        entry, EditorParameterCatalog::UiDefault(entry), &error))
        << entry.field << ": " << error;
  }
  EXPECT_EQ(listed, expected);
}

TEST(EditorParameterCatalogTest, RawDecodeDefaultsMatchPreviousPanelDefaults) {
  // The previous panel builder: method default, highlight reconstruction on, camera white
  // balance, 7600 K user white balance, and the alcedo backend.
  const nlohmann::json previous = {{"raw",
                                    {{"method", "default"},
                                     {"highlights_reconstruct", true},
                                     {"use_camera_wb", true},
                                     {"user_wb", 7600.0},
                                     {"backend", "alcedo"}}}};
  std::string error;
  const auto  defaults = EditorParameterCatalog::ToModelJson(
      "raw_decode", nlohmann::json::object(), nlohmann::json{}, &error);
  ASSERT_TRUE(defaults.has_value()) << error;
  EXPECT_EQ(*defaults, previous);
  const auto* entry = EditorParameterCatalog::Find("raw_decode");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(EditorParameterCatalog::UiDefault(*entry),
            (nlohmann::json{{"method", "default"}, {"highlights_reconstruct", true}}));
  EXPECT_TRUE(ParseEditorParameterWrite("raw_decode", *defaults, &error).has_value()) << error;

  // A partial write keeps the other property of the current projection.
  const nlohmann::json current = {
      {"raw", {{"method", "neural_engine"}, {"highlights_reconstruct", false}}}};
  const auto legacy =
      EditorParameterCatalog::ToModelJson("raw_decode", {{"method", "legacy"}}, current, &error);
  ASSERT_TRUE(legacy.has_value()) << error;
  EXPECT_EQ(legacy->at("raw").at("method"), "legacy");
  EXPECT_EQ(legacy->at("raw").at("highlights_reconstruct"), false);
  EXPECT_EQ(legacy->at("raw").at("user_wb"), 7600.0);

  // The panel lists the method options in menu order.
  ASSERT_EQ(entry->properties.size(), 2U);
  ASSERT_EQ(entry->properties[0].options.size(), 3U);
  EXPECT_EQ(entry->properties[0].options[2].value, "neural_engine");
  EXPECT_EQ(entry->properties[0].options[2].label, "Neural Engine");

  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("raw_decode", {{"method", "ahd"}}, current,
                                                   &error)
                   .has_value());
  EXPECT_EQ(error, "raw_decode.method must be one of: default, legacy, neural_engine");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("raw_decode", {{"backend", "cuda"}}, current,
                                                   &error)
                   .has_value());
  EXPECT_EQ(error,
            "Unknown raw_decode property: backend. Properties: method, highlights_reconstruct");
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson("raw_decode", 1.0, current, &error).has_value());
  EXPECT_EQ(error, "raw_decode must be an object with properties: method, highlights_reconstruct");
}

TEST(EditorParameterCatalogTest, InputProfileOptionsAreTheDevelopOverrides) {
  const auto* entry = EditorParameterCatalog::Find("input_profile");
  ASSERT_NE(entry, nullptr);
  ASSERT_EQ(entry->properties.size(), 1U);
  ASSERT_EQ(entry->properties[0].options.size(), 7U);
  EXPECT_EQ(entry->properties[0].options[0].label, "Auto (from file)");
  std::string error;
  const auto  model = EditorParameterCatalog::ToModelJson(
      "input_profile", {{"profile_override", "display_p3"}}, nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(*model, (nlohmann::json{{"profile_override", "display_p3"}}));
  // The panel projection wraps the Develop input object.
  const auto ui = EditorParameterCatalog::ToUiValue(
      "input_profile", {{"input", {{"raster", true}, {"profile_override", "prophoto"}}}}, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_EQ(*ui, (nlohmann::json{{"profile_override", "prophoto"}}));
  // A RAW image has no input profile: the field has no UI value.
  EXPECT_FALSE(EditorParameterCatalog::ToUiValue(
                   "input_profile", {{"input", {{"raster", false}, {"profile_override", "auto"}}}},
                   &error)
                   .has_value());
  EXPECT_EQ(error, "input_profile: a RAW image has no input profile");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "input_profile", {{"profile_override", "auto"}},
                   {{"input", {{"raster", false}, {"profile_override", "auto"}}}}, &error)
                   .has_value());
  EXPECT_EQ(error, "input_profile: a RAW image has no input profile");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "input_profile", {{"profile_override", "aces"}}, nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error,
            "input_profile.profile_override must be one of: auto, srgb, display_p3, adobe_rgb, "
            "rec2020, prophoto, linear_rec709");
}

TEST(EditorParameterCatalogTest, FilmGrainRangeIsTwiceTheDatasheetGranularity) {
  // Plan 'film_grain_format_and_stock_plan.md', decision U3: 100 % is the datasheet granularity,
  // and the slider goes to 200 %. A stored value of a document made before the change (0 to 1)
  // reads in range and is not changed.
  const auto model = UiToModelJson("film_grain", 200.0);
  ASSERT_TRUE(model.has_value());
  EXPECT_NEAR(model->at("strength").get<double>(), 2.0, kRoundTripTolerance);
  std::string error;
  for (const double stored : {0.0, 0.3, 0.6, 1.0}) {
    const auto ui = EditorParameterCatalog::ToUiValue("film_grain", {{"strength", stored}}, &error);
    ASSERT_TRUE(ui.has_value()) << error;
    EXPECT_NEAR(ui->get<double>(), stored * 100.0, kRoundTripTolerance);
  }
  EXPECT_FALSE(UiToModelJson("film_grain", 201.0).has_value());
}

TEST(EditorParameterCatalogTest, FilmFormatOptionsAreTheThreeFormatsWithDefault35mm) {
  const auto* entry = EditorParameterCatalog::Find("film_format");
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->kind, EditorParameterValueKind::Object);
  EXPECT_EQ(entry->panel, "post");
  ASSERT_EQ(entry->properties.size(), 1U);
  EXPECT_EQ(entry->properties[0].name, "film_format");
  ASSERT_EQ(entry->properties[0].options.size(), 3U);
  // The options are the document identifiers of the formats, in menu order.
  const FilmFormat formats[] = {FilmFormat::Film16mm, FilmFormat::Film35mm, FilmFormat::Film70mm};
  for (std::size_t index = 0; index < 3; ++index) {
    EXPECT_EQ(entry->properties[0].options[index].value, FilmFormatId(formats[index]));
    EXPECT_EQ(entry->properties[0].options[index].label, FilmFormatId(formats[index]));
  }
  EXPECT_EQ(EditorParameterCatalog::UiDefault(*entry), (nlohmann::json{{"film_format", "35mm"}}));
  std::string error;
  const auto  model = EditorParameterCatalog::ToModelJson("film_format", {{"film_format", "16mm"}},
                                                          nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(*model, (nlohmann::json{{"film_format", "16mm"}}));
  // The panel projection wraps the value in the field key.
  const auto ui = EditorParameterCatalog::ToUiValue(
      "film_format", {{"film_format", {{"film_format", "70mm"}}}}, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_EQ(*ui, (nlohmann::json{{"film_format", "70mm"}}));
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("film_format", {{"film_format", "8mm"}},
                                                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "film_format.film_format must be one of: 16mm, 35mm, 70mm");
}

TEST(EditorParameterCatalogTest, LensDefaultsMergeMakerAndModel) {
  const nlohmann::json current = {
      {"lens_calib", {{"enabled", true}, {"lens_maker", ""}, {"lens_model", ""}}}};
  std::string error;
  const auto  maker = EditorParameterCatalog::ToModelJson("lens_calib", {{"lens_maker", "Canon"}},
                                                          current, &error);
  ASSERT_TRUE(maker.has_value()) << error;
  // Every lens default of a new Develop, with the merged switch and identity.
  auto expected                        = MakeDefaultLensCalibrationWriteJson();
  expected["lens_calib"]["enabled"]    = true;
  expected["lens_calib"]["lens_maker"] = "Canon";
  expected["lens_calib"]["lens_model"] = "";
  EXPECT_EQ(*maker, expected);
  EXPECT_TRUE(ParseEditorParameterWrite("lens_calib", *maker, &error).has_value()) << error;

  // The model write keeps the maker of the current value.
  const auto model = EditorParameterCatalog::ToModelJson(
      "lens_calib", {{"lens_model", "EF 50mm f/1.8 STM"}}, *maker, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->at("lens_calib").at("lens_maker"), "Canon");
  EXPECT_EQ(model->at("lens_calib").at("lens_model"), "EF 50mm f/1.8 STM");
  EXPECT_EQ(model->at("lens_calib").at("enabled"), true);

  // A model without a maker does not name a lens: a write is rejected, and the panel state
  // (maker Auto) writes an empty model.
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("lens_calib", {{"lens_model", "EF 50mm"}},
                                                   current, &error)
                   .has_value());
  EXPECT_EQ(error, "lens_calib.lens_model needs a lens_maker");
  const auto panel = EditorParameterCatalog::UiStateToModelJson(
      "lens_calib", {{"enabled", false}, {"lens_maker", ""}, {"lens_model", "EF 50mm"}}, &error);
  ASSERT_TRUE(panel.has_value()) << error;
  EXPECT_EQ(panel->at("lens_calib").at("lens_model"), "");
  EXPECT_EQ(panel->at("lens_calib").at("enabled"), false);
}

TEST(EditorParameterCatalogTest, PartialCropMergesWithCurrentValue) {
  const auto  current = CropProjection(0.1, 0.2, 0.5, 0.4, 0.0, "free", 1.0, 1.0);
  std::string error;
  // Without a presented frame the position write keeps the other properties.
  const auto moved =
      EditorParameterCatalog::ToModelJson("crop_rotate", {{"x", 0.3}}, current, &error);
  ASSERT_TRUE(moved.has_value()) << error;
  EXPECT_EQ(*moved, CropProjection(0.3, 0.2, 0.5, 0.4, 0.0, "free", 1.0, 1.0));
  EXPECT_TRUE(ParseEditorParameterWrite("crop_rotate", *moved, &error).has_value()) << error;
  // An angle write changes only the angle.
  const auto angled =
      EditorParameterCatalog::ToModelJson("crop_rotate", {{"angle_degrees", 2.5}}, current, &error);
  ASSERT_TRUE(angled.has_value()) << error;
  EXPECT_EQ(*angled, CropProjection(0.1, 0.2, 0.5, 0.4, 2.5, "free", 1.0, 1.0));

  // A rotation of the full frame shrinks it so every rotated corner stays inside the source.
  const EditorParameterSource source{4000, 3000};
  const auto full = CropProjection(0.0, 0.0, 1.0, 1.0, 0.0, "free", 1.0, 1.0);
  const auto rotated = EditorParameterCatalog::ToModelJson("crop_rotate", {{"angle_degrees", 10.0}},
                                                           full, source, &error);
  ASSERT_TRUE(rotated.has_value()) << error;
  const auto clamped = ClampCropToRotatedSource(NormalizedRect{0.0f, 0.0f, 1.0f, 1.0f}, 10.0f,
                                                Extent2D{4000, 3000});
  const auto& rect = rotated->at("crop_rotate").at("crop_rect");
  EXPECT_LT(rect.at("w").get<double>(), 1.0);
  EXPECT_NEAR(rect.at("w").get<double>(), clamped.w, 1e-6);
  EXPECT_NEAR(rect.at("h").get<double>(), clamped.h, 1e-6);
  EXPECT_NEAR(rect.at("x").get<double>(), clamped.x, 1e-6);
  EXPECT_EQ(rotated->at("crop_rotate").at("angle_degrees"), 10.0);

  // An aspect size edit selects the custom preset and refits the frame to 3:2 pixels.
  const auto custom = EditorParameterCatalog::ToModelJson(
      "crop_rotate", {{"aspect_width", 3.0}, {"aspect_height", 2.0}}, full, source, &error);
  ASSERT_TRUE(custom.has_value()) << error;
  EXPECT_EQ(custom->at("crop_rotate").at("aspect_ratio_preset"), "custom");
  EXPECT_NEAR(FramePixelAspect(*custom, 4000.0, 3000.0), 1.5, 1e-3);

  // Out-of-range, unknown, and contradictory values are rejected with the allowed values.
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson("crop_rotate", {{"width", 1.5}}, current, &error)
          .has_value());
  EXPECT_EQ(error, "crop_rotate.width must be in [0.0001, 1]");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("crop_rotate", {{"angle_degrees", 200.0}},
                                                   current, &error)
                   .has_value());
  EXPECT_EQ(error, "crop_rotate.angle_degrees must be in [-180, 180]");
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson("crop_rotate", {{"w", 0.5}}, current, &error)
          .has_value());
  EXPECT_EQ(error,
            "Unknown crop_rotate property: w. Properties: x, y, width, height, angle_degrees, "
            "aspect_preset, aspect_width, aspect_height");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "crop_rotate", {{"aspect_preset", "ratio_1_1"}, {"aspect_width", 2.0}},
                   current, &error)
                   .has_value());
  EXPECT_EQ(error, "crop_rotate.aspect_width and aspect_height need aspect_preset custom");
}

TEST(EditorParameterCatalogTest, CompleteCropValueConstrainsForTheChangedValueOnly) {
  // editor.get -> change the angle -> editor.set with the complete value: the locked 16:9 frame
  // keeps its rectangle, because only the rotation changed.
  const auto  current = CropProjection(0.1, 0.2, 0.5, 0.4, 0.0, "ratio_16_9", 16.0, 9.0);
  std::string error;
  auto        ui = EditorParameterCatalog::ToUiValue("crop_rotate", current, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  (*ui)["angle_degrees"] = 2.0;
  const auto model = EditorParameterCatalog::ToModelJson("crop_rotate", *ui, current, &error);
  ASSERT_TRUE(model.has_value()) << error;
  const auto& rect = model->at("crop_rotate").at("crop_rect");
  EXPECT_DOUBLE_EQ(rect.at("x").get<double>(), 0.1);
  EXPECT_DOUBLE_EQ(rect.at("y").get<double>(), 0.2);
  EXPECT_DOUBLE_EQ(rect.at("w").get<double>(), 0.5);
  EXPECT_DOUBLE_EQ(rect.at("h").get<double>(), 0.4);
  EXPECT_DOUBLE_EQ(model->at("crop_rotate").at("angle_degrees").get<double>(), 2.0);
  // An aspect size change without a preset selects custom.
  const auto custom =
      EditorParameterCatalog::ToModelJson("crop_rotate", {{"aspect_width", 3.0}}, current, &error);
  ASSERT_TRUE(custom.has_value()) << error;
  EXPECT_EQ(custom->at("crop_rotate").at("aspect_ratio_preset"), "custom");
}

TEST(EditorParameterCatalogTest, PortraitAspectFlipFollowsOrientation) {
  const auto* entry = EditorParameterCatalog::Find("crop_rotate");
  ASSERT_NE(entry, nullptr);
  auto ui             = EditorParameterCatalog::UiDefault(*entry);
  ui["aspect_preset"] = "ratio_16_9";
  const auto landscape =
      EditorParameterCatalog::CropLockedAspectRatio(ui, EditorParameterSource{4000, 3000});
  const auto portrait =
      EditorParameterCatalog::CropLockedAspectRatio(ui, EditorParameterSource{3000, 4000});
  const auto unknown = EditorParameterCatalog::CropLockedAspectRatio(ui, EditorParameterSource{});
  ASSERT_TRUE(landscape.has_value());
  ASSERT_TRUE(portrait.has_value());
  ASSERT_TRUE(unknown.has_value());
  EXPECT_NEAR(*landscape, 16.0 / 9.0, 1e-5);
  EXPECT_NEAR(*portrait, 9.0 / 16.0, 1e-5);
  EXPECT_NEAR(*unknown, 16.0 / 9.0, 1e-5);

  // A square preset does not flip; the free preset is not locked.
  ui["aspect_preset"] = "ratio_1_1";
  EXPECT_NEAR(*EditorParameterCatalog::CropLockedAspectRatio(ui, EditorParameterSource{3000, 4000}),
              1.0, 1e-6);
  ui["aspect_preset"] = "free";
  EXPECT_FALSE(
      EditorParameterCatalog::CropLockedAspectRatio(ui, EditorParameterSource{3000, 4000})
          .has_value());

  // A preset write on a portrait source fits a 9:16 frame and keeps the unoriented 16:9 size.
  std::string error;
  const auto  full  = CropProjection(0.0, 0.0, 1.0, 1.0, 0.0, "free", 1.0, 1.0);
  const auto  model = EditorParameterCatalog::ToModelJson(
      "crop_rotate", {{"aspect_preset", "ratio_16_9"}}, full, EditorParameterSource{3000, 4000},
      &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_NEAR(FramePixelAspect(*model, 3000.0, 4000.0), 9.0 / 16.0, 1e-3);
  EXPECT_EQ(model->at("crop_rotate").at("aspect_ratio_preset"), "ratio_16_9");
  EXPECT_DOUBLE_EQ(model->at("crop_rotate").at("aspect_ratio").at("width").get<double>(), 16.0);
  EXPECT_DOUBLE_EQ(model->at("crop_rotate").at("aspect_ratio").at("height").get<double>(), 9.0);
  // The same write on a landscape source fits a 16:9 frame.
  const auto wide = EditorParameterCatalog::ToModelJson(
      "crop_rotate", {{"aspect_preset", "ratio_16_9"}}, full, EditorParameterSource{4000, 3000},
      &error);
  ASSERT_TRUE(wide.has_value()) << error;
  EXPECT_NEAR(FramePixelAspect(*wide, 4000.0, 3000.0), 16.0 / 9.0, 1e-3);
}

TEST(EditorParameterCatalogTest, InvalidEotfForSpaceIsRejectedWithValidList) {
  // An EOTF of another encoding space: the error lists the EOTFs of the selected space.
  std::string error;
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "odt", {{"encoding_space", "rec2020"}, {"encoding_eotf", "bt1886"}},
                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "odt.encoding_eotf must be one of: st2084, hlg for encoding_space rec2020");
  // A value that is no EOTF: the error lists every EOTF.
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "odt", {{"encoding_space", "rec2020"}, {"encoding_eotf", "gamma_9"}},
                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error,
            "odt.encoding_eotf must be one of: bt1886, gamma_2_2, srgb_piecewise, st2084, hlg");
  // An encoding space that the display transform Model does not have.
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("odt", {{"encoding_space", "p3_dci"}},
                                                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "odt.encoding_space must be one of: rec709, p3_d65, rec2020");

  // The panel rule: a space change keeps a valid EOTF, else it selects the first EOTF.
  auto model = EditorParameterCatalog::ToModelJson("odt", {{"encoding_space", "p3_d65"}},
                                                   nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->at("odt").at("encoding_eotf"), "gamma_2_2");
  model = EditorParameterCatalog::ToModelJson("odt", {{"encoding_space", "rec2020"}},
                                              nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->at("odt").at("encoding_eotf"), "st2084");
  const auto write = ParseEditorParameterWrite("odt", *model, &error);
  ASSERT_TRUE(write.has_value()) << error;
  const auto* drt = std::get_if<DrtParameterUpdate>(&*write);
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->encoding_space.has_value());
  EXPECT_EQ(*drt->encoding_space, DrtColorSpace::Rec2020);
  ASSERT_TRUE(drt->encoding_eotf.has_value());
  EXPECT_EQ(*drt->encoding_eotf, DrtEotf::St2084);

  // editor.catalog lists the EOTFs of each space.
  const auto entry = EditorParameterCatalog::EntryJson(*EditorParameterCatalog::Find("odt"));
  EXPECT_EQ(entry.at("encoding_eotf_by_space").at("rec2020"),
            nlohmann::json::array({"st2084", "hlg"}));
  EXPECT_EQ(entry.at("encoding_eotf_by_space").at("rec709"),
            nlohmann::json::array({"bt1886", "gamma_2_2", "srgb_piecewise"}));
}

TEST(EditorParameterCatalogTest, OdtWriteHoldsTheControlsOfTheSelectedMethod) {
  // The previous panel builder for the default state (OpenDRT).
  std::string error;
  const auto  open_drt = EditorParameterCatalog::ToModelJson("odt", nlohmann::json::object(),
                                                             nlohmann::json{}, &error);
  ASSERT_TRUE(open_drt.has_value()) << error;
  EXPECT_EQ(*open_drt, (nlohmann::json{{"odt",
                                        {{"method", "open_drt"},
                                         {"encoding_space", "rec709"},
                                         {"encoding_eotf", "gamma_2_2"},
                                         {"peak_luminance", 100.0},
                                         {"open_drt",
                                          {{"look_preset", "standard"},
                                           {"tonescale_preset", "use_look_preset"},
                                           {"creative_white", "use_look_preset"}}}}}}));

  // ACES writes the limiting space and no OpenDRT presets.
  const auto aces = EditorParameterCatalog::ToModelJson(
      "odt", {{"method", "aces_2_0"}, {"limiting_space", "rec2020"}}, nlohmann::json{}, &error);
  ASSERT_TRUE(aces.has_value()) << error;
  EXPECT_EQ(aces->at("odt").at("limiting_space"), "rec2020");
  EXPECT_FALSE(aces->at("odt").contains("open_drt"));

  // A change of a control that the selected method does not write is rejected.
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("odt", {{"limiting_space", "rec2020"}},
                                                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "odt.limiting_space needs method aces_2_0");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "odt", {{"method", "aces_2_0"}, {"look_preset", "umbra"}}, nlohmann::json{},
                   &error)
                   .has_value());
  EXPECT_EQ(error,
            "odt.look_preset needs method open_drt, or film_print with print material "
            "negative_scan");
  // The complete current value (for example from editor.get) writes without an error.
  EXPECT_TRUE(EditorParameterCatalog::ToModelJson(
                  "odt", EditorParameterCatalog::UiDefault(*EditorParameterCatalog::Find("odt")),
                  nlohmann::json{}, &error)
                  .has_value())
      << error;
}

TEST(EditorParameterCatalogTest, FilmPrintOptionsAreTheIdentifiersOfTheDocumentModel) {
  const auto* entry = EditorParameterCatalog::Find("odt");
  ASSERT_NE(entry, nullptr);
  const auto expect_ids = [&](std::string_view name, const auto& ids) {
    const auto property =
        std::find_if(entry->properties.begin(), entry->properties.end(),
                     [&](const EditorParameterProperty& item) { return item.name == name; });
    ASSERT_NE(property, entry->properties.end()) << name;
    ASSERT_EQ(property->options.size(), ids.size()) << name;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      EXPECT_EQ(property->options[i].value, ids[i]);
      EXPECT_FALSE(property->options[i].label.empty()) << ids[i];
    }
  };
  // The Film list has the negatives, then the reversal films in the order of the lab list.
  std::vector<std::string_view> films(kFilmNegativeIds.begin(), kFilmNegativeIds.end());
  films.insert(films.end(), kFilmReversalIds.begin(), kFilmReversalIds.end());
  expect_ids("negative", films);
  expect_ids("print_material", kFilmPrintMaterialIds);
  expect_ids("scene_light", kFilmSceneLightIds);
  EXPECT_EQ(EditorParameterCatalog::UiDefault(*entry).at("method"), "open_drt");
}

TEST(EditorParameterCatalogTest, FilmBrowserHasOneEntryForEachFilmAndPrintMaterial) {
  const auto expect_list = [](const auto& ids, FilmBrowserList list) {
    for (const auto id : ids) {
      const auto count =
          std::count_if(kFilmPresentations.begin(), kFilmPresentations.end(),
                        [&](const FilmPresentation& entry) { return entry.id == id; });
      EXPECT_EQ(count, 1) << id;
      const auto entry = FindFilmPresentation(id);
      ASSERT_TRUE(entry.has_value()) << id;
      EXPECT_EQ(entry->list, list) << id;
    }
  };
  expect_list(kFilmNegativeIds, FilmBrowserList::Negative);
  expect_list(kFilmReversalIds, FilmBrowserList::Reversal);
  expect_list(kFilmPrintMaterialIds, FilmBrowserList::Print);
  EXPECT_EQ(kFilmPresentations.size(),
            kFilmNegativeIds.size() + kFilmReversalIds.size() + kFilmPrintMaterialIds.size());

  // The browser text has no middle dot (U+00B7), by user decision.
  constexpr std::string_view kMiddleDot = "\xC2\xB7";
  std::set<std::string_view> brands;
  for (const auto& brand : kFilmBrands) {
    brands.insert(brand.name);
  }
  for (const auto& entry : kFilmPresentations) {
    EXPECT_FALSE(entry.short_label.empty()) << entry.id;
    EXPECT_GE(entry.color_count, 1U) << entry.id;
    EXPECT_LE(entry.color_count, entry.colors.size()) << entry.id;
    for (std::size_t i = 0; i < entry.color_count; ++i) {
      EXPECT_LE(entry.colors[i], 0xFFFFFFU) << entry.id;
    }
    // Only Negative Scan and Cine Look are above the brand groups.
    if (entry.id == "cine_look" || entry.id == kFilmNegativeScanId) {
      EXPECT_TRUE(entry.brand.empty());
    } else {
      EXPECT_TRUE(brands.contains(entry.brand)) << entry.id;
    }
    EXPECT_EQ(entry.shape == FilmSwatchShape::Reversal, entry.list == FilmBrowserList::Reversal)
        << entry.id;
    EXPECT_EQ(entry.short_label.find(kMiddleDot), std::string_view::npos) << entry.id;
    EXPECT_EQ(entry.family.find(kMiddleDot), std::string_view::npos) << entry.id;
  }

  // In each list, the entries of one brand are together, in the order of kFilmBrands.
  for (const auto list :
       {FilmBrowserList::Negative, FilmBrowserList::Reversal, FilmBrowserList::Print}) {
    std::vector<std::string_view> order;
    for (const auto& entry : kFilmPresentations) {
      if (entry.list == list && (order.empty() || order.back() != entry.brand)) {
        order.push_back(entry.brand);
      }
    }
    std::set<std::string_view> seen(order.begin(), order.end());
    EXPECT_EQ(seen.size(), order.size()) << "a brand is in two groups";
  }

  // Negative Scan is first in the print list, with its own swatch shape.
  const auto first_print = std::find_if(
      kFilmPresentations.begin(), kFilmPresentations.end(),
      [](const FilmPresentation& entry) { return entry.list == FilmBrowserList::Print; });
  ASSERT_NE(first_print, kFilmPresentations.end());
  EXPECT_EQ(first_print->id, kFilmNegativeScanId);
  EXPECT_EQ(first_print->short_label, "Negative Scan");
  EXPECT_EQ(first_print->shape, FilmSwatchShape::Scan);
  EXPECT_EQ(std::next(first_print)->id, "cine_look");

  // A change of process selects these films first.
  EXPECT_EQ(FindFilmPresentation(kFilmDefaultNegative)->list, FilmBrowserList::Negative);
  EXPECT_EQ(FindFilmPresentation(kFilmDefaultReversal)->list, FilmBrowserList::Reversal);
  EXPECT_EQ(kFilmDefaultNegative, FilmPrintParams{}.negative);
  // All Portra films have the same colours.
  EXPECT_EQ(FindFilmPresentation("kodak_portra_400")->colors,
            FindFilmPresentation("kodak_portra_160")->colors);
  EXPECT_EQ(FindFilmPresentation("kodak_portra_400")->colors,
            FindFilmPresentation("kodak_portra_800")->colors);
}

TEST(EditorParameterCatalogTest, FilmPrintWriteOfAReversalFilmKeepsThePrintMaterial) {
  std::string error;
  const auto  model = EditorParameterCatalog::ToModelJson("odt",
                                                          {{"method", "film_print"},
                                                           {"negative", "fuji_velvia_50"},
                                                           {"print_material", "kodak_2383"},
                                                           {"enlarger_flare", 0.02}},
                                                          nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  const auto& film_print = model->at("odt").at("film_print");
  EXPECT_EQ(film_print.at("negative"), "fuji_velvia_50");
  EXPECT_EQ(film_print.at("print_material"), "kodak_2383");
  EXPECT_DOUBLE_EQ(film_print.at("enlarger_flare").get<double>(), 0.02);
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "odt", {{"method", "film_print"}, {"negative", "fuji_velvia_9999"}},
                   nlohmann::json{}, &error)
                   .has_value());
}

TEST(EditorParameterCatalogTest, FilmPrintWriteOfNegativeScanKeepsTheStoredFlare) {
  std::string error;
  const auto  model = EditorParameterCatalog::ToModelJson("odt",
                                                          {{"method", "film_print"},
                                                           {"negative", "kodak_5207"},
                                                           {"print_material", "negative_scan"},
                                                           {"enlarger_flare", 0.02}},
                                                          nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  const auto& film_print = model->at("odt").at("film_print");
  EXPECT_EQ(film_print.at("print_material"), "negative_scan");
  // Negative Scan reads no Enlarger flare, and the panel keeps the stored value.
  EXPECT_DOUBLE_EQ(film_print.at("enlarger_flare").get<double>(), 0.02);
}

TEST(EditorParameterCatalogTest, NegativeScanPropertiesHaveTheRangesOfTheDocumentModel) {
  const auto* entry = EditorParameterCatalog::Find("odt");
  ASSERT_NE(entry, nullptr);
  const auto property = [&](std::string_view name) -> const EditorParameterProperty& {
    const auto it =
        std::find_if(entry->properties.begin(), entry->properties.end(),
                     [&](const EditorParameterProperty& item) { return item.name == name; });
    EXPECT_NE(it, entry->properties.end()) << name;
    return *it;
  };
  struct Range {
    std::string_view name;
    float            minimum;
    float            maximum;
  };
  for (const auto& range : {Range{"auto_strength", kFilmScanAutoStrengthMin, kFilmScanAutoStrengthMax},
                            Range{"auto_density", kFilmScanAutoDensityMin, kFilmScanAutoDensityMax},
                            Range{"scan_contrast", kFilmScanContrastMin, kFilmScanContrastMax},
                            Range{"scan_exposure_stops", kFilmScanExposureStopsMin,
                                  kFilmScanExposureStopsMax},
                            Range{"cyan", kFilmScanKeyMin, kFilmScanKeyMax},
                            Range{"magenta", kFilmScanKeyMin, kFilmScanKeyMax},
                            Range{"yellow", kFilmScanKeyMin, kFilmScanKeyMax},
                            Range{"black", kFilmScanBlackMin, kFilmScanBlackMax},
                            Range{"scanner_red_nm", kFilmScannerRedMinNm, kFilmScannerRedMaxNm},
                            Range{"scanner_green_nm", kFilmScannerGreenMinNm, kFilmScannerGreenMaxNm},
                            Range{"scanner_blue_nm", kFilmScannerBlueMinNm, kFilmScannerBlueMaxNm},
                            Range{"scanner_width_nm", kFilmScannerWidthMinNm,
                                  kFilmScannerWidthMaxNm}}) {
    const auto& item = property(range.name);
    EXPECT_EQ(item.type, EditorParameterPropertyType::Number) << range.name;
    EXPECT_DOUBLE_EQ(item.minimum, static_cast<double>(range.minimum)) << range.name;
    EXPECT_DOUBLE_EQ(item.maximum, static_cast<double>(range.maximum)) << range.name;
  }
  const auto& automatic = property("auto");
  ASSERT_EQ(automatic.options.size(), kFilmScanAutoIds.size());
  for (std::size_t i = 0; i < kFilmScanAutoIds.size(); ++i) {
    EXPECT_EQ(automatic.options[i].value, kFilmScanAutoIds[i]);
  }
  const auto& light = property("scanner_light");
  ASSERT_EQ(light.options.size(), kFilmScannerLightIds.size());
  EXPECT_EQ(light.options[0].value, "halogen");
  EXPECT_EQ(light.options[1].label, "Equal Energy");
  // The defaults of the panel are those of the Model.
  const auto defaults = EditorParameterCatalog::UiDefault(*entry);
  const FilmNegativeScanParams scan;
  EXPECT_EQ(defaults.at("auto"), scan.auto_colour);
  EXPECT_EQ(defaults.at("auto"), "neutral");
  EXPECT_EQ(defaults.at("auto_strength").get<double>(), 0.5);
  EXPECT_FLOAT_EQ(static_cast<float>(defaults.at("auto_strength").get<double>()), scan.auto_strength);
  EXPECT_DOUBLE_EQ(defaults.at("black").get<double>(), 1.0);
  EXPECT_DOUBLE_EQ(defaults.at("scanner_red_nm").get<double>(), 650.0);
  EXPECT_DOUBLE_EQ(defaults.at("scanner_width_nm").get<double>(), 40.0);
  EXPECT_EQ(defaults.at("scanner_light"), "halogen");
  // The default scene light is As Shot (issue #672).
  EXPECT_EQ(defaults.at("scene_light"), "as_shot");
}

TEST(EditorParameterCatalogTest, NegativeScanWritesTheOpenDrtSettingsAndTheEncodingOfThePanel) {
  // An HDR document with OpenDRT settings: Negative Scan keeps them, as method open_drt (issue
  // 661).
  const nlohmann::json hdr = {{"odt",
                               {{"method", "open_drt"},
                                {"encoding_space", "rec2020"},
                                {"encoding_eotf", "st2084"},
                                {"peak_luminance", 1000.0},
                                {"open_drt", {{"look_preset", "arriba"}}}}}};
  std::string          error;
  auto                 model = EditorParameterCatalog::ToModelJson(
      "odt", {{"method", "film_print"}, {"print_material", "negative_scan"}}, hdr, &error);
  ASSERT_TRUE(model.has_value()) << error;
  const auto& odt = model->at("odt");
  EXPECT_EQ(odt.at("method"), "film_print");
  EXPECT_EQ(odt.at("encoding_space"), "rec2020");
  EXPECT_EQ(odt.at("encoding_eotf"), "st2084");
  EXPECT_EQ(odt.at("peak_luminance"), 1000.0);
  EXPECT_EQ(odt.at("open_drt").at("look_preset"), "arriba");
  EXPECT_TRUE(odt.at("open_drt").contains("tonescale_preset"));
  EXPECT_TRUE(odt.at("open_drt").contains("creative_white"));
  // Method OpenDRT has no limiting space, so Negative Scan has none.
  EXPECT_FALSE(odt.contains("limiting_space"));
  // Selecting Negative Scan selects As Shot in the same write, and the scan settings are the
  // defaults of new settings (auto colour neutral at 0.5).
  EXPECT_EQ(odt.at("film_print").at("scene_light"), "as_shot");
  EXPECT_EQ(odt.at("film_print").at("auto"), "neutral");
  EXPECT_DOUBLE_EQ(odt.at("film_print").at("auto_strength").get<double>(), 0.5);
  EXPECT_DOUBLE_EQ(odt.at("film_print").at("black").get<double>(), 1.0);
  EXPECT_EQ(odt.at("film_print").at("scanner_light"), "halogen");

  // A write that selects Negative Scan and a scene light keeps that scene light.
  model = EditorParameterCatalog::ToModelJson(
      "odt",
      {{"method", "film_print"}, {"print_material", "negative_scan"}, {"scene_light", "tungsten"}},
      hdr, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->at("odt").at("film_print").at("scene_light"), "tungsten");

  // With Negative Scan, the OpenDRT settings, the encodings, and the scan settings write.
  auto scan = *EditorParameterCatalog::ToModelJson(
      "odt", {{"method", "film_print"}, {"print_material", "negative_scan"}}, nlohmann::json{},
      &error);
  model = EditorParameterCatalog::ToModelJson("odt",
                                              {{"tonescale_preset", "high_contrast"},
                                               {"encoding_space", "p3_d65"},
                                               {"auto", "neutral"},
                                               {"scan_exposure_stops", -0.5},
                                               {"scanner_width_nm", 60.0},
                                               {"scanner_light", "equal_energy"}},
                                              scan, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->at("odt").at("open_drt").at("tonescale_preset"), "high_contrast");
  EXPECT_EQ(model->at("odt").at("encoding_space"), "p3_d65");
  const auto& film_print = model->at("odt").at("film_print");
  EXPECT_EQ(film_print.at("auto"), "neutral");
  EXPECT_DOUBLE_EQ(film_print.at("scan_exposure_stops").get<double>(), -0.5);
  EXPECT_DOUBLE_EQ(film_print.at("scanner_width_nm").get<double>(), 60.0);
  EXPECT_EQ(film_print.at("scanner_light"), "equal_energy");
  // The Model JSON of the write reads back to the panel values.
  const auto ui = EditorParameterCatalog::ToUiValue("odt", *model, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_EQ(ui->at("auto"), "neutral");
  EXPECT_DOUBLE_EQ(ui->at("scanner_width_nm").get<double>(), 60.0);
  // A stored film print object of a build before G8 has no `auto` and `auto_strength` at Neutral
  // Scan, and reads as it was: auto colour off at strength 0.8.
  auto old_model = *model;
  old_model["odt"]["film_print"].erase("auto");
  old_model["odt"]["film_print"].erase("auto_strength");
  const auto old_ui = EditorParameterCatalog::ToUiValue("odt", old_model, &error);
  ASSERT_TRUE(old_ui.has_value()) << error;
  EXPECT_EQ(old_ui->at("auto"), "off");
  EXPECT_DOUBLE_EQ(old_ui->at("auto_strength").get<double>(), 0.8);
  // A later write of another value keeps As Shot: only a change of print material selects it.
  model = EditorParameterCatalog::ToModelJson("odt", {{"scene_light", "daylight"}}, *model, &error);
  ASSERT_TRUE(model.has_value()) << error;
  model = EditorParameterCatalog::ToModelJson("odt", {{"halation", 2.0}}, *model, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->at("odt").at("film_print").at("scene_light"), "daylight");

  // A print has only Rec. 709 SDR and no OpenDRT settings, and a scan setting needs method
  // film_print.
  const auto print = EditorParameterCatalog::ToModelJson("odt", {{"print_material", "kodak_2383"}},
                                                         *model, &error);
  ASSERT_TRUE(print.has_value()) << error;
  EXPECT_EQ(print->at("odt").at("encoding_space"), "rec709");
  EXPECT_FALSE(print->at("odt").contains("open_drt"));
  EXPECT_EQ(print->at("odt").at("film_print").at("scene_light"), "daylight");
  // A print keeps the scan settings of the document, which only Negative Scan reads.
  EXPECT_EQ(print->at("odt").at("film_print").at("auto"), "neutral");
  // Selecting Negative Scan again on the print under daylight selects As Shot in the same write.
  const auto rescan = EditorParameterCatalog::ToModelJson(
      "odt", {{"print_material", "negative_scan"}}, *print, &error);
  ASSERT_TRUE(rescan.has_value()) << error;
  EXPECT_EQ(rescan->at("odt").at("film_print").at("scene_light"), "as_shot");
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson("odt", {{"look_preset", "umbra"}}, *print, &error)
          .has_value());
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "odt", {{"method", "open_drt"}, {"black", 0.5}}, nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "odt.black needs method film_print");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "odt", {{"method", "film_print"}, {"auto", "vivid"}}, nlohmann::json{}, &error)
                   .has_value());
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "odt", {{"method", "film_print"}, {"scan_contrast", 2.0}}, nlohmann::json{},
                   &error)
                   .has_value());
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "odt", {{"method", "film_print"}, {"scanner_light", 1}}, nlohmann::json{},
                   &error)
                   .has_value());
}

TEST(EditorParameterCatalogTest, FilmPrintWriteHoldsItsFieldsAndRec709SdrOutput) {
  std::string          error;
  // A Display P3 document, and an HDR document: the panel has no encoding controls.
  const nlohmann::json p3    = {{"odt",
                                 {{"method", "open_drt"},
                                  {"encoding_space", "p3_d65"},
                                  {"encoding_eotf", "srgb_piecewise"}}}};
  auto                 model = EditorParameterCatalog::ToModelJson(
      "odt", {{"method", "film_print"}, {"negative", "kodak_portra_400"}, {"halation", 2.5}}, p3,
      &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(*model, (nlohmann::json{{"odt",
                                     {{"method", "film_print"},
                                      {"encoding_space", "rec709"},
                                      {"encoding_eotf", "srgb_piecewise"},
                                      {"peak_luminance", 100.0},
                                      {"film_print",
                                       {{"negative", "kodak_portra_400"},
                                        {"print_material", "kodak_endura_premier"},
                                        {"scene_light", "as_shot"},
                                        {"light_spread", 1.0},
                                        {"halation", 2.5},
                                        {"enlarger_flare", 0.01},
                                        {"auto", "neutral"},
                                        {"auto_strength", 0.5},
                                        {"auto_density", 0.0},
                                        {"scan_contrast", 1.0},
                                        {"scan_exposure_stops", 0.0},
                                        {"cyan", 0.0},
                                        {"magenta", 0.0},
                                        {"yellow", 0.0},
                                        {"black", 1.0},
                                        {"scanner_red_nm", 650.0},
                                        {"scanner_green_nm", 545.0},
                                        {"scanner_blue_nm", 450.0},
                                        {"scanner_width_nm", 40.0},
                                        {"scanner_light", "halogen"}}}}}}));
  const nlohmann::json hdr = {{"odt",
                               {{"method", "aces_2_0"},
                                {"encoding_space", "rec2020"},
                                {"encoding_eotf", "st2084"},
                                {"peak_luminance", 1000.0}}}};
  model = EditorParameterCatalog::ToModelJson("odt", {{"method", "film_print"}}, hdr, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->at("odt").at("encoding_space"), "rec709");
  EXPECT_EQ(model->at("odt").at("encoding_eotf"), "gamma_2_2");
  EXPECT_EQ(model->at("odt").at("peak_luminance"), 100.0);

  // The complete state of the panel, before the echo of the first write reaches it.
  auto state              = EditorParameterCatalog::UiDefault(*EditorParameterCatalog::Find("odt"));
  state["method"]         = "film_print";
  state["encoding_space"] = "rec2020";
  state["encoding_eotf"]  = "hlg";
  state["peak_luminance"] = 1000.0;
  model                   = EditorParameterCatalog::UiStateToModelJson("odt", state, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(model->at("odt").at("encoding_space"), "rec709");
  EXPECT_EQ(model->at("odt").at("encoding_eotf"), "gamma_2_2");
  EXPECT_EQ(model->at("odt").at("peak_luminance"), 100.0);
  EXPECT_FALSE(model->at("odt").contains("open_drt"));
  EXPECT_FALSE(model->at("odt").contains("limiting_space"));

  // Another method writes no film print fields.
  state["method"] = "open_drt";
  model           = EditorParameterCatalog::UiStateToModelJson("odt", state, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_FALSE(model->at("odt").contains("film_print"));
}

TEST(EditorParameterCatalogTest, FilmPrintRejectsAnotherOutputAndFieldsOfOtherMethods) {
  std::string error;
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson(
          "odt", {{"method", "film_print"}, {"encoding_space", "p3_d65"}}, nlohmann::json{}, &error)
          .has_value());
  EXPECT_EQ(error, "odt.encoding_space must be rec709 with method film_print");
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson(
          "odt", {{"method", "film_print"}, {"encoding_eotf", "st2084"}}, nlohmann::json{}, &error)
          .has_value());
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson(
          "odt", {{"method", "film_print"}, {"peak_luminance", 1000.0}}, nlohmann::json{}, &error)
          .has_value());
  EXPECT_EQ(error, "odt.peak_luminance must be 100 with method film_print");
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson("odt", {{"halation", 2.0}}, nlohmann::json{}, &error)
          .has_value());
  EXPECT_EQ(error, "odt.halation needs method film_print");
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson(
          "odt", {{"method", "film_print"}, {"negative", "no_such_film"}}, nlohmann::json{}, &error)
          .has_value());
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson("odt", {{"method", "film_print"}, {"light_spread", 3.5}},
                                          nlohmann::json{}, &error)
          .has_value());
}

TEST(EditorParameterCatalogTest, FilmPrintStoredFlareLimitReadsInsideTheUiRange) {
  // The Model stores a float: 0.1f is 0.100000001 after it widens. A later write of the panel
  // sends this value back, so the read must stay inside the range of the write.
  std::string          error;
  const nlohmann::json stored = {
      {"odt",
       {{"method", "film_print"}, {"film_print", {{"enlarger_flare", kFilmEnlargerFlareMax}}}}}};
  const auto ui = EditorParameterCatalog::ToUiValue("odt", stored, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_LE(ui->at("enlarger_flare").get<double>(), 0.1);
  EXPECT_TRUE(EditorParameterCatalog::ToModelJson("odt", *ui, stored, &error).has_value()) << error;
}

// Issue #718: the Film Grain rule of a method change, which the Display Transform panel and
// editor.set both call.
TEST(EditorParameterCatalogTest, MethodChangeToFilmPrintTurnsOnFilmGrainOnlyWhenItIsZero) {
  const auto grain =
      EditorParameterCatalog::FilmGrainAfterMethodChange("open_drt", "film_print", 0.0);
  ASSERT_TRUE(grain.has_value());
  EXPECT_DOUBLE_EQ(*grain, 100.0);
  EXPECT_DOUBLE_EQ(*grain, EditorParameterCatalog::kFilmPrintFilmGrainUi);
  EXPECT_DOUBLE_EQ(
      EditorParameterCatalog::FilmGrainAfterMethodChange("aces_2_0", "film_print", 0.0).value(),
      100.0);

  // A stored strength above 0 stays.
  EXPECT_FALSE(EditorParameterCatalog::FilmGrainAfterMethodChange("open_drt", "film_print", 40.0));
}

TEST(EditorParameterCatalogTest, MethodChangeFromFilmPrintSetsFilmGrainToZero) {
  const auto grain =
      EditorParameterCatalog::FilmGrainAfterMethodChange("film_print", "open_drt", 40.0);
  ASSERT_TRUE(grain.has_value());
  EXPECT_DOUBLE_EQ(*grain, 0.0);
  EXPECT_DOUBLE_EQ(
      EditorParameterCatalog::FilmGrainAfterMethodChange("film_print", "aces_2_0", 100.0).value(),
      0.0);
  // Film Grain 0 already agrees with the rule.
  EXPECT_FALSE(EditorParameterCatalog::FilmGrainAfterMethodChange("film_print", "open_drt", 0.0));
}

TEST(EditorParameterCatalogTest, WriteThatKeepsTheMethodWritesNoFilmGrain) {
  EXPECT_FALSE(EditorParameterCatalog::FilmGrainAfterMethodChange("film_print", "film_print", 0.0));
  EXPECT_FALSE(EditorParameterCatalog::FilmGrainAfterMethodChange("open_drt", "open_drt", 40.0));
  EXPECT_FALSE(EditorParameterCatalog::FilmGrainAfterMethodChange("open_drt", "aces_2_0", 40.0));
}

TEST(EditorParameterCatalogTest, ColorTempKelvinRoundTripsThroughSliderPivot) {
  for (const double kelvin : {2000.0, 6000.0, 15000.0}) {
    const int position = EditorParameterCatalog::KelvinToSliderPosition(kelvin);
    EXPECT_DOUBLE_EQ(EditorParameterCatalog::SliderPositionToKelvin(position), kelvin) << kelvin;
  }
  EXPECT_EQ(EditorParameterCatalog::KelvinToSliderPosition(2000.0), 0);
  EXPECT_EQ(EditorParameterCatalog::KelvinToSliderPosition(6000.0), 2048);
  EXPECT_EQ(EditorParameterCatalog::KelvinToSliderPosition(15000.0), 4096);

  // The UI value is Kelvin, not the slider position.
  const auto* entry = EditorParameterCatalog::Find("color_temp");
  ASSERT_NE(entry, nullptr);
  const auto json   = EditorParameterCatalog::EntryJson(*entry);
  const auto kelvin = json.at("properties").at(1);
  EXPECT_EQ(kelvin.at("name"), "kelvin");
  EXPECT_DOUBLE_EQ(kelvin.at("ui_min").get<double>(), 2000.0);
  EXPECT_DOUBLE_EQ(kelvin.at("ui_max").get<double>(), 15000.0);
  EXPECT_EQ(kelvin.at("ui_slider").at("position_max"), 4096);
  EXPECT_EQ(kelvin.at("ui_slider").at("pivot_position"), 2048);

  // editor.set color_temp {"kelvin": 5200, "tint": 8}: a Kelvin edit selects custom mode.
  std::string error;
  const auto  model = EditorParameterCatalog::ToModelJson(
      "color_temp", {{"kelvin", 5200}, {"tint", 8}}, nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(*model, (nlohmann::json{
                        {"color_temp", {{"mode", "custom"}, {"custom_cct", 5200}, {"custom_tint", 8}}}}));
  const auto write = ParseEditorParameterWrite("color_temp", *model, &error);
  ASSERT_TRUE(write.has_value()) << error;
  const auto* update = std::get_if<DevelopColorTemperatureUpdate>(&*write);
  ASSERT_NE(update, nullptr);
  EXPECT_EQ(update->wb_mode, std::optional<std::string>{"custom"});
  EXPECT_FLOAT_EQ(update->custom_cct.value_or(0.0f), 5200.0f);
  EXPECT_FLOAT_EQ(update->custom_tint.value_or(0.0f), 8.0f);

  // Out of the Kelvin range, and a Kelvin edit together with As Shot.
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("color_temp", {{"kelvin", 1500}},
                                                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "color_temp.kelvin must be in [2000, 15000]");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "color_temp", {{"mode", "as_shot"}, {"kelvin", 5200}}, nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "color_temp.kelvin and tint need mode custom");

  // The projection shows the recorded pair in As Shot mode and the custom pair in custom mode.
  const nlohmann::json projection = {{"color_temp",
                                      {{"mode", "as_shot"},
                                       {"custom_cct", 4100.0},
                                       {"custom_tint", -3.0},
                                       {"as_shot_cct", 5600.0},
                                       {"as_shot_tint", 4.0}}}};
  auto ui = EditorParameterCatalog::ToUiValue("color_temp", projection, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_EQ(*ui, (nlohmann::json{{"mode", "as_shot"}, {"kelvin", 5600.0}, {"tint", 4.0}}));
  auto custom                          = projection;
  custom["color_temp"]["mode"]         = "custom";
  ui                                   = EditorParameterCatalog::ToUiValue("color_temp", custom, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_EQ(*ui, (nlohmann::json{{"mode", "custom"}, {"kelvin", 4100.0}, {"tint", -3.0}}));
  // The panel Custom choice starts from the recorded pair; As Shot writes the mode only.
  const auto to_custom =
      EditorParameterCatalog::ToModelJson("color_temp", {{"mode", "custom"}}, projection, &error);
  ASSERT_TRUE(to_custom.has_value()) << error;
  EXPECT_EQ(to_custom->at("color_temp").at("custom_cct"), 5600.0);
  const auto to_as_shot =
      EditorParameterCatalog::ToModelJson("color_temp", {{"mode", "as_shot"}}, custom, &error);
  ASSERT_TRUE(to_as_shot.has_value()) << error;
  EXPECT_EQ(*to_as_shot, (nlohmann::json{{"color_temp", {{"mode", "as_shot"}}}}));
}

TEST(EditorParameterCatalogTest, GradeWhiteBalanceDefaultIsTheCat02Identity) {
  const auto* entry = EditorParameterCatalog::Find("grade_white_balance");
  ASSERT_NE(entry, nullptr);
  const auto ui_default = EditorParameterCatalog::UiDefault(*entry);
  EXPECT_DOUBLE_EQ(ui_default.at("temperature").get<double>(),
                   static_cast<double>(kCat02DefaultTemperature));
  EXPECT_DOUBLE_EQ(ui_default.at("tint").get<double>(), static_cast<double>(kCat02DefaultTint));

  std::string error;
  const auto  model = EditorParameterCatalog::ToModelJson(
      "grade_white_balance", {{"temperature", 4800}}, nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  const auto write = ParseEditorParameterWrite("grade_white_balance", *model, &error);
  ASSERT_TRUE(write.has_value()) << error;
  const auto* update = std::get_if<Cat02WhiteBalanceUpdate>(&*write);
  ASSERT_NE(update, nullptr);
  EXPECT_FLOAT_EQ(update->temperature.value_or(0.0f), 4800.0f);
  EXPECT_FLOAT_EQ(update->tint.value_or(0.0f), kCat02DefaultTint);

  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("grade_white_balance", {{"tint", 151}},
                                                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "grade_white_balance.tint must be in [-150, 150]");
}

TEST(EditorParameterCatalogTest, HlsUiScaleMatchesModuleConstant) {
  // Lightness 20 and chroma -40 on the 90 degree bin; the Model value is UI / 1000.
  nlohmann::json lightness = nlohmann::json::array({0, 0, 20, 0, 0, 0, 0, 0});
  nlohmann::json chroma    = nlohmann::json::array({0, 0, -40, 0, 0, 0, 0, 0});
  std::string    error;
  const auto     model = EditorParameterCatalog::ToModelJson(
      "hls", {{"hue", 90}, {"lightness", lightness}, {"chroma", chroma}}, nlohmann::json{},
      &error);
  ASSERT_TRUE(model.has_value()) << error;
  const auto& table = model->at("HLS").at("hls_adj_table");
  EXPECT_DOUBLE_EQ(table.at(2).at(1).get<double>(), 20.0 / ui::hls::kAdjUiToParamScale);
  EXPECT_DOUBLE_EQ(table.at(2).at(2).get<double>(), -40.0 / ui::hls::kAdjUiToParamScale);
  EXPECT_DOUBLE_EQ(model->at("HLS").at("target_hls").at(0).get<double>(), 90.0);

  const auto write = ParseEditorParameterWrite("hls", *model, &error);
  ASSERT_TRUE(write.has_value()) << error;
  const auto* update = std::get_if<HlsUpdate>(&*write);
  ASSERT_NE(update, nullptr);
  ASSERT_TRUE(update->hls_adj_table.has_value());
  EXPECT_FLOAT_EQ(update->hls_adj_table->at(2).l, 0.02f);
  EXPECT_FLOAT_EQ(update->hls_adj_table->at(2).s, -0.04f);
  ASSERT_TRUE(update->hls_adj.has_value());
  EXPECT_FLOAT_EQ(update->hls_adj->l, 0.02f);
  ASSERT_TRUE(update->h_range.has_value());
  EXPECT_FLOAT_EQ(*update->h_range, ui::hls::kDefaultHueRange);

  // The Model tables read back in UI units; the HLS alias names the same entry.
  const auto ui = EditorParameterCatalog::ToUiValue("HLS", *model, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_NEAR(ui->at("lightness").at(2).get<double>(), 20.0, 1e-6);
  EXPECT_NEAR(ui->at("chroma").at(2).get<double>(), -40.0, 1e-6);
  EXPECT_DOUBLE_EQ(ui->at("hue").get<double>(), 90.0);

  // Panel ranges, and a hue that is not a bin.
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "hls", {{"lightness", nlohmann::json::array({0, 0, 120, 0, 0, 0, 0, 0})}},
                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "hls.lightness must be a list of 8 numbers in [-100, 100]");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "hls", {{"hue_shift", nlohmann::json::array({31, 0, 0, 0, 0, 0, 0, 0})}},
                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "hls.hue_shift must be a list of 8 numbers in [-30, 30]");
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson("hls", {{"hue", 30}}, nlohmann::json{}, &error)
          .has_value());
  EXPECT_EQ(error, "hls.hue must be one of: 0, 45, 90, 135, 180, 225, 270, 315");
}

TEST(EditorParameterCatalogTest, ColorWheelUiMatchesTrackballWrite) {
  std::string error;
  const auto  model = EditorParameterCatalog::ToModelJson(
      "color_wheel",
      {{"lift_x", 0.5}, {"lift_y", 0.0}, {"gamma_master", 0.75}, {"offset_master", -0.5}},
      nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  const auto& lift   = model->at("color_wheel").at("lift");
  const auto& gamma  = model->at("color_wheel").at("gamma");
  const auto& gain   = model->at("color_wheel").at("gain");
  const auto& offset = model->at("color_wheel").at("offset");
  // Lift: the per-channel disc stops at the panel strength.
  const auto  stops  = color_wheel::DiscToChannelStops(0.5f, 0.0f, color_wheel::kDiscMaxStops);
  EXPECT_FLOAT_EQ(lift.at("color_offset").at("x").get<float>(), stops[0]);
  EXPECT_FLOAT_EQ(lift.at("strength").get<float>(), color_wheel::kDiscMaxStops);
  // Master values are stops without a sign change; the neutral value of every wheel is 0.
  EXPECT_DOUBLE_EQ(gamma.at("luminance_offset").get<double>(), 0.75);
  EXPECT_DOUBLE_EQ(offset.at("luminance_offset").get<double>(), -0.5);
  EXPECT_FLOAT_EQ(gain.at("color_offset").at("y").get<float>(), 0.0f);

  const auto write = ParseEditorParameterWrite("color_wheel", *model, &error);
  ASSERT_TRUE(write.has_value()) << error;
  const auto ui = EditorParameterCatalog::ToUiValue("color_wheel", *model, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_DOUBLE_EQ(ui->at("gamma_master").get<double>(), 0.75);
  EXPECT_DOUBLE_EQ(ui->at("offset_master").get<double>(), -0.5);
  EXPECT_DOUBLE_EQ(ui->at("lift_x").get<double>(), 0.5);

  // Ranges of the panel, and a disc point outside the wheel.
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson("color_wheel", {{"gain_master", 2.5}},
                                                   nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error, "color_wheel.gain_master must be in [-2, 2]");
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "color_wheel", {{"gain_x", 0.8}, {"gain_y", 0.8}}, nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error,
            "color_wheel.gain_x and gain_y must be inside the unit disc (x * x + y * y <= 1)");
}

TEST(EditorParameterCatalogTest, ColorWheelWriteWithAnUnknownVersionIsRejected) {
  std::string error;
  EXPECT_FALSE(ParseEditorParameterWrite("color_wheel", {{"color_wheel", {{"version", 3}}}}, &error)
                   .has_value());
  EXPECT_NE(error.find("color_wheel.version must be 1 or 2"), std::string::npos) << error;
}

TEST(EditorParameterCatalogTest, DefaultHlsAndColorWheelModelsConvertBackToTheSameModelJson) {
  // Writing the UI value of an unedited Model back must not change the hidden Model fields that
  // the UI value does not show (strength, target_hls, l_range, s_range), so it makes no commit.
  {
    HlsModel             model;
    const nlohmann::json model_json = model.ToJson();
    std::string          error;
    const auto           ui = EditorParameterCatalog::ToUiValue("hls", model_json, &error);
    ASSERT_TRUE(ui.has_value()) << error;
    const auto written = EditorParameterCatalog::ToModelJson("hls", *ui, model_json, &error);
    ASSERT_TRUE(written.has_value()) << error;
    EXPECT_EQ(written->at("HLS"), model_json);
    const auto write = ParseEditorParameterWrite("hls", *written, &error);
    ASSERT_TRUE(write.has_value()) << error;
    const auto* update = std::get_if<HlsUpdate>(&*write);
    ASSERT_NE(update, nullptr);
    model.ApplyUpdate(*update);
    EXPECT_EQ(model.ToJson(), model_json);
  }
  {
    ColorWheelModel      model;
    const nlohmann::json model_json = model.ToJson();
    std::string          error;
    const auto           ui = EditorParameterCatalog::ToUiValue("color_wheel", model_json, &error);
    ASSERT_TRUE(ui.has_value()) << error;
    const auto written =
        EditorParameterCatalog::ToModelJson("color_wheel", *ui, model_json, &error);
    ASSERT_TRUE(written.has_value()) << error;
    EXPECT_EQ(written->at("color_wheel"), model_json);
    const auto write = ParseEditorParameterWrite("color_wheel", *written, &error);
    ASSERT_TRUE(write.has_value()) << error;
    const auto* update = std::get_if<ColorWheelUpdate>(&*write);
    ASSERT_NE(update, nullptr);
    model.ApplyUpdate(*update);
    EXPECT_EQ(model.ToJson(), model_json);
  }
}

TEST(EditorParameterCatalogTest, CurvePointsOutsideTheUnitSquareOrOutOfOrderAreRejected) {
  const auto curve_error = [](const nlohmann::json& points) {
    std::string error;
    EXPECT_FALSE(
        ParseEditorParameterWrite("curve", {{"curve", {{"points", points}}}}, &error).has_value())
        << points.dump();
    return error;
  };
  EXPECT_EQ(curve_error({{{"x", 0.0}, {"y", 0.0}}, {{"x", 1.2}, {"y", 1.0}}}),
            "curve.points[1].x must be in [0, 1]");
  EXPECT_EQ(curve_error({{{"x", 0.0}, {"y", -0.5}}, {{"x", 1.0}, {"y", 1.0}}}),
            "curve.points[0].y must be in [0, 1]");
  EXPECT_EQ(curve_error({{{"x", -0.1}, {"y", 0.0}}, {{"x", 1.0}, {"y", 1.0}}}),
            "curve.points[0].x must be in [0, 1]");
  EXPECT_EQ(curve_error({{{"x", 0.0}, {"y", 0.0}}, {{"x", 1.0}, {"y", 1.5}}}),
            "curve.points[1].y must be in [0, 1]");
  EXPECT_EQ(curve_error({{{"x", 0.0}, {"y", 0.0}},
                         {{"x", 0.6}, {"y", 0.5}},
                         {{"x", 0.4}, {"y", 0.6}},
                         {{"x", 1.0}, {"y", 1.0}}}),
            "curve.points[2].x must be greater than curve.points[1].x");
  EXPECT_EQ(curve_error({{{"x", 0.0}, {"y", 0.0}}, {{"x", 0.0}, {"y", 1.0}}}),
            "curve.points[1].x must be greater than curve.points[0].x");

  // The corners of the unit square are valid points.
  std::string error;
  EXPECT_TRUE(ParseEditorParameterWrite(
                  "curve", {{"curve", {{"points", {{{"x", 0.0}, {"y", 1.0}}, {{"x", 1.0}, {"y", 0.0}}}}}}},
                  &error)
                  .has_value())
      << error;
}

TEST(EditorParameterCatalogTest, CurveWriteAcceptsSixteenPointsAndRejectsSeventeen) {
  const auto evenly_spaced = [](int count) {
    nlohmann::json points = nlohmann::json::array();
    for (int i = 0; i < count; ++i) {
      const double x = static_cast<double>(i) / static_cast<double>(count - 1);
      points.push_back({{"x", x}, {"y", x}});
    }
    return nlohmann::json{{"curve", {{"points", std::move(points)}}}};
  };
  std::string error;
  const auto  sixteen = ParseEditorParameterWrite("curve", evenly_spaced(16), &error);
  ASSERT_TRUE(sixteen.has_value()) << error;
  const auto* write = std::get_if<EditorCurveWrite>(&*sixteen);
  ASSERT_NE(write, nullptr);
  ASSERT_TRUE(write->points.has_value());
  EXPECT_EQ(write->points->size(), 16U);

  EXPECT_FALSE(ParseEditorParameterWrite("curve", evenly_spaced(17), &error).has_value());
  EXPECT_EQ(error, "curve.points must contain at most 16 points");
}

TEST(EditorParameterCatalogTest, CurveWriteWithPointsOnlyLeavesTheChannelCurvesAsTheyAre) {
  // The alcedo-cli form from before the channel curves: {"curve":{"size":N,"points":[...]}}.
  const nlohmann::json params = {
      {"curve",
       {{"size", 3},
        {"points", {{{"x", 0.0}, {"y", 0.0}}, {{"x", 0.5}, {"y", 0.6}}, {{"x", 1.0}, {"y", 1.0}}}}}}};
  std::string error;
  const auto  parsed = ParseEditorParameterWrite("curve", params, &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  const auto* write = std::get_if<EditorCurveWrite>(&*parsed);
  ASSERT_NE(write, nullptr);
  ASSERT_TRUE(write->points.has_value());
  EXPECT_EQ(write->points->size(), 3U);
  EXPECT_FALSE(write->red.has_value());
  EXPECT_FALSE(write->green.has_value());
  EXPECT_FALSE(write->blue.has_value());

  CurveModel  model;
  CurveUpdate blue;
  blue.blue = std::vector<CurvePoint>{{0.0f, 0.1f}, {1.0f, 0.9f}};
  model.ApplyUpdate(blue);
  model.ApplyUpdate(*write);
  EXPECT_EQ(model.Points().size(), 3U);
  EXPECT_EQ(model.ChannelPoints(CurveChannel::Blue), *blue.blue);
}

TEST(EditorParameterCatalogTest, CurveWriteWithOneChannelKeySetsOnlyThatCurve) {
  std::string error;
  const auto  parsed = ParseEditorParameterWrite(
      "curve", {{"curve", {{"blue", {{{"x", 0.0}, {"y", 0.1}}, {{"x", 1.0}, {"y", 0.9}}}}}}},
      &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  const auto* write = std::get_if<EditorCurveWrite>(&*parsed);
  ASSERT_NE(write, nullptr);
  EXPECT_FALSE(write->points.has_value());
  EXPECT_FALSE(write->red.has_value());
  EXPECT_FALSE(write->green.has_value());
  ASSERT_TRUE(write->blue.has_value());
  EXPECT_EQ(*write->blue, (std::vector<CurvePoint>{{0.0f, 0.1f}, {1.0f, 0.9f}}));

  CurveModel  model;
  CurveUpdate rgb;
  rgb.points = std::vector<CurvePoint>{{0.0f, 0.0f}, {0.4f, 0.5f}, {1.0f, 1.0f}};
  model.ApplyUpdate(rgb);
  model.ApplyUpdate(*write);
  EXPECT_EQ(model.Points(), *rgb.points);
  EXPECT_EQ(model.ChannelPoints(CurveChannel::Blue), *write->blue);
}

TEST(EditorParameterCatalogTest, CurveWriteRejectsUnknownKeysAndInvalidChannelPointsWithNamedErrors) {
  std::string error;
  EXPECT_FALSE(ParseEditorParameterWrite(
                   "curve",
                   {{"curve", {{"alpha", {{{"x", 0.0}, {"y", 0.0}}, {{"x", 1.0}, {"y", 1.0}}}}}}},
                   &error)
                   .has_value());
  EXPECT_EQ(error, "Unknown parameter: curve.alpha");

  EXPECT_FALSE(ParseEditorParameterWrite("curve", {{"curve", nlohmann::json::object()}}, &error)
                   .has_value());
  EXPECT_EQ(error, "curve requires points, red, green, or blue");

  EXPECT_FALSE(ParseEditorParameterWrite(
                   "curve", {{"curve", {{"red", {{{"x", 0.0}, {"y", 0.0}}, {{"x", 1.2}, {"y", 1.0}}}}}}},
                   &error)
                   .has_value());
  EXPECT_EQ(error, "curve.red[1].x must be in [0, 1]");

  EXPECT_FALSE(
      ParseEditorParameterWrite("curve", {{"curve", {{"green", {{{"x", 0.0}, {"y", 0.0}}}}}}}, &error)
          .has_value());
  EXPECT_EQ(error, "curve.green must contain at least two points");

  // size counts the RGB points only.
  EXPECT_FALSE(ParseEditorParameterWrite(
                   "curve",
                   {{"curve",
                     {{"size", 2}, {"blue", {{{"x", 0.0}, {"y", 0.0}}, {{"x", 1.0}, {"y", 1.0}}}}}}},
                   &error)
                   .has_value());
  EXPECT_EQ(error, "curve.size must match curve.points");
}

TEST(EditorParameterCatalogTest, EachToneCurvePresetPassesTheCurveWrite) {
  // The points of each preset as a point list, the shape that the panel writes.
  for (const auto& preset : ToneCurvePresets()) {
    nlohmann::json points = nlohmann::json::array();
    for (const auto& point : preset.points) {
      points.push_back({{"x", point.x}, {"y", point.y}});
    }
    std::string error;
    const auto  parsed =
        ParseEditorParameterWrite("curve", {{"curve", {{"red", std::move(points)}}}}, &error);
    ASSERT_TRUE(parsed.has_value()) << preset.id << ": " << error;
    EXPECT_EQ(*std::get_if<EditorCurveWrite>(&*parsed)->red, preset.PointList()) << preset.id;
  }
}

TEST(EditorParameterCatalogTest, CurveWriteAppliesAPresetById) {
  std::string error;
  const auto  parsed = ParseEditorParameterWrite(
      "curve", {{"curve", {{"points", "film_like"}, {"blue", "kodak_2383"}}}}, &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  const auto* write = std::get_if<EditorCurveWrite>(&*parsed);
  ASSERT_NE(write, nullptr);
  ASSERT_TRUE(write->points.has_value());
  EXPECT_EQ(*write->points, FindToneCurvePreset("film_like")->PointList());
  EXPECT_FALSE(write->red.has_value());
  ASSERT_TRUE(write->blue.has_value());
  EXPECT_EQ(*write->blue, FindToneCurvePreset("kodak_2383")->PointList());

  EXPECT_FALSE(
      ParseEditorParameterWrite("curve", {{"curve", {{"green", "velvia"}}}}, &error).has_value());
  EXPECT_EQ(error, "curve.green names no Tone Curve preset: velvia; curve.presets lists them");
  // A stored value holds points only.
  EXPECT_FALSE(ParseEditorParameterValue("curve", {{"points", "film_like"}}, &error).has_value());
}

TEST(EditorParameterCatalogTest, CurveValueWithoutAChannelKeyReadsThatChannelAsIdentity) {
  // A complete stored value (history replay) is the Model JSON, which omits an identity channel
  // curve: the value sets that channel to the identity.
  std::string error;
  const auto  parsed = ParseEditorParameterValue(
      "curve", {{"points", {{{"x", 0.0}, {"y", 0.0}}, {{"x", 1.0}, {"y", 1.0}}}}}, &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  const auto* write = std::get_if<EditorCurveWrite>(&*parsed);
  ASSERT_NE(write, nullptr);
  for (const auto channel : kCurveChannels) {
    ASSERT_TRUE(write->Channel(channel).has_value()) << CurveChannelKey(channel);
    EXPECT_EQ(*write->Channel(channel), IdentityCurvePoints()) << CurveChannelKey(channel);
  }
  EXPECT_FALSE(ParseEditorParameterValue(
                   "curve", {{"blue", {{{"x", 0.0}, {"y", 0.0}}, {{"x", 1.0}, {"y", 1.0}}}}}, &error)
                   .has_value());
  EXPECT_EQ(error, "curve.points is required");
}

TEST(EditorParameterCatalogTest, CurveResetValueResetsTheFourCurves) {
  const auto* entry = EditorParameterCatalog::Find("curve");
  ASSERT_NE(entry, nullptr);
  const auto reset = EditorParameterCatalog::UiDefault(*entry);
  for (const auto channel : kCurveChannels) {
    EXPECT_TRUE(reset.contains(std::string{CurveChannelKey(channel)})) << CurveChannelKey(channel);
  }
  std::string error;
  const auto  model_json =
      EditorParameterCatalog::ToModelJson("curve", reset, nlohmann::json{}, &error);
  ASSERT_TRUE(model_json.has_value()) << error;
  const auto parsed = ParseEditorParameterWrite("curve", *model_json, &error);
  ASSERT_TRUE(parsed.has_value()) << error;

  CurveModel  model;
  CurveUpdate edited;
  edited.points = std::vector<CurvePoint>{{0.0f, 0.0f}, {0.4f, 0.5f}, {1.0f, 1.0f}};
  edited.red    = std::vector<CurvePoint>{{0.0f, 0.1f}, {1.0f, 0.9f}};
  model.ApplyUpdate(edited);
  model.ApplyUpdate(std::get<EditorCurveWrite>(*parsed));
  EXPECT_TRUE(model.IsDefault());
}

TEST(EditorParameterCatalogTest, CurveAndLutUseTheModelShape) {
  std::string          error;
  const nlohmann::json curve = {
      {"points", {{{"x", 0.0}, {"y", 0.0}}, {{"x", 0.5}, {"y", 0.6}}, {{"x", 1.0}, {"y", 1.0}}}}};
  const auto model = EditorParameterCatalog::ToModelJson("curve", curve, nlohmann::json{}, &error);
  ASSERT_TRUE(model.has_value()) << error;
  EXPECT_EQ(*model, curve);
  EXPECT_TRUE(ParseEditorParameterWrite("curve", *model, &error).has_value()) << error;
  const auto ui = EditorParameterCatalog::ToUiValue("curve", {{"curve", curve}}, &error);
  ASSERT_TRUE(ui.has_value()) << error;
  EXPECT_EQ(*ui, curve);

  // The ocio_lmt alias names the lut entry; {} is no LUT.
  const auto* lut = EditorParameterCatalog::Find("ocio_lmt");
  ASSERT_NE(lut, nullptr);
  EXPECT_EQ(lut->field, "lut");
  EXPECT_EQ(EditorParameterCatalog::UiDefault(*lut), nlohmann::json::object());
  const auto entry = EditorParameterCatalog::EntryJson(*lut);
  EXPECT_EQ(entry.at("kind"), "model");
  EXPECT_FALSE(entry.at("model_shape").get<std::string>().empty());
  EXPECT_FALSE(
      EditorParameterCatalog::ToModelJson("lut", nlohmann::json(3), nlohmann::json{}, &error)
          .has_value());
  EXPECT_EQ(error.rfind("lut must be its Model JSON: ", 0), 0U) << error;
}

TEST(EditorParameterCatalogTest, LutPanelProjectionConvertsToModelJsonThatParsesAsWrite) {
  const nlohmann::json official = {{"referenceKind", "official"},
                                   {"packageId", "spectral_film_lut"},
                                   {"lutId", "kodak-5207"},
                                   {"ocio_lmt", ""},
                                   {"path", ""},
                                   {"lutName", "Kodak Vision3 250D 5207"},
                                   {"strength", 0.5},
                                   {"input_encoding", "acescc"},
                                   {"output_encoding", "acescct"}};
  const nlohmann::json library  = {{"referenceKind", "library"},
                                   {"libraryPath", "user/look.cube"},
                                   {"ocio_lmt", ""},
                                   {"path", ""},
                                   {"lutName", "look"},
                                   {"strength", 1.0},
                                   {"input_encoding", "acescc"},
                                   {"output_encoding", "acescc"}};
  const nlohmann::json file     = {{"referenceKind", "file"},
                                   {"ocio_lmt", "C:/luts/look.cube"},
                                   {"path", "C:/luts/look.cube"},
                                   {"lutName", ""},
                                   {"strength", 0.25},
                                   {"input_encoding", "acescc"},
                                   {"output_encoding", "acescc"}};
  const nlohmann::json none     = {{"referenceKind", "none"},
                                   {"ocio_lmt", ""},
                                   {"path", ""},
                                   {"lutName", ""},
                                   {"strength", 1.0},
                                   {"input_encoding", "acescc"},
                                   {"output_encoding", "acescc"}};

  const std::array<std::pair<nlohmann::json, nlohmann::json>, 4> cases = {{
      {official,
       {{"reference",
         {{"kind", "official"}, {"package_id", "spectral_film_lut"}, {"lut_id", "kodak-5207"}}},
        {"name", "Kodak Vision3 250D 5207"},
        {"strength", 0.5},
        {"input_encoding", "acescc"},
        {"output_encoding", "acescct"}}},
      {library,
       {{"reference", {{"kind", "library"}, {"path", "user/look.cube"}}},
        {"name", "look"},
        {"strength", 1.0},
        {"input_encoding", "acescc"},
        {"output_encoding", "acescc"}}},
      {file,
       {{"cube_path", "C:/luts/look.cube"},
        {"strength", 0.25},
        {"input_encoding", "acescc"},
        {"output_encoding", "acescc"}}},
      {none, nlohmann::json::object()},
  }};
  for (const auto& [panel, expected] : cases) {
    std::string error;
    const auto  ui = EditorParameterCatalog::ToUiValue("lut", panel, &error);
    ASSERT_TRUE(ui.has_value()) << panel.dump() << ": " << error;
    EXPECT_EQ(*ui, expected) << panel.dump();
    // The UI value is accepted as a write unchanged: editor.get output is editor.set input.
    const auto model = EditorParameterCatalog::ToModelJson("lut", *ui, panel, &error);
    ASSERT_TRUE(model.has_value()) << error;
    EXPECT_TRUE(ParseEditorParameterWrite("lut", *model, &error).has_value())
        << ui->dump() << ": " << error;
  }

  std::string error;
  EXPECT_FALSE(
      EditorParameterCatalog::ToUiValue("lut", {{"referenceKind", "cloud"}}, &error).has_value());
  EXPECT_EQ(error, "lut Model JSON referenceKind 'cloud' is not a LUT reference kind");
}

TEST(EditorParameterCatalogTest, EveryCatalogFieldCoversEveryFieldKey) {
  // The DCP profile reference is an editor field without a catalog entry, so alcedo-cli has no
  // parameter for it (dcp_camera_profile_plan.md, decision P-6).
  EXPECT_EQ(EditorParameterCatalog::Find("camera_profile_file"), nullptr);
  std::set<EditorAdjustmentField> adjustments;
  for (const auto& [key, adjustment] : EditorAdjustmentFieldKeys()) {
    if (adjustment == EditorAdjustmentField::CameraProfileFile) {
      continue;
    }
    const auto* entry = EditorParameterCatalog::Find(key);
    ASSERT_NE(entry, nullptr) << key;
    EXPECT_EQ(entry->adjustment, adjustment) << key;
    EXPECT_TRUE(key == entry->field || key == entry->alias) << key;
    adjustments.insert(adjustment);
  }
  // One entry per adjustment: an alias maps to the entry of its field.
  EXPECT_EQ(adjustments.size(), EditorParameterCatalog::Entries().size());
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    EXPECT_EQ(ResolveEditorAdjustmentField(std::string{entry.field}), entry.adjustment)
        << entry.field;
  }
}

}  // namespace
}  // namespace alcedo
