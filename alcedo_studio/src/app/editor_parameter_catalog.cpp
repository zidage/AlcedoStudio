//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_parameter_catalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <sstream>
#include <string>
#include <utility>

#include "app/editor_adjustment_context.hpp"
#include "edit/geometry/crop_frame.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_raster_input.hpp"
#include "ui/alcedo_main/editor_support/modules/geometry.hpp"

namespace alcedo {
namespace {

using Conversion   = EditorScalarUnitConversion;
using PropertyType = EditorParameterPropertyType;
using json         = nlohmann::json;

constexpr EditorScalarUiRange kSignedPercentRange{-100.0, 100.0, 0.0, 1.0, 0};
constexpr EditorScalarUiRange kPercentRange{0.0, 100.0, 0.0, 1.0, 0};

constexpr auto Scalar(std::string_view field, std::string_view alias,
                      EditorAdjustmentField adjustment, std::string_view panel,
                      std::string_view model_key, Conversion conversion, EditorScalarUiRange range)
    -> EditorParameterCatalogEntry {
  return EditorParameterCatalogEntry{field,     alias,      adjustment, EditorParameterValueKind::Scalar,
                                     panel,     model_key,  conversion, range};
}

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

auto FormatNumber(double value) -> std::string {
  std::ostringstream stream;
  stream << value;
  return stream.str();
}

auto RangeText(double minimum, double maximum) -> std::string {
  return "[" + FormatNumber(minimum) + ", " + FormatNumber(maximum) + "]";
}

auto RangeText(const EditorScalarUiRange& range) -> std::string {
  return RangeText(range.minimum, range.maximum);
}

auto KindText(EditorParameterValueKind kind) -> const char* {
  switch (kind) {
    case EditorParameterValueKind::Object:
      return "object";
    case EditorParameterValueKind::Scalar:
    default:
      return "scalar";
  }
}

auto PropertyTypeText(PropertyType type) -> const char* {
  switch (type) {
    case PropertyType::Boolean:
      return "boolean";
    case PropertyType::String:
      return "string";
    case PropertyType::Option:
      return "option";
    case PropertyType::Number:
    default:
      return "number";
  }
}

/// The object under @p key of @p model_json when it holds one, else @p model_json itself. The
/// panel projection wraps object fields (`{"raw": {...}}`); a flat object is also accepted.
auto UnwrapObject(const json& model_json, const char* key) -> const json& {
  if (model_json.is_object()) {
    const auto it = model_json.find(key);
    if (it != model_json.end() && it->is_object()) {
      return *it;
    }
  }
  return model_json;
}

auto ReadNumber(const json& object, const char* key, double fallback, std::string_view field,
                double* out, std::string* error) -> bool {
  const auto it = object.find(key);
  if (it == object.end()) {
    *out = fallback;
    return true;
  }
  if (!it->is_number() || !std::isfinite(it->get<double>())) {
    return SetError(error, std::string{field} + " Model JSON has no finite " + key + " value");
  }
  *out = it->get<double>();
  return true;
}

auto ReadBool(const json& object, const char* key, bool fallback, std::string_view field,
              bool* out, std::string* error) -> bool {
  const auto it = object.find(key);
  if (it == object.end()) {
    *out = fallback;
    return true;
  }
  if (!it->is_boolean()) {
    return SetError(error, std::string{field} + " Model JSON " + key + " is not a boolean");
  }
  *out = it->get<bool>();
  return true;
}

auto ReadString(const json& object, const char* key, const std::string& fallback,
                std::string_view field, std::string* out, std::string* error) -> bool {
  const auto it = object.find(key);
  if (it == object.end()) {
    *out = fallback;
    return true;
  }
  if (!it->is_string()) {
    return SetError(error, std::string{field} + " Model JSON " + key + " is not a string");
  }
  *out = it->get<std::string>();
  return true;
}

auto RequireObject(const json& model_json, std::string_view field, std::string* error) -> bool {
  if (!model_json.is_object()) {
    return SetError(error, std::string{field} + " Model JSON must be an object");
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// raw_decode: demosaic method and highlight reconstruction of a RAW document.

constexpr std::array kRawMethodOptions = {
    EditorParameterOption{"default", "Default"},
    EditorParameterOption{"legacy", "Legacy"},
    EditorParameterOption{"neural_engine", "Neural Engine"},
};

constexpr std::array kRawDecodeProperties = {
    EditorParameterProperty{"method", PropertyType::Option, kRawMethodOptions},
    EditorParameterProperty{"highlights_reconstruct", PropertyType::Boolean},
};

auto RawDecodeUiDefault() -> json {
  return {{"method", "default"}, {"highlights_reconstruct", true}};
}

auto RawDecodeModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& raw = UnwrapObject(model_json, "raw");
  if (!RequireObject(raw, "raw_decode", error)) {
    return std::nullopt;
  }
  std::string method;
  bool        highlights = true;
  if (!ReadString(raw, "method", "default", "raw_decode", &method, error) ||
      !ReadBool(raw, "highlights_reconstruct", true, "raw_decode", &highlights, error)) {
    return std::nullopt;
  }
  return json{{"method", method}, {"highlights_reconstruct", highlights}};
}

auto RawDecodeUiToModel(const json& ui_value) -> json {
  // The white balance and the backend keep the values of a new RAW Develop: the white balance
  // has its own field, and the decode backend is a pipeline setting, not an edit parameter.
  return {{"raw",
           {{"method", ui_value.at("method")},
            {"highlights_reconstruct", ui_value.at("highlights_reconstruct")},
            {"use_camera_wb", true},
            {"user_wb", 7600.0},
            {"backend", "alcedo"}}}};
}

// ---------------------------------------------------------------------------------------------
// input_profile: input profile override of a raster document (decision D5: Auto keeps the
// description read from the file).

constexpr std::array kInputProfileOptions = {
    EditorParameterOption{"auto", "Auto (from file)"},
    EditorParameterOption{"srgb", "sRGB"},
    EditorParameterOption{"display_p3", "Display P3"},
    EditorParameterOption{"adobe_rgb", "Adobe RGB"},
    EditorParameterOption{"rec2020", "Rec.2020"},
    EditorParameterOption{"prophoto", "ProPhoto"},
    EditorParameterOption{"linear_rec709", "Linear Rec.709"},
};

constexpr auto InputProfileOptionsMatchModel() -> bool {
  if (kInputProfileOptions.size() != kRasterInputProfileOverrides.size()) {
    return false;
  }
  for (std::size_t i = 0; i < kInputProfileOptions.size(); ++i) {
    if (kInputProfileOptions[i].value != kRasterInputProfileOverrides[i]) {
      return false;
    }
  }
  return true;
}
static_assert(InputProfileOptionsMatchModel(),
              "input_profile options must be the Develop profile overrides in menu order");

constexpr std::array kInputProfileProperties = {
    EditorParameterProperty{"profile_override", PropertyType::Option, kInputProfileOptions},
};

auto InputProfileUiDefault() -> json { return {{"profile_override", "auto"}}; }

auto InputProfileModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& input = UnwrapObject(model_json, "input");
  if (!RequireObject(input, "input_profile", error)) {
    return std::nullopt;
  }
  std::string profile;
  if (!ReadString(input, "profile_override", "auto", "input_profile", &profile, error)) {
    return std::nullopt;
  }
  return json{{"profile_override", profile}};
}

auto InputProfileUiToModel(const json& ui_value) -> json {
  return {{"profile_override", ui_value.at("profile_override")}};
}

// ---------------------------------------------------------------------------------------------
// lens_calib: lens correction switch and the lens identity.

constexpr std::array kLensProperties = {
    EditorParameterProperty{"enabled", PropertyType::Boolean},
    EditorParameterProperty{"lens_maker", PropertyType::String},
    EditorParameterProperty{"lens_model", PropertyType::String},
};

auto LensUiDefault() -> json {
  const auto defaults = MakeDefaultLensCalibrationWriteJson();
  return {{"enabled", defaults.at("lens_calib").at("enabled")},
          {"lens_maker", ""},
          {"lens_model", ""}};
}

auto LensModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& lens = UnwrapObject(model_json, "lens_calib");
  if (!RequireObject(lens, "lens_calib", error)) {
    return std::nullopt;
  }
  const auto  defaults = LensUiDefault();
  bool        enabled  = defaults.at("enabled").get<bool>();
  std::string maker;
  std::string model;
  if (!ReadBool(lens, "enabled", enabled, "lens_calib", &enabled, error) ||
      !ReadString(lens, "lens_maker", "", "lens_calib", &maker, error) ||
      !ReadString(lens, "lens_model", "", "lens_calib", &model, error)) {
    return std::nullopt;
  }
  return json{{"enabled", enabled}, {"lens_maker", maker}, {"lens_model", model}};
}

auto LensUiToModel(const json& ui_value) -> json {
  auto        model = MakeDefaultLensCalibrationWriteJson();
  auto&       lens  = model.at("lens_calib");
  const auto& maker = ui_value.at("lens_maker").get_ref<const std::string&>();
  lens["enabled"]    = ui_value.at("enabled");
  lens["lens_maker"] = maker;
  // An empty maker means "from metadata"; a model without its maker does not name a lens.
  lens["lens_model"] = maker.empty() ? std::string{} : ui_value.at("lens_model").get<std::string>();
  return model;
}

auto LensConstrain(const json& merged_ui, const json& ui_change,
                   const EditorParameterSource& source, std::string* error)
    -> std::optional<json> {
  static_cast<void>(source);
  if (ui_change.contains("lens_model") && !ui_change.at("lens_model").get_ref<const std::string&>().empty() &&
      merged_ui.at("lens_maker").get_ref<const std::string&>().empty()) {
    SetError(error, "lens_calib.lens_model needs a lens_maker");
    return std::nullopt;
  }
  return merged_ui;
}

// ---------------------------------------------------------------------------------------------
// crop_rotate: crop frame in source-normalized coordinates, rotation, and aspect lock.

constexpr double kCropPositionLimit  = 1.0;
constexpr double kCropAspectSizeMax  = 100.0;
constexpr double kCropRotationLimit  = 180.0;
constexpr double kCropOrientationEps = 1e-4;
constexpr double kCropRectStep       = 0.001;
constexpr int    kCropRectDecimals   = 3;
// The geometry module minimum (float) as the exact panel value 0.0001.
constexpr double kCropSizeMin        = 1e-4;
static_assert(static_cast<float>(kCropSizeMin) == ui::geometry::kCropRectMinSize &&
              static_cast<float>(kCropSizeMin) == ui::geometry::kCropAspectMinValue);

auto CropAspectOptions() -> const std::array<EditorParameterOption, 12>& {
  static const auto options = [] {
    std::array<EditorParameterOption, 12> result{};
    const auto&                           presets = ui::geometry::CropAspectPresetOptions();
    for (std::size_t i = 0; i < presets.size(); ++i) {
      result[i] = EditorParameterOption{presets[i].id_, presets[i].label_};
    }
    return result;
  }();
  return options;
}

auto CropProperties() -> const std::array<EditorParameterProperty, 8>& {
  static const std::array<EditorParameterProperty, 8> properties = {
      // With a rotation the unrotated rectangle of the frame may start left of or above the
      // source; the rotated-source constraint decides the valid position.
      EditorParameterProperty{"x", PropertyType::Number, {}, -kCropPositionLimit,
                              kCropPositionLimit, kCropRectStep, kCropRectDecimals},
      EditorParameterProperty{"y", PropertyType::Number, {}, -kCropPositionLimit,
                              kCropPositionLimit, kCropRectStep, kCropRectDecimals},
      EditorParameterProperty{"width", PropertyType::Number, {}, kCropSizeMin,
                              1.0, kCropRectStep, kCropRectDecimals},
      EditorParameterProperty{"height", PropertyType::Number, {}, kCropSizeMin,
                              1.0, kCropRectStep, kCropRectDecimals},
      EditorParameterProperty{"angle_degrees", PropertyType::Number, {}, -kCropRotationLimit,
                              kCropRotationLimit, 0.1, 1},
      EditorParameterProperty{"aspect_preset", PropertyType::Option, CropAspectOptions()},
      EditorParameterProperty{"aspect_width", PropertyType::Number, {},
                              kCropSizeMin, kCropAspectSizeMax, 0.01, 2},
      EditorParameterProperty{"aspect_height", PropertyType::Number, {},
                              kCropSizeMin, kCropAspectSizeMax, 0.01, 2},
  };
  return properties;
}

auto CropUiDefault() -> json {
  return {{"x", 0.0},
          {"y", 0.0},
          {"width", 1.0},
          {"height", 1.0},
          {"angle_degrees", 0.0},
          {"aspect_preset", "free"},
          {"aspect_width", 1.0},
          {"aspect_height", 1.0}};
}

auto CropModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& crop = UnwrapObject(model_json, "crop_rotate");
  if (!RequireObject(crop, "crop_rotate", error)) {
    return std::nullopt;
  }
  static const json kEmpty = json::object();
  const auto        rect_it   = crop.find("crop_rect");
  const auto        aspect_it = crop.find("aspect_ratio");
  const json&       rect      = rect_it != crop.end() ? *rect_it : kEmpty;
  const json&       aspect    = aspect_it != crop.end() ? *aspect_it : kEmpty;
  if (!RequireObject(rect, "crop_rotate.crop_rect", error) ||
      !RequireObject(aspect, "crop_rotate.aspect_ratio", error)) {
    return std::nullopt;
  }
  double      x = 0.0, y = 0.0, w = 1.0, h = 1.0, angle = 0.0, aspect_w = 1.0, aspect_h = 1.0;
  std::string preset;
  if (!ReadNumber(rect, "x", 0.0, "crop_rotate", &x, error) ||
      !ReadNumber(rect, "y", 0.0, "crop_rotate", &y, error) ||
      !ReadNumber(rect, "w", 1.0, "crop_rotate", &w, error) ||
      !ReadNumber(rect, "h", 1.0, "crop_rotate", &h, error) ||
      !ReadNumber(crop, "angle_degrees", 0.0, "crop_rotate", &angle, error) ||
      !ReadString(crop, "aspect_ratio_preset", "free", "crop_rotate", &preset, error) ||
      !ReadNumber(aspect, "width", 1.0, "crop_rotate", &aspect_w, error) ||
      !ReadNumber(aspect, "height", 1.0, "crop_rotate", &aspect_h, error)) {
    return std::nullopt;
  }
  return json{{"x", x},
              {"y", y},
              {"width", w},
              {"height", h},
              {"angle_degrees", angle},
              {"aspect_preset", preset},
              {"aspect_width", aspect_w},
              {"aspect_height", aspect_h}};
}

auto CropUiToModel(const json& ui_value) -> json {
  return {{"crop_rotate",
           {{"crop_rect",
             {{"x", ui_value.at("x")},
              {"y", ui_value.at("y")},
              {"w", ui_value.at("width")},
              {"h", ui_value.at("height")}}},
            {"angle_degrees", ui_value.at("angle_degrees")},
            {"aspect_ratio_preset", ui_value.at("aspect_preset")},
            {"aspect_ratio",
             {{"width", ui_value.at("aspect_width")}, {"height", ui_value.at("aspect_height")}}}}}};
}

auto IsFixedAspectPreset(std::string_view preset) -> bool {
  return preset != "custom";
}

/// The control that a partial crop write changed, in the priority of the panel: an aspect
/// change refits the frame, a size change resizes it, a position or rotation change keeps it.
auto CropDriverFromChange(const json& ui_change) -> EditorCropDriver {
  if (ui_change.contains("aspect_preset")) {
    return EditorCropDriver::AspectPreset;
  }
  if (ui_change.contains("aspect_width") || ui_change.contains("aspect_height")) {
    return EditorCropDriver::AspectSize;
  }
  if (ui_change.contains("width")) {
    return EditorCropDriver::Width;
  }
  if (ui_change.contains("height")) {
    return EditorCropDriver::Height;
  }
  if (ui_change.contains("x") || ui_change.contains("y")) {
    return EditorCropDriver::Position;
  }
  if (ui_change.contains("angle_degrees")) {
    return EditorCropDriver::Rotation;
  }
  return EditorCropDriver::None;
}

auto CropConstrain(const json& merged_ui, const json& ui_change,
                   const EditorParameterSource& source, std::string* error)
    -> std::optional<json> {
  // A fixed preset sets the aspect size to its own ratio, so a fixed preset and an aspect size
  // in one write disagree.
  if (ui_change.contains("aspect_preset") &&
      IsFixedAspectPreset(ui_change.at("aspect_preset").get_ref<const std::string&>()) &&
      (ui_change.contains("aspect_width") || ui_change.contains("aspect_height"))) {
    SetError(error, "crop_rotate.aspect_width and aspect_height need aspect_preset custom");
    return std::nullopt;
  }
  return EditorParameterCatalog::ConstrainCrop(merged_ui, CropDriverFromChange(ui_change), source);
}

/// Source width / height, or 1 when no frame is presented.
auto SourceAspect(const EditorParameterSource& source) -> double {
  if (source.width == 0 || source.height == 0) {
    return 1.0;
  }
  return static_cast<double>(source.width) / static_cast<double>(source.height);
}

// ---------------------------------------------------------------------------------------------

auto ObjectEntry(std::string_view field, EditorAdjustmentField adjustment, std::string_view panel,
                 std::span<const EditorParameterProperty> properties, json (*ui_default)(),
                 std::optional<json> (*model_to_ui)(const json&, std::string*),
                 json (*ui_to_model)(const json&),
                 std::optional<json> (*constrain)(const json&, const json&,
                                                  const EditorParameterSource&, std::string*))
    -> EditorParameterCatalogEntry {
  EditorParameterCatalogEntry entry{field, {}, adjustment, EditorParameterValueKind::Object, panel};
  entry.properties  = properties;
  entry.ui_default  = ui_default;
  entry.model_to_ui = model_to_ui;
  entry.ui_to_model = ui_to_model;
  entry.constrain   = constrain;
  return entry;
}

// Values of the Tone, Look, Post Processing, RAW Decode, and Geometry panels. The exposure
// default is the neutral reset value; the product Default document starts at +1.5 EV.
auto Table() -> const std::array<EditorParameterCatalogEntry, 17>& {
  static const std::array<EditorParameterCatalogEntry, 17> entries = {
      Scalar("exposure", {}, EditorAdjustmentField::Exposure, "tone", "exposure_ev",
             Conversion::Identity, EditorScalarUiRange{-10.0, 10.0, 0.0, 0.01, 2}),
      Scalar("contrast", {}, EditorAdjustmentField::Contrast, "tone", "contrast",
             Conversion::Identity, kSignedPercentRange),
      Scalar("highlights", {}, EditorAdjustmentField::Highlights, "tone", "highlights",
             Conversion::Identity, kSignedPercentRange),
      Scalar("shadows", {}, EditorAdjustmentField::Shadows, "tone", "shadows",
             Conversion::Identity, kSignedPercentRange),
      Scalar("white", "whites", EditorAdjustmentField::Whites, "tone", "white",
             Conversion::Identity, kSignedPercentRange),
      Scalar("black", "blacks", EditorAdjustmentField::Blacks, "tone", "black",
             Conversion::Identity, kSignedPercentRange),
      Scalar("saturation", {}, EditorAdjustmentField::Saturation, "look", "saturation",
             Conversion::OffsetFromOnePercent, kSignedPercentRange),
      Scalar("vibrance", {}, EditorAdjustmentField::Vibrance, "look", "vibrance",
             Conversion::Identity, kSignedPercentRange),
      Scalar("diffusion", {}, EditorAdjustmentField::Diffusion, "post", "strength",
             Conversion::Percent, kPercentRange),
      Scalar("clarity", {}, EditorAdjustmentField::Clarity, "post", "clarity",
             Conversion::Identity, kSignedPercentRange),
      Scalar("sharpen", {}, EditorAdjustmentField::Sharpen, "post", "offset",
             Conversion::Identity, kPercentRange),
      Scalar("film_grain", {}, EditorAdjustmentField::FilmGrain, "post", "strength",
             Conversion::Percent, kPercentRange),
      Scalar("halation", {}, EditorAdjustmentField::Halation, "post", "strength",
             Conversion::Percent, kPercentRange),
      ObjectEntry("raw_decode", EditorAdjustmentField::RawDecode, "raw", kRawDecodeProperties,
                  &RawDecodeUiDefault, &RawDecodeModelToUi, &RawDecodeUiToModel, nullptr),
      ObjectEntry("input_profile", EditorAdjustmentField::InputProfile, "raw",
                  kInputProfileProperties, &InputProfileUiDefault, &InputProfileModelToUi,
                  &InputProfileUiToModel, nullptr),
      ObjectEntry("lens_calib", EditorAdjustmentField::LensCalibration, "raw", kLensProperties,
                  &LensUiDefault, &LensModelToUi, &LensUiToModel, &LensConstrain),
      ObjectEntry("crop_rotate", EditorAdjustmentField::CropRotate, "geometry", CropProperties(),
                  &CropUiDefault, &CropModelToUi, &CropUiToModel, &CropConstrain),
  };
  return entries;
}

/// The scalar Model value in @p model_json, or nullptr. See EditorParameterCatalog::ToUiValue.
auto FindScalarModelValue(const EditorParameterCatalogEntry& entry,
                          const json&                        model_json) -> const json* {
  if (model_json.is_number()) {
    return &model_json;
  }
  if (!model_json.is_object()) {
    return nullptr;
  }
  const std::string model_key{entry.model_key};
  const std::string field{entry.field};
  if (model_json.contains(model_key)) {
    return &model_json.at(model_key);
  }
  if (model_json.contains(field)) {
    const auto& nested = model_json.at(field);
    if (nested.is_object()) {
      return nested.contains(model_key) ? &nested.at(model_key) : nullptr;
    }
    return &nested;
  }
  if (model_json.contains("value")) {
    return &model_json.at("value");
  }
  return nullptr;
}

auto FindProperty(const EditorParameterCatalogEntry& entry, std::string_view name)
    -> const EditorParameterProperty* {
  for (const auto& property : entry.properties) {
    if (property.name == name) {
      return &property;
    }
  }
  return nullptr;
}

auto PropertyNamesText(const EditorParameterCatalogEntry& entry) -> std::string {
  std::string text;
  for (const auto& property : entry.properties) {
    if (!text.empty()) {
      text += ", ";
    }
    text += property.name;
  }
  return text;
}

auto OptionValuesText(const EditorParameterProperty& property) -> std::string {
  std::string text;
  for (const auto& option : property.options) {
    if (!text.empty()) {
      text += ", ";
    }
    text += option.value;
  }
  return text;
}

auto ValidateProperty(const EditorParameterCatalogEntry& entry,
                      const EditorParameterProperty& property, const json& value,
                      std::string* error) -> bool {
  const std::string name = std::string{entry.field} + "." + std::string{property.name};
  switch (property.type) {
    case PropertyType::Number: {
      if (!value.is_number()) {
        return SetError(error, name + " must be a number in " +
                                   RangeText(property.minimum, property.maximum));
      }
      const double number = value.get<double>();
      if (!std::isfinite(number) || number < property.minimum || number > property.maximum) {
        return SetError(error,
                        name + " must be in " + RangeText(property.minimum, property.maximum));
      }
      return true;
    }
    case PropertyType::Boolean:
      return value.is_boolean() || SetError(error, name + " must be a boolean");
    case PropertyType::String:
      return value.is_string() || SetError(error, name + " must be a string");
    case PropertyType::Option: {
      if (value.is_string()) {
        const auto& text = value.get_ref<const std::string&>();
        for (const auto& option : property.options) {
          if (option.value == text) {
            return true;
          }
        }
      }
      return SetError(error, name + " must be one of: " + OptionValuesText(property));
    }
  }
  return SetError(error, name + " has an unknown property type");
}

/// @p value with the properties that it omits taken from the entry default.
auto CompleteObject(const EditorParameterCatalogEntry& entry, const json& value) -> json {
  json complete = entry.ui_default();
  for (const auto& [key, item] : value.items()) {
    complete[key] = item;
  }
  return complete;
}

auto ObjectTypeError(const EditorParameterCatalogEntry& entry, std::string* error) -> bool {
  return SetError(error, std::string{entry.field} + " must be an object with properties: " +
                             PropertyNamesText(entry));
}

auto PropertyJson(const EditorParameterProperty& property) -> json {
  json result = {{"name", std::string{property.name}}, {"type", PropertyTypeText(property.type)}};
  if (property.type == PropertyType::Number) {
    result["ui_min"]      = property.minimum;
    result["ui_max"]      = property.maximum;
    result["ui_step"]     = property.step;
    result["ui_decimals"] = property.decimals;
  }
  if (property.type == PropertyType::Option) {
    json options = json::array();
    for (const auto& option : property.options) {
      options.push_back({{"value", std::string{option.value}}, {"label", std::string{option.label}}});
    }
    result["options"] = std::move(options);
  }
  return result;
}

auto CropNumber(const json& crop_ui, const char* key) -> float {
  return static_cast<float>(crop_ui.at(key).get<double>());
}

}  // namespace

auto EditorParameterCatalog::Entries() -> std::span<const EditorParameterCatalogEntry> {
  return Table();
}

auto EditorParameterCatalog::Find(std::string_view field_key)
    -> const EditorParameterCatalogEntry* {
  for (const auto& entry : Table()) {
    if (entry.field == field_key || (!entry.alias.empty() && entry.alias == field_key)) {
      return &entry;
    }
  }
  return nullptr;
}

auto EditorParameterCatalog::ScalarUiToModel(const EditorParameterCatalogEntry& entry,
                                             double ui_value) -> double {
  switch (entry.conversion) {
    case Conversion::OffsetFromOnePercent:
      return 1.0 + ui_value / 100.0;
    case Conversion::Percent:
      return ui_value / 100.0;
    case Conversion::Identity:
    default:
      return ui_value;
  }
}

auto EditorParameterCatalog::ScalarModelToUi(const EditorParameterCatalogEntry& entry,
                                             double model_value) -> double {
  switch (entry.conversion) {
    case Conversion::OffsetFromOnePercent:
      return (model_value - 1.0) * 100.0;
    case Conversion::Percent:
      return model_value * 100.0;
    case Conversion::Identity:
    default:
      return model_value;
  }
}

auto EditorParameterCatalog::ValidateScalarUiValue(const EditorParameterCatalogEntry& entry,
                                                   double ui_value, std::string* error) -> bool {
  if (!std::isfinite(ui_value) || ui_value < entry.range.minimum ||
      ui_value > entry.range.maximum) {
    return SetError(error, std::string{entry.field} + " must be in " + RangeText(entry.range));
  }
  return true;
}

auto EditorParameterCatalog::ValidateObjectUiValue(const EditorParameterCatalogEntry& entry,
                                                   const json& ui_value, std::string* error)
    -> bool {
  if (entry.kind != EditorParameterValueKind::Object || !ui_value.is_object()) {
    return ObjectTypeError(entry, error);
  }
  for (const auto& [key, value] : ui_value.items()) {
    const auto* property = FindProperty(entry, key);
    if (property == nullptr) {
      return SetError(error, "Unknown " + std::string{entry.field} + " property: " + key +
                                 ". Properties: " + PropertyNamesText(entry));
    }
    if (!ValidateProperty(entry, *property, value, error)) {
      return false;
    }
  }
  return true;
}

auto EditorParameterCatalog::ToModelJson(std::string_view field_key, const json& ui_value,
                                         const json& current_model_json, std::string* error)
    -> std::optional<json> {
  return ToModelJson(field_key, ui_value, current_model_json, EditorParameterSource{}, error);
}

auto EditorParameterCatalog::ToModelJson(std::string_view field_key, const json& ui_value,
                                         const json&                  current_model_json,
                                         const EditorParameterSource& source, std::string* error)
    -> std::optional<json> {
  const auto* entry = Find(field_key);
  if (entry == nullptr) {
    SetError(error, "Unknown editor parameter field: " + std::string{field_key});
    return std::nullopt;
  }
  if (entry->kind == EditorParameterValueKind::Object) {
    if (!ValidateObjectUiValue(*entry, ui_value, error)) {
      return std::nullopt;
    }
    json merged;
    if (current_model_json.is_null()) {
      merged = entry->ui_default();
    } else {
      auto current = entry->model_to_ui(current_model_json, error);
      if (!current.has_value()) {
        return std::nullopt;
      }
      merged = CompleteObject(*entry, *current);
    }
    for (const auto& [key, value] : ui_value.items()) {
      merged[key] = value;
    }
    if (entry->constrain != nullptr) {
      auto constrained = entry->constrain(merged, ui_value, source, error);
      if (!constrained.has_value()) {
        return std::nullopt;
      }
      merged = std::move(*constrained);
    }
    return entry->ui_to_model(merged);
  }
  if (!ui_value.is_number()) {
    SetError(error, std::string{entry->field} + " must be a number in " + RangeText(entry->range));
    return std::nullopt;
  }
  const double value = ui_value.get<double>();
  if (!ValidateScalarUiValue(*entry, value, error)) {
    return std::nullopt;
  }
  json model = json::object();
  model[std::string{entry->model_key}] = ScalarUiToModel(*entry, value);
  return model;
}

auto EditorParameterCatalog::UiStateToModelJson(std::string_view field_key, const json& ui_value,
                                                std::string* error) -> std::optional<json> {
  const auto* entry = Find(field_key);
  if (entry == nullptr) {
    SetError(error, "Unknown editor parameter field: " + std::string{field_key});
    return std::nullopt;
  }
  if (entry->kind != EditorParameterValueKind::Object) {
    SetError(error, std::string{entry->field} + " is not an object field");
    return std::nullopt;
  }
  if (!ValidateObjectUiValue(*entry, ui_value, error)) {
    return std::nullopt;
  }
  return entry->ui_to_model(CompleteObject(*entry, ui_value));
}

auto EditorParameterCatalog::ToUiValue(std::string_view field_key, const json& model_json,
                                       std::string* error) -> std::optional<json> {
  const auto* entry = Find(field_key);
  if (entry == nullptr) {
    SetError(error, "Unknown editor parameter field: " + std::string{field_key});
    return std::nullopt;
  }
  if (entry->kind == EditorParameterValueKind::Object) {
    auto ui = entry->model_to_ui(model_json, error);
    if (!ui.has_value()) {
      return std::nullopt;
    }
    return CompleteObject(*entry, *ui);
  }
  const auto* value = FindScalarModelValue(*entry, model_json);
  if (value == nullptr || !value->is_number() || !std::isfinite(value->get<double>())) {
    SetError(error, std::string{entry->field} + " Model JSON has no finite " +
                        std::string{entry->model_key} + " value");
    return std::nullopt;
  }
  return json(ScalarModelToUi(*entry, value->get<double>()));
}

auto EditorParameterCatalog::UiDefault(const EditorParameterCatalogEntry& entry) -> json {
  if (entry.kind == EditorParameterValueKind::Object) {
    return entry.ui_default();
  }
  return json(entry.range.default_value);
}

auto EditorParameterCatalog::CropLockedAspectRatio(const json&                  crop_ui,
                                                   const EditorParameterSource& source)
    -> std::optional<double> {
  const auto preset =
      ui::geometry::ParseCropAspectPreset(crop_ui.at("aspect_preset").get<std::string>());
  const float aspect_w = CropNumber(crop_ui, "aspect_width");
  const float aspect_h = CropNumber(crop_ui, "aspect_height");
  if (!preset.has_value() || !ui::geometry::HasLockedAspect(*preset, aspect_w, aspect_h)) {
    return std::nullopt;
  }
  if (*preset == ui::geometry::CropAspectPreset::Custom) {
    const auto ratio = ui::geometry::AspectRatioFromSize(aspect_w, aspect_h);
    return ratio.has_value() ? static_cast<double>(*ratio) : 1.0;
  }
  const auto ratio = ui::geometry::CropAspectPresetRatio(*preset);
  if (!ratio.has_value()) {
    return 1.0;
  }
  double value = static_cast<double>(ratio->at(0)) /
                 std::max(static_cast<double>(ratio->at(1)), kCropOrientationEps);
  // Fixed presets describe an unoriented frame shape. Match that shape to the source so 16:9
  // becomes 9:16 for a portrait photograph.
  const bool source_portrait = SourceAspect(source) < 1.0;
  const bool preset_portrait = value < 1.0;
  if (std::abs(value - 1.0) > kCropOrientationEps && source_portrait != preset_portrait) {
    value = 1.0 / std::max(value, kCropOrientationEps);
  }
  return value;
}

auto EditorParameterCatalog::ConstrainCrop(const json& crop_ui, EditorCropDriver driver,
                                           const EditorParameterSource& source) -> json {
  const auto* entry = Find("crop_rotate");
  json        ui    = CompleteObject(*entry, crop_ui);
  if (driver == EditorCropDriver::AspectPreset) {
    const auto& preset_id = ui.at("aspect_preset").get_ref<const std::string&>();
    if (IsFixedAspectPreset(preset_id)) {
      const auto preset = ui::geometry::ParseCropAspectPreset(preset_id);
      const auto ratio  = preset.has_value() ? ui::geometry::CropAspectPresetRatio(*preset)
                                             : std::optional<std::array<float, 2>>{};
      ui["aspect_width"]  = ratio.has_value() ? static_cast<double>(ratio->at(0)) : 1.0;
      ui["aspect_height"] = ratio.has_value() ? static_cast<double>(ratio->at(1)) : 1.0;
    }
  } else if (driver == EditorCropDriver::AspectSize) {
    ui["aspect_preset"] = "custom";
  }

  // The geometry equations are single precision. A rectangle that no constraint touches keeps
  // its exact values.
  std::array<float, 4> rect = {CropNumber(ui, "x"), CropNumber(ui, "y"), CropNumber(ui, "width"),
                               CropNumber(ui, "height")};
  bool                 constrained = false;
  if (const auto aspect = CropLockedAspectRatio(ui, source); aspect.has_value()) {
    const float image_aspect = static_cast<float>(SourceAspect(source));
    const float crop_aspect  = static_cast<float>(*aspect);
    if (driver == EditorCropDriver::AspectPreset || driver == EditorCropDriver::AspectSize) {
      rect        = ui::geometry::MakeMaxAspectCropRect(image_aspect, crop_aspect);
      constrained = true;
    } else if (driver == EditorCropDriver::Width || driver == EditorCropDriver::Height) {
      rect        = ui::geometry::ResizeAspectRectAroundCenter(rect[0], rect[1], rect[2], rect[3],
                                                               image_aspect, crop_aspect,
                                                               driver == EditorCropDriver::Width);
      constrained = true;
    }
  }
  if (source.width != 0 && source.height != 0) {
    const auto clamped =
        ClampCropToRotatedSource(NormalizedRect{rect[0], rect[1], rect[2], rect[3]},
                                 CropNumber(ui, "angle_degrees"),
                                 Extent2D{source.width, source.height});
    rect        = {clamped.x, clamped.y, clamped.w, clamped.h};
    constrained = true;
  }
  if (!constrained) {
    return ui;
  }
  ui["x"]      = static_cast<double>(rect[0]);
  ui["y"]      = static_cast<double>(rect[1]);
  ui["width"]  = static_cast<double>(rect[2]);
  ui["height"] = static_cast<double>(rect[3]);
  return ui;
}

auto EditorParameterCatalog::EntryJson(const EditorParameterCatalogEntry& entry) -> json {
  json result = {
      {"field", std::string{entry.field}},
      {"kind", KindText(entry.kind)},
      {"panel", std::string{entry.panel}},
  };
  if (entry.kind == EditorParameterValueKind::Object) {
    json properties = json::array();
    for (const auto& property : entry.properties) {
      properties.push_back(PropertyJson(property));
    }
    result["properties"] = std::move(properties);
    result["ui_default"] = entry.ui_default();
  } else {
    result["ui_min"]      = entry.range.minimum;
    result["ui_max"]      = entry.range.maximum;
    result["ui_default"]  = entry.range.default_value;
    result["ui_step"]     = entry.range.step;
    result["ui_decimals"] = entry.range.decimals;
  }
  if (!entry.alias.empty()) {
    result["aliases"] = json::array({std::string{entry.alias}});
  }
  return result;
}

auto EditorParameterCatalog::CatalogJson() -> json {
  json fields = json::array();
  for (const auto& entry : Table()) {
    fields.push_back(EntryJson(entry));
  }
  return fields;
}

}  // namespace alcedo
