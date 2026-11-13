//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_parameter_catalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <sstream>
#include <string>
#include <utility>

#include "app/editor_adjustment_context.hpp"
#include "app/editor_panel_projection.hpp"
#include "edit/geometry/crop_frame.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/develop_raster_input.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_tone.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "ui/alcedo_main/editor_support/modules/color_temp.hpp"
#include "ui/alcedo_main/editor_support/modules/geometry.hpp"
#include "ui/alcedo_main/editor_support/modules/hls_math.hpp"

namespace alcedo {
namespace {

using Conversion   = EditorScalarUnitConversion;
using PropertyType = EditorParameterPropertyType;
using json         = nlohmann::json;

constexpr EditorScalarUiRange kSignedPercentRange{-100.0, 100.0, 0.0, 1.0, 0};
constexpr EditorScalarUiRange kPercentRange{0.0, 100.0, 0.0, 1.0, 0};
/// Film Grain: 100 % is the datasheet granularity of the film (decision U3).
constexpr EditorScalarUiRange kFilmGrainRange{0.0, 200.0, 0.0, 1.0, 0};

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
    case EditorParameterValueKind::Model:
      return "model";
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
    case PropertyType::NumberList:
      return "number_list";
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

/// The panel projection of a RAW image has `"raster": false`: a RAW image has no input profile,
/// so the field has no UI value, like the RAW Decode panel hides the control.
auto InputProfileModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& input = UnwrapObject(model_json, "input");
  if (!RequireObject(input, "input_profile", error)) {
    return std::nullopt;
  }
  bool raster = true;
  if (!ReadBool(input, "raster", true, "input_profile", &raster, error)) {
    return std::nullopt;
  }
  if (!raster) {
    SetError(error, "input_profile: a RAW image has no input profile");
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

auto LensConstrain(const json& merged_ui, const json& ui_change, const json& current_ui,
                   const EditorParameterSource& source, std::string* error)
    -> std::optional<json> {
  static_cast<void>(current_ui);
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

auto CropConstrain(const json& merged_ui, const json& ui_change, const json& current_ui,
                   const EditorParameterSource& source, std::string* error)
    -> std::optional<json> {
  // The constraint follows the values that the write changes. A write of the complete current
  // value (for example from editor.get) with one changed value constrains for that value only.
  json changed = json::object();
  for (const auto& [key, value] : ui_change.items()) {
    if (value != current_ui.at(key)) {
      changed[key] = value;
    }
  }
  // A fixed preset sets the aspect size to its own ratio, so a write that gives a fixed preset
  // and changes the aspect size disagrees. An aspect size change alone selects custom.
  if (ui_change.contains("aspect_preset") &&
      IsFixedAspectPreset(ui_change.at("aspect_preset").get_ref<const std::string&>()) &&
      (changed.contains("aspect_width") || changed.contains("aspect_height"))) {
    SetError(error, "crop_rotate.aspect_width and aspect_height need aspect_preset custom");
    return std::nullopt;
  }
  return EditorParameterCatalog::ConstrainCrop(merged_ui, CropDriverFromChange(changed), source);
}

/// Source width / height, or 1 when no frame is presented.
auto SourceAspect(const EditorParameterSource& source) -> double {
  if (source.width == 0 || source.height == 0) {
    return 1.0;
  }
  return static_cast<double>(source.width) / static_cast<double>(source.height);
}

// ---------------------------------------------------------------------------------------------
// odt: display transform. Method, output encoding, and the method presets.

constexpr std::array kOdtMethodOptions = {
    EditorParameterOption{"aces_2_0", "ACES 2.0"},
    EditorParameterOption{"open_drt", "OpenDRT"},
    EditorParameterOption{"film_print", "Film Print"},
};

// The DrtColorSpace values of the display transform Model (ParseDrtColorSpace).
constexpr std::array kOdtEncodingSpaceOptions = {
    EditorParameterOption{"rec709", "Rec.709"},
    EditorParameterOption{"p3_d65", "P3-D65"},
    EditorParameterOption{"rec2020", "Rec.2020"},
};

constexpr std::array kOdtLimitingSpaceOptions = {
    EditorParameterOption{"rec709", "Rec.709"},
    EditorParameterOption{"rec2020", "Rec.2020"},
    EditorParameterOption{"p3_d65", "P3-D65"},
};

// Every EOTF of the per-space lists below.
constexpr std::array kOdtEotfOptions = {
    EditorParameterOption{"bt1886", "BT.1886"},
    EditorParameterOption{"gamma_2_2", "Gamma 2.2"},
    EditorParameterOption{"srgb_piecewise", "sRGB"},
    EditorParameterOption{"st2084", "ST 2084 (PQ)"},
    EditorParameterOption{"hlg", "HLG"},
};

constexpr std::array kOdtEotfRec709Options = {
    EditorParameterOption{"bt1886", "BT.1886"},
    EditorParameterOption{"gamma_2_2", "Gamma 2.2"},
    EditorParameterOption{"srgb_piecewise", "sRGB"},
};

constexpr std::array kOdtEotfP3D65Options = {
    EditorParameterOption{"gamma_2_2", "Gamma 2.2"},
    EditorParameterOption{"srgb_piecewise", "sRGB"},
    EditorParameterOption{"st2084", "ST 2084 (PQ)"},
};

constexpr std::array kOdtEotfRec2020Options = {
    EditorParameterOption{"st2084", "ST 2084 (PQ)"},
    EditorParameterOption{"hlg", "HLG"},
};

constexpr std::array kOdtLookOptions = {
    EditorParameterOption{"standard", "Standard"},
    EditorParameterOption{"arriba", "Arriba"},
    EditorParameterOption{"sylvan", "Sylvan"},
    EditorParameterOption{"colorful", "Colorful"},
    EditorParameterOption{"aery", "Aery"},
    EditorParameterOption{"dystopic", "Dystopic"},
    EditorParameterOption{"umbra", "Umbra"},
    EditorParameterOption{"custom", "Custom"},
};

constexpr std::array kOdtTonescaleOptions = {
    EditorParameterOption{"use_look_preset", "Use Look Preset"},
    EditorParameterOption{"low_contrast", "Low Contrast"},
    EditorParameterOption{"medium_contrast", "Medium Contrast"},
    EditorParameterOption{"high_contrast", "High Contrast"},
    EditorParameterOption{"arriba_tonescale", "Arriba Tonescale"},
    EditorParameterOption{"sylvan_tonescale", "Sylvan Tonescale"},
    EditorParameterOption{"colorful_tonescale", "Colorful Tonescale"},
    EditorParameterOption{"aery_tonescale", "Aery Tonescale"},
    EditorParameterOption{"dystopic_tonescale", "Dystopic Tonescale"},
    EditorParameterOption{"umbra_tonescale", "Umbra Tonescale"},
    EditorParameterOption{"aces_1_x", "ACES 1.x"},
    EditorParameterOption{"aces_2_0", "ACES 2.0"},
    EditorParameterOption{"marvelous_tonscape", "Marvelous Tonscape"},
    EditorParameterOption{"dagrinchi_tonegroan", "Dagrinchi Tonegroan"},
    EditorParameterOption{"custom", "Custom"},
};

constexpr std::array kOdtCreativeWhiteOptions = {
    EditorParameterOption{"use_look_preset", "Use Look Preset"},
    EditorParameterOption{"d93", "D93"},
    EditorParameterOption{"d75", "D75"},
    EditorParameterOption{"d65", "D65"},
    EditorParameterOption{"d60", "D60"},
    EditorParameterOption{"d55", "D55"},
    EditorParameterOption{"d50", "D50"},
};

/// Menu entries of a list of film identifiers and their catalog names.
template <std::size_t Count>
constexpr auto FilmOptions(const std::array<std::string_view, Count>& ids,
                           const std::array<std::string_view, Count>& names)
    -> std::array<EditorParameterOption, Count> {
  std::array<EditorParameterOption, Count> options{};
  for (std::size_t i = 0; i < Count; ++i) {
    options[i] = EditorParameterOption{ids[i], names[i]};
  }
  return options;
}

/// The Film list: the negatives, then the colour reversal films (lab decision U86).
constexpr auto FilmStockOptions()
    -> std::array<EditorParameterOption, kFilmNegativeIds.size() + kFilmReversalIds.size()> {
  std::array<EditorParameterOption, kFilmNegativeIds.size() + kFilmReversalIds.size()> options{};
  const auto  negatives = FilmOptions(kFilmNegativeIds, kFilmNegativeNames);
  const auto  reversals = FilmOptions(kFilmReversalIds, kFilmReversalNames);
  std::size_t index     = 0;
  for (const auto& option : negatives) {
    options[index++] = option;
  }
  for (const auto& option : reversals) {
    options[index++] = option;
  }
  return options;
}

constexpr auto kOdtFilmNegativeOptions = FilmStockOptions();
constexpr auto kOdtFilmPrintMaterialOptions =
    FilmOptions(kFilmPrintMaterialIds, kFilmPrintMaterialNames);

constexpr std::array<EditorParameterOption, kFilmSceneLightIds.size()> kOdtFilmSceneLightOptions = {
    EditorParameterOption{"reference", "Reference"},
    EditorParameterOption{"as_shot", "As Shot"},
    EditorParameterOption{"daylight", "Daylight"},
    EditorParameterOption{"tungsten", "Tungsten"},
    EditorParameterOption{"fluorescent", "Fluorescent"},
    EditorParameterOption{"led", "LED"},
    EditorParameterOption{"sodium", "Sodium"},
    EditorParameterOption{"street_mixed", "Street Mixed"},
    EditorParameterOption{"street_sodium", "Street Sodium"},
    EditorParameterOption{"street_led", "Street LED"},
    EditorParameterOption{"interior_warm", "Interior Warm"},
    EditorParameterOption{"interior_office", "Interior Office"},
};

constexpr double kOdtFilmEnlargerFlareUiMax = 0.1;

constexpr std::array<EditorParameterOption, kFilmScanAutoIds.size()> kOdtFilmScanAutoOptions = {
    EditorParameterOption{"off", "Off"},
    EditorParameterOption{"neutral", "Neutral"},
    EditorParameterOption{"warm", "Warm"},
    EditorParameterOption{"cool", "Cool"},
};

constexpr std::array<EditorParameterOption, kFilmScannerLightIds.size()>
    kOdtFilmScannerLightOptions = {
        EditorParameterOption{"halogen", "Halogen"},
        EditorParameterOption{"equal_energy", "Equal Energy"},
};

/// A number property of the film print with a stored float range.
constexpr auto FilmNumber(const char* key, float minimum, float maximum, double step, int decimals)
    -> EditorParameterProperty {
  return EditorParameterProperty{key,
                                 PropertyType::Number,
                                 {},
                                 static_cast<double>(minimum),
                                 static_cast<double>(maximum),
                                 step,
                                 decimals};
}

constexpr std::array kOdtProperties = {
    EditorParameterProperty{"method", PropertyType::Option, kOdtMethodOptions},
    EditorParameterProperty{"encoding_space", PropertyType::Option, kOdtEncodingSpaceOptions},
    EditorParameterProperty{"encoding_eotf", PropertyType::Option, kOdtEotfOptions},
    EditorParameterProperty{"peak_luminance", PropertyType::Number, {}, 100.0, 1000.0, 1.0, 0},
    EditorParameterProperty{"limiting_space", PropertyType::Option, kOdtLimitingSpaceOptions},
    EditorParameterProperty{"look_preset", PropertyType::Option, kOdtLookOptions},
    EditorParameterProperty{"tonescale_preset", PropertyType::Option, kOdtTonescaleOptions},
    EditorParameterProperty{"creative_white", PropertyType::Option, kOdtCreativeWhiteOptions},
    EditorParameterProperty{"negative", PropertyType::Option, kOdtFilmNegativeOptions},
    EditorParameterProperty{"print_material", PropertyType::Option, kOdtFilmPrintMaterialOptions},
    EditorParameterProperty{"scene_light", PropertyType::Option, kOdtFilmSceneLightOptions},
    FilmNumber("light_spread", kFilmLightSpreadMin, kFilmLightSpreadMax, 0.01, 2),
    FilmNumber("halation", kFilmHalationMin, kFilmHalationMax, 0.01, 2),
    EditorParameterProperty{"enlarger_flare",
                            PropertyType::Number,
                            {},
                            static_cast<double>(kFilmEnlargerFlareMin),
                            kOdtFilmEnlargerFlareUiMax,
                            0.001,
                            3},
    EditorParameterProperty{"auto", PropertyType::Option, kOdtFilmScanAutoOptions},
    FilmNumber("auto_strength", kFilmScanAutoStrengthMin, kFilmScanAutoStrengthMax, 0.05, 2),
    FilmNumber("auto_density", kFilmScanAutoDensityMin, kFilmScanAutoDensityMax, 0.05, 2),
    FilmNumber("scan_contrast", kFilmScanContrastMin, kFilmScanContrastMax, 0.05, 2),
    FilmNumber("scan_exposure_stops", kFilmScanExposureStopsMin, kFilmScanExposureStopsMax, 0.05,
               2),
    FilmNumber("cyan", kFilmScanKeyMin, kFilmScanKeyMax, 0.01, 2),
    FilmNumber("magenta", kFilmScanKeyMin, kFilmScanKeyMax, 0.01, 2),
    FilmNumber("yellow", kFilmScanKeyMin, kFilmScanKeyMax, 0.01, 2),
    FilmNumber("black", kFilmScanBlackMin, kFilmScanBlackMax, 0.05, 2),
    FilmNumber("scanner_red_nm", kFilmScannerRedMinNm, kFilmScannerRedMaxNm, 5.0, 0),
    FilmNumber("scanner_green_nm", kFilmScannerGreenMinNm, kFilmScannerGreenMaxNm, 5.0, 0),
    FilmNumber("scanner_blue_nm", kFilmScannerBlueMinNm, kFilmScannerBlueMaxNm, 5.0, 0),
    FilmNumber("scanner_width_nm", kFilmScannerWidthMinNm, kFilmScannerWidthMaxNm, 5.0, 0),
    EditorParameterProperty{"scanner_light", PropertyType::Option, kOdtFilmScannerLightOptions},
};

/// OpenDRT preset properties: the Model holds them under `open_drt`.
constexpr std::array<const char*, 3> kOdtOpenDrtKeys = {"look_preset", "tonescale_preset",
                                                        "creative_white"};

/// Film print properties: the Model holds them under `film_print`.
constexpr std::array<const char*, 3> kOdtFilmSelectionKeys = {"negative", "print_material",
                                                              "scene_light"};
constexpr std::array<const char*, 3> kOdtFilmSliderKeys    = {"light_spread", "halation",
                                                              "enlarger_flare"};
/// The settings of Negative Scan (issue 661). Only Negative Scan reads them, so the panel shows
/// them there only. The Model omits a key at its default.
constexpr std::array<const char*, 2>  kOdtFilmScanOptionKeys = {"auto", "scanner_light"};
constexpr std::array<const char*, 12> kOdtFilmScanNumberKeys = {
    "auto_strength",  "auto_density",     "scan_contrast",    "scan_exposure_stops",
    "cyan",           "magenta",          "yellow",           "black",
    "scanner_red_nm", "scanner_green_nm", "scanner_blue_nm",  "scanner_width_nm"};

constexpr const char* kOdtFilmPrintEncodingSpace = "rec709";
constexpr double      kOdtFilmPrintPeakLuminance = 100.0;

auto OdtUiDefault() -> json {
  const FilmPrintParams film_print;
  const auto&           scan = film_print.scan;
  return {{"method", "open_drt"},
          {"encoding_space", "rec709"},
          {"encoding_eotf", "gamma_2_2"},
          {"peak_luminance", 100.0},
          {"limiting_space", "rec709"},
          {"look_preset", "standard"},
          {"tonescale_preset", "use_look_preset"},
          {"creative_white", "use_look_preset"},
          {"negative", film_print.negative},
          {"print_material", film_print.print_material},
          {"scene_light", film_print.scene_light},
          {"light_spread", static_cast<double>(film_print.light_spread)},
          {"halation", static_cast<double>(film_print.halation)},
          {"enlarger_flare", 0.01},
          {"auto", scan.auto_colour},
          // The decimal default of new settings, as Enlarger flare.
          {"auto_strength", 0.5},
          {"auto_density", static_cast<double>(scan.auto_density)},
          {"scan_contrast", static_cast<double>(scan.contrast)},
          {"scan_exposure_stops", static_cast<double>(scan.exposure_stops)},
          {"cyan", static_cast<double>(scan.cyan)},
          {"magenta", static_cast<double>(scan.magenta)},
          {"yellow", static_cast<double>(scan.yellow)},
          {"black", static_cast<double>(scan.black)},
          {"scanner_red_nm", static_cast<double>(scan.scanner_red_nm)},
          {"scanner_green_nm", static_cast<double>(scan.scanner_green_nm)},
          {"scanner_blue_nm", static_cast<double>(scan.scanner_blue_nm)},
          {"scanner_width_nm", static_cast<double>(scan.scanner_width_nm)},
          {"scanner_light", scan.scanner_light}};
}

/// True when the UI value selects the Negative Scan form: method film_print with print material
/// Negative Scan and a negative. That form ends in linear AP1 that the OpenDRT of the document shows
/// (issue 661), so it has the OpenDRT settings and encodings of method open_drt.
auto OdtIsNegativeScan(const json& ui_value) -> bool {
  return ui_value.at("method").get_ref<const std::string&>() == "film_print" &&
         IsFilmNegativeScan(ui_value.at("negative").get_ref<const std::string&>(),
                            ui_value.at("print_material").get_ref<const std::string&>());
}

auto OptionsText(std::span<const EditorParameterOption> options) -> std::string {
  std::string text;
  for (const auto& option : options) {
    if (!text.empty()) {
      text += ", ";
    }
    text += option.value;
  }
  return text;
}

auto HasOption(std::span<const EditorParameterOption> options, std::string_view value) -> bool {
  return std::any_of(options.begin(), options.end(),
                     [value](const EditorParameterOption& option) { return option.value == value; });
}

auto OdtModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& odt = UnwrapObject(model_json, "odt");
  if (!RequireObject(odt, "odt", error)) {
    return std::nullopt;
  }
  const auto  defaults = OdtUiDefault();
  json        ui       = json::object();
  for (const char* key : {"method", "encoding_space", "encoding_eotf", "limiting_space"}) {
    std::string value;
    if (!ReadString(odt, key, defaults.at(key).get<std::string>(), "odt", &value, error)) {
      return std::nullopt;
    }
    ui[key] = value;
  }
  double peak = 0.0;
  if (!ReadNumber(odt, "peak_luminance", defaults.at("peak_luminance").get<double>(), "odt", &peak,
                  error)) {
    return std::nullopt;
  }
  ui["peak_luminance"] = peak;
  static const json kEmpty   = json::object();
  const auto        drt_it   = odt.find("open_drt");
  const json&       open_drt = drt_it != odt.end() ? *drt_it : kEmpty;
  if (!RequireObject(open_drt, "odt.open_drt", error)) {
    return std::nullopt;
  }
  for (const char* key : kOdtOpenDrtKeys) {
    std::string value;
    if (!ReadString(open_drt, key, defaults.at(key).get<std::string>(), "odt", &value, error)) {
      return std::nullopt;
    }
    ui[key] = value;
  }
  const auto  film_it    = odt.find("film_print");
  const json& film_print = film_it != odt.end() ? *film_it : kEmpty;
  if (!RequireObject(film_print, "odt.film_print", error)) {
    return std::nullopt;
  }
  for (const char* key : kOdtFilmSelectionKeys) {
    std::string value;
    if (!ReadString(film_print, key, defaults.at(key).get<std::string>(), "odt", &value, error)) {
      return std::nullopt;
    }
    ui[key] = value;
  }
  for (const char* key : kOdtFilmSliderKeys) {
    double value = 0.0;
    if (!ReadNumber(film_print, key, defaults.at(key).get<double>(), "odt", &value, error)) {
      return std::nullopt;
    }
    ui[key] = value;
  }
  // As the Model loads it: in a stored `film_print` object a missing `auto` or `auto_strength` is
  // the stored default (off, 0.8) of a build before G8; with no object, the default of new
  // settings.
  auto scan_defaults = defaults;
  if (film_it != odt.end()) {
    scan_defaults["auto"]          = std::string{kFilmScanAutoStoredDefault};
    // The decimal of kFilmScanAutoStrengthStoredDefault: the float 0.8f is 0.800000011920929.
    scan_defaults["auto_strength"] = 0.8;
  }
  for (const char* key : kOdtFilmScanOptionKeys) {
    std::string value;
    if (!ReadString(film_print, key, scan_defaults.at(key).get<std::string>(), "odt", &value,
                    error)) {
      return std::nullopt;
    }
    ui[key] = value;
  }
  for (const char* key : kOdtFilmScanNumberKeys) {
    double value = 0.0;
    if (!ReadNumber(film_print, key, scan_defaults.at(key).get<double>(), "odt", &value, error)) {
      return std::nullopt;
    }
    ui[key] = value;
  }
  // The Model stores a float, and the float of the decimal limit 0.1 is above that limit.
  ui["enlarger_flare"] =
      std::min(ui.at("enlarger_flare").get<double>(), kOdtFilmEnlargerFlareUiMax);
  return ui;
}

/// The film print has Rec. 709 SDR output only: an EOTF of Rec. 709 stays, another EOTF reads as
/// the default.
auto OdtFilmPrintEotf(const json& ui_value) -> std::string {
  const auto& eotf = ui_value.at("encoding_eotf").get_ref<const std::string&>();
  return HasOption(EditorParameterCatalog::OdtEotfOptions(kOdtFilmPrintEncodingSpace), eotf)
             ? eotf
             : OdtUiDefault().at("encoding_eotf").get<std::string>();
}

auto OdtUiToModel(const json& ui_value) -> json {
  if (ui_value.at("method").get_ref<const std::string&>() == "film_print") {
    json film_print = json::object();
    for (const char* key : kOdtFilmSelectionKeys) {
      film_print[key] = ui_value.at(key);
    }
    for (const char* key : kOdtFilmSliderKeys) {
      film_print[key] = ui_value.at(key);
    }
    for (const auto& keys : {std::span<const char* const>{kOdtFilmScanOptionKeys},
                             std::span<const char* const>{kOdtFilmScanNumberKeys}}) {
      for (const char* key : keys) {
        film_print[key] = ui_value.at(key);
      }
    }
    if (OdtIsNegativeScan(ui_value)) {
      // Negative Scan writes the OpenDRT settings and the encoding of the panel, as method open_drt.
      json open_drt = json::object();
      for (const char* key : kOdtOpenDrtKeys) {
        open_drt[key] = ui_value.at(key);
      }
      return {{"odt",
               {{"method", "film_print"},
                {"encoding_space", ui_value.at("encoding_space")},
                {"encoding_eotf", ui_value.at("encoding_eotf")},
                {"peak_luminance", ui_value.at("peak_luminance")},
                {"open_drt", std::move(open_drt)},
                {"film_print", std::move(film_print)}}}};
    }
    // The panel hides the encoding controls of a print, so a selection in an HDR or P3 document
    // writes the only output of this form. Undo restores the earlier encoding through the history
    // value.
    return {{"odt",
             {{"method", "film_print"},
              {"encoding_space", kOdtFilmPrintEncodingSpace},
              {"encoding_eotf", OdtFilmPrintEotf(ui_value)},
              {"peak_luminance", kOdtFilmPrintPeakLuminance},
              {"film_print", std::move(film_print)}}}};
  }
  // The panel writes the controls of the selected method only.
  json odt = {{"method", ui_value.at("method")},
              {"encoding_space", ui_value.at("encoding_space")},
              {"encoding_eotf", ui_value.at("encoding_eotf")},
              {"peak_luminance", ui_value.at("peak_luminance")}};
  if (ui_value.at("method").get_ref<const std::string&>() == "aces_2_0") {
    odt["limiting_space"] = ui_value.at("limiting_space");
  } else {
    json open_drt = json::object();
    for (const char* key : kOdtOpenDrtKeys) {
      open_drt[key] = ui_value.at(key);
    }
    odt["open_drt"] = std::move(open_drt);
  }
  return {{"odt", std::move(odt)}};
}

/// True when @p ui_change sets @p key to a value other than the current one.
auto ChangesValue(const json& ui_change, const json& current_ui, const char* key) -> bool {
  const auto it = ui_change.find(key);
  return it != ui_change.end() && *it != current_ui.at(key);
}

auto OdtConstrain(const json& merged_ui, const json& ui_change, const json& current_ui,
                  const EditorParameterSource& source, std::string* error)
    -> std::optional<json> {
  static_cast<void>(source);
  json ui = merged_ui;
  // Selecting Negative Scan selects the scene light of the RAW white balance in the same write,
  // unless the write sets a scene light of its own (issue 661).
  if (ui.at("method").get_ref<const std::string&>() == "film_print" &&
      ChangesValue(ui_change, current_ui, "print_material") &&
      ui_change.at("print_material") == std::string{kFilmNegativeScanId} &&
      !ChangesValue(ui_change, current_ui, "scene_light")) {
    ui["scene_light"] = std::string{kFilmAsShotLightId};
  }
  const bool negative_scan = OdtIsNegativeScan(ui);
  if (ui.at("method").get_ref<const std::string&>() == "film_print" && !negative_scan) {
    if (ChangesValue(ui_change, current_ui, "encoding_space") &&
        ui_change.at("encoding_space") != kOdtFilmPrintEncodingSpace) {
      SetError(error, "odt.encoding_space must be rec709 with method film_print");
      return std::nullopt;
    }
    if (ChangesValue(ui_change, current_ui, "peak_luminance") &&
        ui_change.at("peak_luminance") != kOdtFilmPrintPeakLuminance) {
      SetError(error, "odt.peak_luminance must be 100 with method film_print");
      return std::nullopt;
    }
    ui["encoding_space"] = kOdtFilmPrintEncodingSpace;
    ui["peak_luminance"] = kOdtFilmPrintPeakLuminance;
    if (!ChangesValue(ui_change, current_ui, "encoding_eotf")) {
      ui["encoding_eotf"] = OdtFilmPrintEotf(ui);
    }
  }
  const auto& space = ui.at("encoding_space").get_ref<const std::string&>();
  const auto  eotfs = EditorParameterCatalog::OdtEotfOptions(space);
  if (!HasOption(eotfs, ui.at("encoding_eotf").get_ref<const std::string&>())) {
    if (ui_change.contains("encoding_eotf")) {
      SetError(error, "odt.encoding_eotf must be one of: " + OptionsText(eotfs) +
                          " for encoding_space " + space);
      return std::nullopt;
    }
    // The panel rule: a new encoding space keeps the EOTF when the space has it, else it
    // selects the first EOTF of the space.
    ui["encoding_eotf"] = std::string{eotfs.front().value};
  }
  // The Model write holds the controls of the selected method only, so a change of the other
  // method controls is not written.
  const auto& method = ui.at("method").get_ref<const std::string&>();
  if (method != "aces_2_0" && ChangesValue(ui_change, current_ui, "limiting_space")) {
    SetError(error, "odt.limiting_space needs method aces_2_0");
    return std::nullopt;
  }
  if (method != "open_drt" && !negative_scan) {
    for (const char* key : kOdtOpenDrtKeys) {
      if (ChangesValue(ui_change, current_ui, key)) {
        SetError(error, std::string{"odt."} + key +
                            " needs method open_drt, or film_print with print material "
                            "negative_scan");
        return std::nullopt;
      }
    }
  }
  if (method != "film_print") {
    for (const auto& keys :
         {std::span<const char* const>{kOdtFilmSelectionKeys},
          std::span<const char* const>{kOdtFilmSliderKeys},
          std::span<const char* const>{kOdtFilmScanOptionKeys},
          std::span<const char* const>{kOdtFilmScanNumberKeys}}) {
      for (const char* key : keys) {
        if (ChangesValue(ui_change, current_ui, key)) {
          SetError(error, std::string{"odt."} + key + " needs method film_print");
          return std::nullopt;
        }
      }
    }
  }
  return ui;
}

// ---------------------------------------------------------------------------------------------
// color_temp: Develop white balance of a RAW document. Kelvin and tint on the RAW Custom WB
// scale; as_shot uses the white balance that the camera recorded.

constexpr std::array kColorTempModeOptions = {
    EditorParameterOption{"as_shot", "As Shot"},
    EditorParameterOption{"custom", "Custom"},
};

constexpr std::array kColorTempProperties = {
    EditorParameterProperty{"mode", PropertyType::Option, kColorTempModeOptions},
    EditorParameterProperty{"kelvin", PropertyType::Number, {},
                            static_cast<double>(ui::color_temp::kCctMin),
                            static_cast<double>(ui::color_temp::kCctMax), 1.0, 0,
                            EditorParameterSliderScale::KelvinPivot},
    EditorParameterProperty{"tint", PropertyType::Number, {},
                            static_cast<double>(ui::color_temp::kTintMin),
                            static_cast<double>(ui::color_temp::kTintMax), 1.0, 0},
};

auto ColorTempUiDefault() -> json {
  const EditorPanelColorTempValue defaults;
  return {{"mode", defaults.mode},
          {"kelvin", static_cast<double>(defaults.custom_cct)},
          {"tint", static_cast<double>(defaults.custom_tint)}};
}

auto ColorTempModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& temp = UnwrapObject(model_json, "color_temp");
  if (!RequireObject(temp, "color_temp", error)) {
    return std::nullopt;
  }
  const auto  defaults = ColorTempUiDefault();
  std::string mode;
  if (!ReadString(temp, "mode", defaults.at("mode").get<std::string>(), "color_temp", &mode,
                  error)) {
    return std::nullopt;
  }
  // The UI value is the pair that the panel shows: the custom pair in custom mode, the
  // recorded camera pair in as_shot mode.
  const bool  custom      = mode == "custom";
  const char* kelvin_key  = custom ? "custom_cct" : "as_shot_cct";
  const char* tint_key    = custom ? "custom_tint" : "as_shot_tint";
  double      kelvin      = 0.0;
  double      tint        = 0.0;
  if (!ReadNumber(temp, kelvin_key, defaults.at("kelvin").get<double>(), "color_temp", &kelvin,
                  error) ||
      !ReadNumber(temp, tint_key, defaults.at("tint").get<double>(), "color_temp", &tint, error)) {
    return std::nullopt;
  }
  return json{{"mode", mode}, {"kelvin", kelvin}, {"tint", tint}};
}

auto ColorTempUiToModel(const json& ui_value) -> json {
  if (ui_value.at("mode").get_ref<const std::string&>() == "custom") {
    return {{"color_temp",
             {{"mode", "custom"},
              {"custom_cct", ui_value.at("kelvin")},
              {"custom_tint", ui_value.at("tint")}}}};
  }
  // As Shot keeps the custom pair of the document for a later switch back to custom.
  return {{"color_temp", {{"mode", "as_shot"}}}};
}

auto ColorTempConstrain(const json& merged_ui, const json& ui_change, const json& current_ui,
                        const EditorParameterSource& source, std::string* error)
    -> std::optional<json> {
  static_cast<void>(source);
  const bool sets_pair =
      ChangesValue(ui_change, current_ui, "kelvin") || ChangesValue(ui_change, current_ui, "tint");
  if (!sets_pair) {
    return merged_ui;
  }
  if (ui_change.contains("mode") && ui_change.at("mode").get_ref<const std::string&>() != "custom") {
    SetError(error, "color_temp.kelvin and tint need mode custom");
    return std::nullopt;
  }
  // The panel rule: a Kelvin or tint edit selects custom mode.
  json ui    = merged_ui;
  ui["mode"] = "custom";
  return ui;
}

// ---------------------------------------------------------------------------------------------
// grade_white_balance: Color Grade CAT02 white balance in the grading color space.

constexpr std::array kGradeWhiteBalanceProperties = {
    EditorParameterProperty{"temperature", PropertyType::Number, {},
                            static_cast<double>(kCat02TemperatureMin),
                            static_cast<double>(kCat02TemperatureMax), 1.0, 0,
                            EditorParameterSliderScale::KelvinPivot},
    EditorParameterProperty{"tint", PropertyType::Number, {}, static_cast<double>(kCat02TintMin),
                            static_cast<double>(kCat02TintMax), 1.0, 0},
};
static_assert(kCat02TemperatureMin == static_cast<float>(ui::color_temp::kCctMin) &&
                  kCat02TemperatureMax == static_cast<float>(ui::color_temp::kCctMax),
              "grade_white_balance uses the Kelvin range of the white balance track");

auto GradeWhiteBalanceUiDefault() -> json {
  return {{"temperature", static_cast<double>(kCat02DefaultTemperature)},
          {"tint", static_cast<double>(kCat02DefaultTint)}};
}

auto GradeWhiteBalanceModelToUi(const json& model_json, std::string* error)
    -> std::optional<json> {
  const auto& white_balance = UnwrapObject(model_json, "grade_white_balance");
  if (!RequireObject(white_balance, "grade_white_balance", error)) {
    return std::nullopt;
  }
  double temperature = 0.0;
  double tint        = 0.0;
  if (!ReadNumber(white_balance, "temperature", static_cast<double>(kCat02DefaultTemperature),
                  "grade_white_balance", &temperature, error) ||
      !ReadNumber(white_balance, "tint", static_cast<double>(kCat02DefaultTint),
                  "grade_white_balance", &tint, error)) {
    return std::nullopt;
  }
  return json{{"temperature", temperature}, {"tint", tint}};
}

auto GradeWhiteBalanceUiToModel(const json& ui_value) -> json {
  return {{"grade_white_balance",
           {{"temperature", ui_value.at("temperature")}, {"tint", ui_value.at("tint")}}}};
}

// ---------------------------------------------------------------------------------------------
// hls: eight hue bins of the Look panel Selective Color. The UI value holds every bin in panel
// units and the selected bin (the target hue).

constexpr int kHlsBinCount = static_cast<int>(ui::hls::kCandidateHues.size());
static_assert(kHlsBinCount == kHlsHueBinCount, "the panel shows every HLS hue bin");

constexpr std::array kHlsProperties = {
    EditorParameterProperty{"hue", PropertyType::Number, {},
                            static_cast<double>(ui::hls::kCandidateHues.front()),
                            static_cast<double>(ui::hls::kCandidateHues.back()), 45.0, 0},
    EditorParameterProperty{"hue_shift", PropertyType::NumberList, {},
                            -static_cast<double>(ui::hls::kMaxHueShiftDegrees),
                            static_cast<double>(ui::hls::kMaxHueShiftDegrees), 1.0, 0,
                            EditorParameterSliderScale::Linear, kHlsBinCount},
    EditorParameterProperty{"lightness", PropertyType::NumberList, {},
                            static_cast<double>(ui::hls::kAdjUiMin),
                            static_cast<double>(ui::hls::kAdjUiMax), 1.0, 0,
                            EditorParameterSliderScale::Linear, kHlsBinCount},
    EditorParameterProperty{"chroma", PropertyType::NumberList, {},
                            static_cast<double>(ui::hls::kAdjUiMin),
                            static_cast<double>(ui::hls::kAdjUiMax), 1.0, 0,
                            EditorParameterSliderScale::Linear, kHlsBinCount},
    EditorParameterProperty{"hue_smoothness", PropertyType::NumberList, {},
                            static_cast<double>(ui::hls::kHueRangeUiMin),
                            static_cast<double>(ui::hls::kHueRangeUiMax), 1.0, 0,
                            EditorParameterSliderScale::Linear, kHlsBinCount},
};

auto FilledList(double value) -> json {
  json list = json::array();
  for (int i = 0; i < kHlsBinCount; ++i) {
    list.push_back(value);
  }
  return list;
}

auto HlsUiDefault() -> json {
  return {{"hue", static_cast<double>(ui::hls::kCandidateHues.front())},
          {"hue_shift", FilledList(0.0)},
          {"lightness", FilledList(0.0)},
          {"chroma", FilledList(0.0)},
          {"hue_smoothness", FilledList(static_cast<double>(ui::hls::kDefaultHueRange))}};
}

/// The finite number at @p index of the array @p list, or nullopt.
auto ListNumber(const json& list, std::size_t index) -> std::optional<double> {
  if (!list.is_array() || index >= list.size() || !list.at(index).is_number()) {
    return std::nullopt;
  }
  const double value = list.at(index).get<double>();
  return std::isfinite(value) ? std::optional<double>{value} : std::nullopt;
}

auto HlsModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& hls = UnwrapObject(UnwrapObject(model_json, "HLS"), "hls");
  if (!RequireObject(hls, "hls", error)) {
    return std::nullopt;
  }
  auto ui = HlsUiDefault();
  if (const auto it = hls.find("hls_adj_table"); it != hls.end()) {
    if (!it->is_array() || it->size() != kHlsHueBinCount) {
      SetError(error, "hls Model JSON hls_adj_table must hold eight [h, l, s] rows");
      return std::nullopt;
    }
    for (std::size_t i = 0; i < it->size(); ++i) {
      const auto h = ListNumber(it->at(i), 0);
      const auto l = ListNumber(it->at(i), 1);
      const auto s = ListNumber(it->at(i), 2);
      if (!h || !l || !s) {
        SetError(error, "hls Model JSON hls_adj_table must hold eight [h, l, s] rows");
        return std::nullopt;
      }
      // The Model stores lightness and chroma divided by the panel scale.
      ui["hue_shift"][i] = *h;
      ui["lightness"][i] = *l * static_cast<double>(ui::hls::kAdjUiToParamScale);
      ui["chroma"][i]    = *s * static_cast<double>(ui::hls::kAdjUiToParamScale);
    }
  }
  if (const auto it = hls.find("h_range_table"); it != hls.end()) {
    if (!it->is_array() || it->size() != kHlsHueBinCount) {
      SetError(error, "hls Model JSON h_range_table must hold eight numbers");
      return std::nullopt;
    }
    for (std::size_t i = 0; i < it->size(); ++i) {
      const auto range = ListNumber(*it, i);
      if (!range) {
        SetError(error, "hls Model JSON h_range_table must hold eight numbers");
        return std::nullopt;
      }
      ui["hue_smoothness"][i] = *range;
    }
  }
  if (const auto it = hls.find("target_hls"); it != hls.end()) {
    const auto hue = ListNumber(*it, 0);
    if (!hue) {
      SetError(error, "hls Model JSON target_hls must start with the target hue");
      return std::nullopt;
    }
    // The panel selects the candidate hue that is nearest to the target.
    ui["hue"] = static_cast<double>(
        ui::hls::kCandidateHues[static_cast<std::size_t>(
            ui::hls::ClosestCandidateHueIndex(static_cast<float>(*hue)))]);
  }
  return ui;
}

/// Index of the candidate hue @p hue, or -1.
auto CandidateHueIndex(double hue) -> int {
  for (int i = 0; i < kHlsBinCount; ++i) {
    if (static_cast<double>(ui::hls::kCandidateHues[static_cast<std::size_t>(i)]) == hue) {
      return i;
    }
  }
  return -1;
}

auto HlsUiToModel(const json& ui_value) -> json {
  // The Look panel write: every bin, the selected bin as the target, and the fixed L/S ranges.
  const double scale = static_cast<double>(ui::hls::kAdjUiToParamScale);
  json         bins  = json::array();
  json         table = json::array();
  json         range = json::array();
  for (std::size_t i = 0; i < ui::hls::kCandidateHues.size(); ++i) {
    bins.push_back(static_cast<double>(ui::hls::kCandidateHues[i]));
    table.push_back(json::array({ui_value.at("hue_shift").at(i).get<double>(),
                                 ui_value.at("lightness").at(i).get<double>() / scale,
                                 ui_value.at("chroma").at(i).get<double>() / scale}));
    range.push_back(ui_value.at("hue_smoothness").at(i));
  }
  // HlsValidate accepted the hue, so it is a candidate hue.
  const auto active  = static_cast<std::size_t>(CandidateHueIndex(ui_value.at("hue").get<double>()));
  json       hls_adj = table.at(active);
  json       h_range = range.at(active);
  json       target  = json::array({static_cast<double>(ui::hls::kCandidateHues[active]),
                                    static_cast<double>(ui::hls::kFixedTargetLightness),
                                    static_cast<double>(ui::hls::kFixedTargetSaturation)});
  json       hls     = json::object();
  hls["hue_bins"]      = std::move(bins);
  hls["hls_adj_table"] = std::move(table);
  hls["h_range_table"] = std::move(range);
  hls["target_hls"]    = std::move(target);
  hls["hls_adj"]       = std::move(hls_adj);
  hls["h_range"]       = std::move(h_range);
  hls["l_range"]       = static_cast<double>(ui::hls::kFixedLightnessRange);
  hls["s_range"]       = static_cast<double>(ui::hls::kFixedSaturationRange);
  return {{"HLS", std::move(hls)}};
}

auto HlsValidate(const json& ui_value, std::string* error) -> bool {
  if (CandidateHueIndex(ui_value.at("hue").get<double>()) < 0) {
    return SetError(error, "hls.hue must be one of: 0, 45, 90, 135, 180, 225, 270, 315");
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// color_wheel: the Lift, Gamma, Gain, and Offset tone wheels. The UI value is the disc position of
// each wheel in the unit disc and its Master value in stops.

constexpr std::array<const char*, 4> kWheelNames = {"lift", "gamma", "gain", "offset"};

constexpr auto WheelProperties() -> std::array<EditorParameterProperty, 12> {
  std::array<EditorParameterProperty, 12> result{};
  constexpr std::array<std::array<std::string_view, 3>, 4> kNames = {{
      {"lift_x", "lift_y", "lift_master"},
      {"gamma_x", "gamma_y", "gamma_master"},
      {"gain_x", "gain_y", "gain_master"},
      {"offset_x", "offset_y", "offset_master"},
  }};
  constexpr double kMasterMax = static_cast<double>(color_wheel::kMasterMaxStops);
  for (std::size_t wheel = 0; wheel < kNames.size(); ++wheel) {
    result[wheel * 3]     = {kNames[wheel][0], PropertyType::Number, {}, -1.0, 1.0, 0.001, 3};
    result[wheel * 3 + 1] = {kNames[wheel][1], PropertyType::Number, {}, -1.0, 1.0, 0.001, 3};
    result[wheel * 3 + 2] = {
        kNames[wheel][2], PropertyType::Number, {}, -kMasterMax, kMasterMax, 0.01, 2};
  }
  return result;
}

constexpr auto kColorWheelProperties = WheelProperties();

auto ColorWheelUiDefault() -> json {
  json ui = json::object();
  for (const char* wheel : kWheelNames) {
    const std::string name{wheel};
    ui[name + "_x"]      = 0.0;
    ui[name + "_y"]      = 0.0;
    ui[name + "_master"] = 0.0;
  }
  return ui;
}

auto ColorWheelModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& wheels = UnwrapObject(model_json, "color_wheel");
  if (!RequireObject(wheels, "color_wheel", error)) {
    return std::nullopt;
  }
  static const json kEmpty = json::object();
  json              ui     = ColorWheelUiDefault();
  for (const char* wheel : kWheelNames) {
    const std::string name{wheel};
    const json*       control = &kEmpty;
    if (const auto it = wheels.find(name); it != wheels.end()) {
      control = &*it;
    }
    if (!RequireObject(*control, "color_wheel." + name, error)) {
      return std::nullopt;
    }
    const json* disc = &kEmpty;
    if (const auto it = control->find("disc"); it != control->end()) {
      disc = &*it;
    }
    if (!RequireObject(*disc, "color_wheel." + name + ".disc", error)) {
      return std::nullopt;
    }
    double x = 0.0, y = 0.0, master = 0.0;
    if (!ReadNumber(*disc, "x", 0.0, "color_wheel", &x, error) ||
        !ReadNumber(*disc, "y", 0.0, "color_wheel", &y, error) ||
        !ReadNumber(*control, "luminance_offset", 0.0, "color_wheel", &master, error)) {
      return std::nullopt;
    }
    ui[name + "_x"]      = x;
    ui[name + "_y"]      = y;
    ui[name + "_master"] = master;
  }
  return ui;
}

auto ColorWheelUiToModel(const json& ui_value) -> json {
  // The trackball write: the persisted format version, and the disc, the panel strength, the
  // per-channel disc stops, and the Master stops of each wheel.
  json wheels       = json::object();
  wheels["version"] = 2;
  for (const char* wheel : kWheelNames) {
    const std::string name{wheel};
    const double      x     = ui_value.at(name + "_x").get<double>();
    const double      y     = ui_value.at(name + "_y").get<double>();
    const auto stops = color_wheel::DiscToChannelStops(static_cast<float>(x), static_cast<float>(y),
                                                       color_wheel::kDiscMaxStops);
    wheels[name]     = {{"disc", {{"x", x}, {"y", y}}},
                        {"strength", static_cast<double>(color_wheel::kDiscMaxStops)},
                        {"color_offset",
                         {{"x", static_cast<double>(stops[0])},
                          {"y", static_cast<double>(stops[1])},
                          {"z", static_cast<double>(stops[2])}}},
                        {"luminance_offset", ui_value.at(name + "_master").get<double>()}};
  }
  return {{"color_wheel", std::move(wheels)}};
}

auto ColorWheelValidate(const json& ui_value, std::string* error) -> bool {
  for (const char* wheel : kWheelNames) {
    const std::string name{wheel};
    const double      x = ui_value.at(name + "_x").get<double>();
    const double      y = ui_value.at(name + "_y").get<double>();
    if (x * x + y * y > 1.0 + static_cast<double>(color_wheel::kEpsilon)) {
      return SetError(error, "color_wheel." + name + "_x and " + name +
                                 "_y must be inside the unit disc (x * x + y * y <= 1)");
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// curve and lut: the UI value is the Model JSON.

/// The reset value has the four curves, because a curve write leaves a curve that it does not
/// have as it is.
auto CurveUiDefault() -> json {
  json result = json::object();
  for (const auto channel : kCurveChannels) {
    json points = json::array();
    for (const auto& point : IdentityCurvePoints()) {
      points.push_back({{"x", static_cast<double>(point.x)}, {"y", static_cast<double>(point.y)}});
    }
    result[std::string{CurveChannelKey(channel)}] = std::move(points);
  }
  return result;
}

auto CurveModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& curve = UnwrapObject(model_json, "curve");
  if (!RequireObject(curve, "curve", error)) {
    return std::nullopt;
  }
  return curve;
}

auto LutUiDefault() -> json { return json::object(); }

/// The Model JSON of the LUT panel projection (`referenceKind` with `path`, `packageId` and
/// `lutId`, or `libraryPath`; `lutName`, `strength`, and the encodings): `cube_path` or a tagged
/// `reference`, the shape of the `editor_value` of lut.list. No LUT is {}.
auto LutPanelToModel(const json& panel, std::string* error) -> std::optional<json> {
  std::string kind;
  if (!ReadString(panel, "referenceKind", "none", "lut", &kind, error)) {
    return std::nullopt;
  }
  if (kind == "none") {
    return json::object();
  }
  json model = json::object();
  if (kind == "file") {
    std::string path;
    if (!ReadString(panel, "path", "", "lut", &path, error)) {
      return std::nullopt;
    }
    model["cube_path"] = path;
  } else if (kind == "official") {
    std::string package_id;
    std::string lut_id;
    if (!ReadString(panel, "packageId", "", "lut", &package_id, error) ||
        !ReadString(panel, "lutId", "", "lut", &lut_id, error)) {
      return std::nullopt;
    }
    model["reference"] = {{"kind", "official"}, {"package_id", package_id}, {"lut_id", lut_id}};
  } else if (kind == "library") {
    std::string path;
    if (!ReadString(panel, "libraryPath", "", "lut", &path, error)) {
      return std::nullopt;
    }
    model["reference"] = {{"kind", "library"}, {"path", path}};
  } else {
    SetError(error, "lut Model JSON referenceKind '" + kind + "' is not a LUT reference kind");
    return std::nullopt;
  }
  std::string name;
  double      strength = 1.0;
  std::string input_encoding;
  std::string output_encoding;
  if (!ReadString(panel, "lutName", "", "lut", &name, error) ||
      !ReadNumber(panel, "strength", 1.0, "lut", &strength, error) ||
      !ReadString(panel, "input_encoding", "", "lut", &input_encoding, error) ||
      !ReadString(panel, "output_encoding", "", "lut", &output_encoding, error)) {
    return std::nullopt;
  }
  if (!name.empty()) {
    model["name"] = name;
  }
  model["strength"] = strength;
  if (!input_encoding.empty()) {
    model["input_encoding"] = input_encoding;
  }
  if (!output_encoding.empty()) {
    model["output_encoding"] = output_encoding;
  }
  return model;
}

/// The UI value of lut is its Model JSON, which editor.set accepts. The panel projection form is
/// converted by LutPanelToModel; a Model JSON object is the UI value as it is.
auto LutModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& lut = UnwrapObject(model_json, "lut");
  if (!RequireObject(lut, "lut", error)) {
    return std::nullopt;
  }
  if (lut.contains("referenceKind")) {
    return LutPanelToModel(lut, error);
  }
  return lut;
}

auto ModelIdentity(const json& ui_value) -> json { return ui_value; }

// ---------------------------------------------------------------------------------------------
// film_format: the film format of the DRT node (plan 'film_grain_format_and_stock_plan.md',
// decision U1). The values and the labels are the document identifiers of FilmFormatId, in menu
// order (EditorParameterCatalogTest.FilmFormatOptionsAreTheThreeFormatsWithDefault35mm).

constexpr std::array kFilmFormatOptions = {
    EditorParameterOption{"16mm", "16mm"},
    EditorParameterOption{"35mm", "35mm"},
    EditorParameterOption{"70mm", "70mm"},
};

constexpr std::array kFilmFormatProperties = {
    EditorParameterProperty{"film_format", PropertyType::Option, kFilmFormatOptions},
};

auto FilmFormatUiDefault() -> json { return {{"film_format", "35mm"}}; }

auto FilmFormatModelToUi(const json& model_json, std::string* error) -> std::optional<json> {
  const auto& format = UnwrapObject(model_json, "film_format");
  if (!RequireObject(format, "film_format", error)) {
    return std::nullopt;
  }
  std::string value;
  if (!ReadString(format, "film_format", "35mm", "film_format", &value, error)) {
    return std::nullopt;
  }
  return json{{"film_format", value}};
}

auto FilmFormatUiToModel(const json& ui_value) -> json {
  return {{"film_format", ui_value.at("film_format")}};
}

// ---------------------------------------------------------------------------------------------

auto ObjectEntry(std::string_view field, EditorAdjustmentField adjustment, std::string_view panel,
                 std::span<const EditorParameterProperty> properties, json (*ui_default)(),
                 std::optional<json> (*model_to_ui)(const json&, std::string*),
                 json (*ui_to_model)(const json&),
                 std::optional<json> (*constrain)(const json&, const json&, const json&,
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

auto WithAlias(EditorParameterCatalogEntry entry, std::string_view alias)
    -> EditorParameterCatalogEntry {
  entry.alias = alias;
  return entry;
}

auto WithValidate(EditorParameterCatalogEntry entry, bool (*validate)(const json&, std::string*))
    -> EditorParameterCatalogEntry {
  entry.validate = validate;
  return entry;
}

auto ModelEntry(std::string_view field, std::string_view alias, EditorAdjustmentField adjustment,
                std::string_view panel, std::string_view model_shape, json (*ui_default)(),
                std::optional<json> (*model_to_ui)(const json&, std::string*))
    -> EditorParameterCatalogEntry {
  EditorParameterCatalogEntry entry{field, alias, adjustment, EditorParameterValueKind::Model,
                                    panel};
  entry.model_shape = model_shape;
  entry.ui_default  = ui_default;
  entry.model_to_ui = model_to_ui;
  entry.ui_to_model = &ModelIdentity;
  return entry;
}

// Values of the Tone, Look, LUT, Post Processing, RAW Decode, Geometry, and Display Transform
// panels. The exposure default is the neutral reset value; the product Default document starts
// at +1.5 EV.
auto Table() -> const std::array<EditorParameterCatalogEntry, 25>& {
  static const std::array<EditorParameterCatalogEntry, 25> entries = {
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
             Conversion::Percent, kFilmGrainRange),
      ObjectEntry("film_format", EditorAdjustmentField::FilmFormat, "post", kFilmFormatProperties,
                  &FilmFormatUiDefault, &FilmFormatModelToUi, &FilmFormatUiToModel, nullptr),
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
      ObjectEntry("odt", EditorAdjustmentField::Drt, "display", kOdtProperties, &OdtUiDefault,
                  &OdtModelToUi, &OdtUiToModel, &OdtConstrain),
      ObjectEntry("color_temp", EditorAdjustmentField::ColorTemperature, "raw", kColorTempProperties,
                  &ColorTempUiDefault, &ColorTempModelToUi, &ColorTempUiToModel,
                  &ColorTempConstrain),
      ObjectEntry("grade_white_balance", EditorAdjustmentField::GradeWhiteBalance, "look",
                  kGradeWhiteBalanceProperties, &GradeWhiteBalanceUiDefault,
                  &GradeWhiteBalanceModelToUi, &GradeWhiteBalanceUiToModel, nullptr),
      WithValidate(WithAlias(ObjectEntry("hls", EditorAdjustmentField::Hls, "look",
                                         kHlsProperties, &HlsUiDefault, &HlsModelToUi,
                                         &HlsUiToModel, nullptr),
                             "HLS"),
                   &HlsValidate),
      WithValidate(ObjectEntry("color_wheel", EditorAdjustmentField::ColorWheel, "look",
                               kColorWheelProperties, &ColorWheelUiDefault, &ColorWheelModelToUi,
                               &ColorWheelUiToModel, nullptr),
                   &ColorWheelValidate),
      ModelEntry("curve", {}, EditorAdjustmentField::Curve, "tone",
                 "{\"points\": [{\"x\": number, \"y\": number}, ...], \"red\": [...], "
                 "\"green\": [...], \"blue\": [...]}: the RGB curve (points) and the red, green, "
                 "and blue curves, each 2 to 16 control points in the unit square with x "
                 "strictly increasing. A write needs one or more of the four keys and leaves "
                 "the other curves as they are; in a write, the value of a key can be the id of "
                 "a Tone Curve preset (curve.presets), which writes the points of the preset. "
                 "The render applies the RGB curve to each channel, then the curve of the "
                 "channel",
                 &CurveUiDefault, &CurveModelToUi),
      ModelEntry("lut", "ocio_lmt", EditorAdjustmentField::Lut, "lut",
                 "{\"cube_path\": string} or a tagged {\"reference\": object}, and optional "
                 "\"name\", \"strength\" (0 to 1), \"input_encoding\", and "
                 "\"output_encoding\" (color encoding ids). {} is no LUT",
                 &LutUiDefault, &LutModelToUi),
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
    case PropertyType::NumberList: {
      const std::string text = name + " must be a list of " + std::to_string(property.count) +
                               " numbers in " + RangeText(property.minimum, property.maximum);
      if (!value.is_array() || value.size() != static_cast<std::size_t>(property.count)) {
        return SetError(error, text);
      }
      for (const auto& item : value) {
        if (!item.is_number() || !std::isfinite(item.get<double>()) ||
            item.get<double>() < property.minimum || item.get<double>() > property.maximum) {
          return SetError(error, text);
        }
      }
      return true;
    }
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
  if (property.type == PropertyType::NumberList) {
    result["ui_count"] = property.count;
  }
  if (property.type == PropertyType::Number || property.type == PropertyType::NumberList) {
    result["ui_min"]      = property.minimum;
    result["ui_max"]      = property.maximum;
    result["ui_step"]     = property.step;
    result["ui_decimals"] = property.decimals;
    if (property.slider == EditorParameterSliderScale::KelvinPivot) {
      result["ui_slider"] = {{"scale", "kelvin_pivot"},
                             {"position_min", ui::color_temp::kSliderUiMin},
                             {"position_max", ui::color_temp::kSliderUiMax},
                             {"pivot_position", ui::color_temp::kSliderUiMid},
                             {"pivot_kelvin", static_cast<double>(ui::color_temp::kPivotCct)}};
    }
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
  if (entry->kind == EditorParameterValueKind::Model) {
    if (!ui_value.is_object()) {
      SetError(error, std::string{entry->field} + " must be its Model JSON: " +
                          std::string{entry->model_shape});
      return std::nullopt;
    }
    return entry->ui_to_model(ui_value);
  }
  if (entry->kind == EditorParameterValueKind::Object) {
    if (!ValidateObjectUiValue(*entry, ui_value, error)) {
      return std::nullopt;
    }
    json current_ui;
    if (current_model_json.is_null()) {
      current_ui = entry->ui_default();
    } else {
      auto current = entry->model_to_ui(current_model_json, error);
      if (!current.has_value()) {
        return std::nullopt;
      }
      current_ui = CompleteObject(*entry, *current);
    }
    json merged = current_ui;
    for (const auto& [key, value] : ui_value.items()) {
      merged[key] = value;
    }
    if (entry->constrain != nullptr) {
      auto constrained = entry->constrain(merged, ui_value, current_ui, source, error);
      if (!constrained.has_value()) {
        return std::nullopt;
      }
      merged = std::move(*constrained);
    }
    if (entry->validate != nullptr && !entry->validate(merged, error)) {
      return std::nullopt;
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
  if (entry->kind == EditorParameterValueKind::Model) {
    return ToModelJson(field_key, ui_value, json{}, error);
  }
  if (entry->kind != EditorParameterValueKind::Object) {
    SetError(error, std::string{entry->field} + " is not an object field");
    return std::nullopt;
  }
  if (!ValidateObjectUiValue(*entry, ui_value, error)) {
    return std::nullopt;
  }
  const json complete = CompleteObject(*entry, ui_value);
  if (entry->validate != nullptr && !entry->validate(complete, error)) {
    return std::nullopt;
  }
  return entry->ui_to_model(complete);
}

auto EditorParameterCatalog::ToUiValue(std::string_view field_key, const json& model_json,
                                       std::string* error) -> std::optional<json> {
  const auto* entry = Find(field_key);
  if (entry == nullptr) {
    SetError(error, "Unknown editor parameter field: " + std::string{field_key});
    return std::nullopt;
  }
  if (entry->kind == EditorParameterValueKind::Model) {
    return entry->model_to_ui(model_json, error);
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
  if (entry.kind != EditorParameterValueKind::Scalar) {
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

auto EditorParameterCatalog::OdtEotfOptions(std::string_view encoding_space)
    -> std::span<const EditorParameterOption> {
  if (encoding_space == "rec709") {
    return kOdtEotfRec709Options;
  }
  if (encoding_space == "p3_d65") {
    return kOdtEotfP3D65Options;
  }
  if (encoding_space == "rec2020") {
    return kOdtEotfRec2020Options;
  }
  return {};
}

auto EditorParameterCatalog::FilmGrainAfterMethodChange(std::string_view previous_method,
                                                        std::string_view method,
                                                        double           film_grain_ui)
    -> std::optional<double> {
  constexpr std::string_view kFilmPrint = "film_print";
  if (method == kFilmPrint && previous_method != kFilmPrint && !(film_grain_ui > 0.0)) {
    return kFilmPrintFilmGrainUi;
  }
  if (method != kFilmPrint && previous_method == kFilmPrint && film_grain_ui != 0.0) {
    return 0.0;
  }
  return std::nullopt;
}

auto EditorParameterCatalog::KelvinToSliderPosition(double kelvin) -> int {
  return ui::color_temp::CctToSliderPos(static_cast<float>(kelvin));
}

auto EditorParameterCatalog::SliderPositionToKelvin(int position) -> double {
  return static_cast<double>(ui::color_temp::SliderPosToCct(position));
}

auto EditorParameterCatalog::EntryJson(const EditorParameterCatalogEntry& entry) -> json {
  json result = {
      {"field", std::string{entry.field}},
      {"kind", KindText(entry.kind)},
      {"panel", std::string{entry.panel}},
  };
  if (entry.kind == EditorParameterValueKind::Model) {
    result["model_shape"] = std::string{entry.model_shape};
    result["ui_default"]  = entry.ui_default();
  } else if (entry.kind == EditorParameterValueKind::Object) {
    json properties = json::array();
    for (const auto& property : entry.properties) {
      properties.push_back(PropertyJson(property));
    }
    result["properties"] = std::move(properties);
    result["ui_default"] = entry.ui_default();
    if (entry.field == "odt") {
      json by_space = json::object();
      for (const auto& space : kOdtEncodingSpaceOptions) {
        json values = json::array();
        for (const auto& eotf : OdtEotfOptions(space.value)) {
          values.push_back(std::string{eotf.value});
        }
        by_space[std::string{space.value}] = std::move(values);
      }
      result["encoding_eotf_by_space"] = std::move(by_space);
    }
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
