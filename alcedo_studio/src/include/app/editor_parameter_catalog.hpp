//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
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
  /// An object of named properties in panel units. A write may give some of the properties.
  Object,
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

/// Value type of one property of an object field.
enum class EditorParameterPropertyType {
  Number,
  Boolean,
  String,
  /// A string from the property option list.
  Option,
};

/// One choice of an option property. @p label is the English panel text; QML translates it.
struct EditorParameterOption {
  std::string_view value;
  std::string_view label;
};

/// Position scale of the slider that shows a number property.
enum class EditorParameterSliderScale {
  /// The slider position is the value.
  Linear,
  /// Kelvin on the RAW white balance track: positions 0..4096, linear on each side of the
  /// 6000 K pivot at position 2048 (EditorParameterCatalog::KelvinToSliderPosition).
  KelvinPivot,
};

/// One property of an object field in UI units.
struct EditorParameterProperty {
  std::string_view                       name;
  EditorParameterPropertyType            type = EditorParameterPropertyType::Number;
  /// Option: the allowed values in menu order.
  std::span<const EditorParameterOption> options{};
  /// Number: the range, inclusive, the slider step, and the decimal places that the panel shows.
  double                                 minimum  = 0.0;
  double                                 maximum  = 0.0;
  double                                 step     = 0.0;
  int                                    decimals = 0;
  EditorParameterSliderScale             slider   = EditorParameterSliderScale::Linear;
};

/// Size of the presented source image in reference pixels. Zero when no frame is presented.
/// The crop field uses it for the orientation of fixed aspect presets and for the
/// rotated-source constraint.
struct EditorParameterSource {
  std::uint32_t width  = 0;
  std::uint32_t height = 0;
};

/// Input change that the crop constraint follows (the control that the user changed).
enum class EditorCropDriver {
  None,
  Position,
  Width,
  Height,
  Rotation,
  /// A preset choice. A fixed preset sets the aspect width and height to its ratio.
  AspectPreset,
  /// An aspect width or height edit. The preset becomes `custom`.
  AspectSize,
};

/**
 * @brief One editor field in UI units: its range or properties, panel, and Model JSON form.
 *
 * The catalog table is the only source of the UI ranges, defaults, and option lists of these
 * fields. The QML panels and the automation commands read the same entry.
 */
struct EditorParameterCatalogEntry {
  /// Canonical field key, for example `saturation`.
  std::string_view           field;
  /// Other field key that resolves to the same adjustment (`whites` for `white`), or empty.
  std::string_view           alias;
  EditorAdjustmentField      adjustment;
  EditorParameterValueKind   kind;
  /// Adjustment panel id that shows the field (`tone`, `look`, `post`, `raw`, `geometry`).
  std::string_view           panel;
  /// Scalar: key of the Model value in the Model JSON that ParseEditorParameterWrite accepts.
  std::string_view           model_key  = {};
  EditorScalarUnitConversion conversion = EditorScalarUnitConversion::Identity;
  EditorScalarUiRange        range      = {};
  /// Object: the properties of the UI value.
  std::span<const EditorParameterProperty> properties = {};
  /// Object: the UI value of the field default.
  auto (*ui_default)() -> nlohmann::json = nullptr;
  /// Object: the UI value of field Model JSON or of its panel projection form.
  auto (*model_to_ui)(const nlohmann::json& model_json, std::string* error)
      -> std::optional<nlohmann::json> = nullptr;
  /// Object: the Model JSON of a complete, valid UI value.
  auto (*ui_to_model)(const nlohmann::json& ui_value) -> nlohmann::json = nullptr;
  /// Object, optional: constraint that runs on the merged UI value before ui_to_model.
  /// @p ui_change is the partial value that the caller wrote; @p current_ui is the complete UI
  /// value before the change.
  auto (*constrain)(const nlohmann::json& merged_ui, const nlohmann::json& ui_change,
                    const nlohmann::json& current_ui, const EditorParameterSource& source,
                    std::string* error) -> std::optional<nlohmann::json> = nullptr;
};

/**
 * @brief Static table of editor fields with UI ranges and UI-to-Model conversions.
 *
 * Qt-free. The table does not change at run time, so every function is a pure read.
 * Conversions validate the UI value before conversion: a value outside the UI range, an
 * unknown property, or an option that is not in the list is an error that names the allowed
 * values. The catalog does not clamp.
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
   * @brief Check the properties of an object UI value against the entry.
   *
   * Every key must be a property of the entry and must have its type, range, and option list.
   * A value may omit properties. The error names the property and the allowed values, for
   * example `crop_rotate.width must be in [0.0001, 1]`.
   */
  [[nodiscard]] static auto ValidateObjectUiValue(const EditorParameterCatalogEntry& entry,
                                                  const nlohmann::json& ui_value,
                                                  std::string*          error) -> bool;

  /**
   * @brief Convert the UI value of @p field_key to the Model JSON of one parameter write.
   *
   * The result is the object that ParseEditorParameterWrite accepts for the field, for example
   * `{"saturation": 1.2}` for the UI value 20. An object value may give some of the properties:
   * they replace the matching properties of @p current_model_json (the field default when it is
   * null), the entry constraint runs, and the result is one complete write. The crop constraint
   * can move or resize the frame (aspect lock, rotated source); the written value is the result.
   *
   * @param ui_value Number for a scalar field, object for an object field.
   * @param current_model_json Current Model JSON of the field, or its panel projection form.
   *        Scalar fields do not read it. Null means the field default.
   * @param source Presented source size, for the crop constraint.
   * @param error Failure detail: unknown field, wrong type, or the allowed values.
   * @return nullopt on failure. Nothing is written in any case.
   */
  [[nodiscard]] static auto ToModelJson(std::string_view field_key, const nlohmann::json& ui_value,
                                        const nlohmann::json& current_model_json,
                                        std::string*          error) -> std::optional<nlohmann::json>;
  [[nodiscard]] static auto ToModelJson(std::string_view field_key, const nlohmann::json& ui_value,
                                        const nlohmann::json&        current_model_json,
                                        const EditorParameterSource& source, std::string* error)
      -> std::optional<nlohmann::json>;

  /**
   * @brief Convert a complete object UI value to Model JSON without a merge or a constraint.
   *
   * The panels use it after they applied the constraint to their own values. Missing
   * properties take the field default.
   */
  [[nodiscard]] static auto UiStateToModelJson(std::string_view      field_key,
                                               const nlohmann::json& ui_value, std::string* error)
      -> std::optional<nlohmann::json>;

  /**
   * @brief Convert Model JSON of @p field_key to its UI value.
   *
   * Accepts the Model JSON that ToModelJson returns and the panel projection form. For a
   * scalar: the value under the Model key, under the field key (a number or an object that
   * holds the Model key), or under `value`. For an object: the wrapped or flat Model object.
   *
   * @return The UI value, or nullopt with @p error set.
   */
  [[nodiscard]] static auto ToUiValue(std::string_view field_key, const nlohmann::json& model_json,
                                      std::string* error) -> std::optional<nlohmann::json>;

  /// UI value of the field default: the scalar default or the default object.
  [[nodiscard]] static auto UiDefault(const EditorParameterCatalogEntry& entry) -> nlohmann::json;

  /**
   * @brief Apply the Geometry panel constraint to a complete crop UI value.
   *
   * The sequence of the panel: an aspect preset choice sets the aspect size, an aspect size
   * edit selects `custom`, a locked aspect fits or resizes the crop rectangle for the change,
   * and the rotated-source constraint keeps every frame corner inside the source.
   */
  [[nodiscard]] static auto ConstrainCrop(const nlohmann::json& crop_ui, EditorCropDriver driver,
                                          const EditorParameterSource& source) -> nlohmann::json;

  /// Width / height of the crop frame for a locked aspect, oriented to the source (a 16:9
  /// preset is 9:16 for a portrait source). nullopt when the aspect is not locked.
  [[nodiscard]] static auto CropLockedAspectRatio(const nlohmann::json&        crop_ui,
                                                  const EditorParameterSource& source)
      -> std::optional<double>;

  /// The EOTF choices of the display transform for @p encoding_space, in menu order. Empty for
  /// a space that is not an `odt.encoding_space` option.
  [[nodiscard]] static auto OdtEotfOptions(std::string_view encoding_space)
      -> std::span<const EditorParameterOption>;

  /// Slider position (0..4096) of @p kelvin on the white balance track. 6000 K is position 2048.
  [[nodiscard]] static auto KelvinToSliderPosition(double kelvin) -> int;

  /// Kelvin at slider position @p position of the white balance track.
  [[nodiscard]] static auto SliderPositionToKelvin(int position) -> double;

  /// The `editor.catalog` form of @p entry: field, kind, panel, and the UI range or schema.
  [[nodiscard]] static auto EntryJson(const EditorParameterCatalogEntry& entry) -> nlohmann::json;

  /// Every entry in the `editor.catalog` form, in panel order.
  [[nodiscard]] static auto CatalogJson() -> nlohmann::json;
};

}  // namespace alcedo
