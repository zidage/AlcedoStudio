//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_parameter_catalog.hpp"

#include <array>
#include <cmath>
#include <sstream>
#include <string>
#include <utility>

namespace alcedo {
namespace {

using Conversion = EditorScalarUnitConversion;

constexpr EditorScalarUiRange kSignedPercentRange{-100.0, 100.0, 0.0, 1.0, 0};
constexpr EditorScalarUiRange kPercentRange{0.0, 100.0, 0.0, 1.0, 0};

constexpr auto Scalar(std::string_view field, std::string_view alias,
                      EditorAdjustmentField adjustment, std::string_view panel,
                      std::string_view model_key, Conversion conversion, EditorScalarUiRange range)
    -> EditorParameterCatalogEntry {
  return EditorParameterCatalogEntry{field,     alias,      adjustment, EditorParameterValueKind::Scalar,
                                     panel,     model_key,  conversion, range};
}

// Values of the Tone, Look, and Post Processing panel sliders. The exposure default is the
// neutral reset value; the product Default document starts at +1.5 EV.
constexpr std::array kEntries = {
    Scalar("exposure", {}, EditorAdjustmentField::Exposure, "tone", "exposure_ev",
           Conversion::Identity, EditorScalarUiRange{-10.0, 10.0, 0.0, 0.01, 2}),
    Scalar("contrast", {}, EditorAdjustmentField::Contrast, "tone", "contrast",
           Conversion::Identity, kSignedPercentRange),
    Scalar("highlights", {}, EditorAdjustmentField::Highlights, "tone", "highlights",
           Conversion::Identity, kSignedPercentRange),
    Scalar("shadows", {}, EditorAdjustmentField::Shadows, "tone", "shadows", Conversion::Identity,
           kSignedPercentRange),
    Scalar("white", "whites", EditorAdjustmentField::Whites, "tone", "white", Conversion::Identity,
           kSignedPercentRange),
    Scalar("black", "blacks", EditorAdjustmentField::Blacks, "tone", "black", Conversion::Identity,
           kSignedPercentRange),
    Scalar("saturation", {}, EditorAdjustmentField::Saturation, "look", "saturation",
           Conversion::OffsetFromOnePercent, kSignedPercentRange),
    Scalar("vibrance", {}, EditorAdjustmentField::Vibrance, "look", "vibrance",
           Conversion::Identity, kSignedPercentRange),
    Scalar("diffusion", {}, EditorAdjustmentField::Diffusion, "post", "strength",
           Conversion::Percent, kPercentRange),
    Scalar("clarity", {}, EditorAdjustmentField::Clarity, "post", "clarity", Conversion::Identity,
           kSignedPercentRange),
    Scalar("sharpen", {}, EditorAdjustmentField::Sharpen, "post", "offset", Conversion::Identity,
           kPercentRange),
    Scalar("film_grain", {}, EditorAdjustmentField::FilmGrain, "post", "strength",
           Conversion::Percent, kPercentRange),
    Scalar("halation", {}, EditorAdjustmentField::Halation, "post", "strength",
           Conversion::Percent, kPercentRange),
};

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

auto RangeText(const EditorScalarUiRange& range) -> std::string {
  return "[" + FormatNumber(range.minimum) + ", " + FormatNumber(range.maximum) + "]";
}

auto KindText(EditorParameterValueKind kind) -> const char* {
  switch (kind) {
    case EditorParameterValueKind::Scalar:
    default:
      return "scalar";
  }
}

/// The scalar Model value in @p model_json, or nullptr. See EditorParameterCatalog::ToUiValue.
auto FindScalarModelValue(const EditorParameterCatalogEntry& entry,
                          const nlohmann::json&              model_json) -> const nlohmann::json* {
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

}  // namespace

auto EditorParameterCatalog::Entries() -> std::span<const EditorParameterCatalogEntry> {
  return kEntries;
}

auto EditorParameterCatalog::Find(std::string_view field_key)
    -> const EditorParameterCatalogEntry* {
  for (const auto& entry : kEntries) {
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

auto EditorParameterCatalog::ToModelJson(std::string_view field_key, const nlohmann::json& ui_value,
                                         const nlohmann::json& current_model_json,
                                         std::string* error) -> std::optional<nlohmann::json> {
  static_cast<void>(current_model_json);
  const auto* entry = Find(field_key);
  if (entry == nullptr) {
    SetError(error, "Unknown editor parameter field: " + std::string{field_key});
    return std::nullopt;
  }
  if (!ui_value.is_number()) {
    SetError(error, std::string{entry->field} + " must be a number in " + RangeText(entry->range));
    return std::nullopt;
  }
  const double value = ui_value.get<double>();
  if (!ValidateScalarUiValue(*entry, value, error)) {
    return std::nullopt;
  }
  nlohmann::json model = nlohmann::json::object();
  model[std::string{entry->model_key}] = ScalarUiToModel(*entry, value);
  return model;
}

auto EditorParameterCatalog::ToUiValue(std::string_view field_key, const nlohmann::json& model_json,
                                       std::string* error) -> std::optional<nlohmann::json> {
  const auto* entry = Find(field_key);
  if (entry == nullptr) {
    SetError(error, "Unknown editor parameter field: " + std::string{field_key});
    return std::nullopt;
  }
  const auto* value = FindScalarModelValue(*entry, model_json);
  if (value == nullptr || !value->is_number() || !std::isfinite(value->get<double>())) {
    SetError(error, std::string{entry->field} + " Model JSON has no finite " +
                        std::string{entry->model_key} + " value");
    return std::nullopt;
  }
  return nlohmann::json(ScalarModelToUi(*entry, value->get<double>()));
}

auto EditorParameterCatalog::EntryJson(const EditorParameterCatalogEntry& entry)
    -> nlohmann::json {
  nlohmann::json json = {
      {"field", std::string{entry.field}},
      {"kind", KindText(entry.kind)},
      {"panel", std::string{entry.panel}},
      {"ui_min", entry.range.minimum},
      {"ui_max", entry.range.maximum},
      {"ui_default", entry.range.default_value},
      {"ui_step", entry.range.step},
      {"ui_decimals", entry.range.decimals},
  };
  if (!entry.alias.empty()) {
    json["aliases"] = nlohmann::json::array({std::string{entry.alias}});
  }
  return json;
}

auto EditorParameterCatalog::CatalogJson() -> nlohmann::json {
  nlohmann::json fields = nlohmann::json::array();
  for (const auto& entry : kEntries) {
    fields.push_back(EntryJson(entry));
  }
  return fields;
}

}  // namespace alcedo
