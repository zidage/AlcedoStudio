//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <json.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "app/editor_adjustment_pipeline.hpp"

namespace alcedo {

/// Shape of the UI value of one editor field.
enum class EditorParameterValueKind {
  /// One number in the units that the panel slider shows.
  Scalar,
};

/// Conversion between the UI value of a scalar field and its Model value.
enum class EditorScalarUnitConversion {
  /// Model value = UI value.
  Identity,
  /// Saturation: Model value = 1 + UI / 100. UI value = (Model - 1) * 100.
  OffsetFromOnePercent,
  /// Diffusion, film grain, halation strength: Model value = UI / 100. UI value = Model * 100.
  Percent,
};

/// UI range of a scalar field: the values that the panel slider accepts.
struct EditorScalarUiRange {
  double minimum       = 0.0;
  double maximum       = 0.0;
  double default_value = 0.0;
  double step          = 1.0;
  /// Decimal places that the panel shows.
  int    decimals      = 0;
};

/**
 * @brief One editor field in UI units: its range, panel, and Model JSON key.
 *
 * The catalog table is the only source of the UI ranges of these fields. The QML panels and
 * the automation commands read the same entry.
 */
struct EditorParameterCatalogEntry {
  /// Canonical field key, for example `saturation`.
  std::string_view           field;
  /// Other field key that resolves to the same adjustment (`whites` for `white`), or empty.
  std::string_view           alias;
  EditorAdjustmentField      adjustment;
  EditorParameterValueKind   kind;
  /// Adjustment panel id that shows the field (`tone`, `look`, `post`).
  std::string_view           panel;
  /// Key of the scalar Model value in the Model JSON that ParseEditorParameterWrite accepts.
  std::string_view           model_key;
  EditorScalarUnitConversion conversion;
  EditorScalarUiRange        range;
};

/**
 * @brief Static table of editor fields with UI ranges and UI-to-Model conversions.
 *
 * Qt-free. The table does not change at run time, so every function is a pure read.
 * Conversions validate the UI value before conversion: a value outside the UI range is an
 * error with the allowed range. The catalog does not clamp.
 */
class EditorParameterCatalog {
 public:
  /// Every catalog entry in panel order.
  [[nodiscard]] static auto Entries() -> std::span<const EditorParameterCatalogEntry>;

  /// The entry of @p field_key or of its alias. nullptr for a field that is not in the catalog.
  [[nodiscard]] static auto Find(std::string_view field_key) -> const EditorParameterCatalogEntry*;

  /// Model value of the scalar UI value @p ui_value. @p ui_value must be in range.
  [[nodiscard]] static auto ScalarUiToModel(const EditorParameterCatalogEntry& entry,
                                            double ui_value) -> double;

  /// UI value of the scalar Model value @p model_value.
  [[nodiscard]] static auto ScalarModelToUi(const EditorParameterCatalogEntry& entry,
                                            double model_value) -> double;

  /**
   * @brief Check a scalar UI value against the entry range.
   *
   * @return false for a value that is not finite or is outside [minimum, maximum]. @p error
   *         then names the field and the range, for example
   *         `saturation must be in [-100, 100]`.
   */
  [[nodiscard]] static auto ValidateScalarUiValue(const EditorParameterCatalogEntry& entry,
                                                  double ui_value, std::string* error) -> bool;

  /**
   * @brief Convert the UI value of @p field_key to the Model JSON of one parameter write.
   *
   * The result is the object that ParseEditorParameterWrite accepts for the field, for example
   * `{"saturation": 1.2}` for the UI value 20.
   *
   * @param ui_value Number for a scalar field.
   * @param current_model_json Current Model JSON of the field. Scalar fields do not read it.
   * @param error Failure detail: unknown field, wrong type, or the allowed range.
   * @return nullopt on failure. Nothing is written in any case.
   */
  [[nodiscard]] static auto ToModelJson(std::string_view field_key, const nlohmann::json& ui_value,
                                        const nlohmann::json& current_model_json,
                                        std::string* error) -> std::optional<nlohmann::json>;

  /**
   * @brief Convert Model JSON of @p field_key to its UI value.
   *
   * Accepts the Model JSON that ToModelJson returns and the panel projection form: the value
   * under the Model key, under the field key (a number or an object that holds the Model key),
   * or under `value`.
   *
   * @return The UI value (a number for a scalar field), or nullopt with @p error set.
   */
  [[nodiscard]] static auto ToUiValue(std::string_view field_key, const nlohmann::json& model_json,
                                      std::string* error) -> std::optional<nlohmann::json>;

  /// The `editor.catalog` form of @p entry: field, kind, panel, and the UI range.
  [[nodiscard]] static auto EntryJson(const EditorParameterCatalogEntry& entry) -> nlohmann::json;

  /// Every entry in the `editor.catalog` form, in panel order.
  [[nodiscard]] static auto CatalogJson() -> nlohmann::json;
};

}  // namespace alcedo
