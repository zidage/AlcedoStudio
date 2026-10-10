//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// EditorParameterCatalog: UI ranges of the editor fields, UI-to-Model conversions, range
// rejection, and the object fields of the RAW Decode and Geometry panels. The converted Model
// JSON must parse through ParseEditorParameterWrite, the parser that history replay and the QML
// collection boundary use.

#include <gtest/gtest.h>

#include <cmath>
#include <json.hpp>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "app/editor_adjustment_context.hpp"
#include "app/editor_adjustment_pipeline.hpp"
#include "app/editor_parameter_catalog.hpp"
#include "app/editor_parameter_write.hpp"
#include "edit/geometry/crop_frame.hpp"
#include "edit/geometry/types.hpp"

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
    if (!IsScalar(entry)) {
      std::string error;
      const auto  model = EditorParameterCatalog::ToModelJson(entry.field, nlohmann::json::object(),
                                                              nlohmann::json{}, &error);
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
    EXPECT_EQ(item.at("kind"), IsScalar(*entry) ? "scalar" : "object");
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
  };
  std::set<std::pair<std::string, std::string>> listed;
  for (const auto& entry : EditorParameterCatalog::Entries()) {
    if (IsScalar(entry)) {
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
  EXPECT_FALSE(EditorParameterCatalog::ToModelJson(
                   "input_profile", {{"profile_override", "aces"}}, nlohmann::json{}, &error)
                   .has_value());
  EXPECT_EQ(error,
            "input_profile.profile_override must be one of: auto, srgb, display_p3, adobe_rgb, "
            "rec2020, prophoto, linear_rec709");
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

}  // namespace
}  // namespace alcedo
