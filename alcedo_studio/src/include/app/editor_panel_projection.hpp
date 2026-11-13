//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "app/editor_adjustment_types.hpp"
#include "edit/geometry/types.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/image_geometry_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/lut_reference.hpp"

namespace alcedo {

class PipelineDocument;

/// Tone/Look scalar shown under a QML field key such as `exposure`.
struct EditorPanelScalarValue {
  std::string display_key;
  float       value = 0.0f;
};

/// Nested strength/offset such as film_grain.strength or sharpen.offset.
struct EditorPanelNestedScalarValue {
  std::string object_key;
  std::string value_key;
  float       value = 0.0f;
};

/// The LMT association of the exact Color Grade: user intent only. Whether the referenced
/// file is currently available is resolved by the LUT library, not by the document.
struct EditorPanelLutValue {
  LutReference reference;
  std::string  display_name;
  float        strength = 1.0f;
  /// Catalog encoding ids (color_encoding_catalog.hpp) of the LUT input and output.
  std::string  input_encoding{kDefaultLutEncodingId};
  std::string  output_encoding{kDefaultLutEncodingId};
};

/// The four curves of a Curve Model: `points` is the RGB curve.
struct EditorPanelCurveValue {
  std::vector<CurvePoint> points;
  std::vector<CurvePoint> red;
  std::vector<CurvePoint> green;
  std::vector<CurvePoint> blue;
};

struct EditorPanelHlsValue {
  std::array<HlsVec3, kHlsHueBinCount> hls_adj_table{};
  std::array<float, kHlsHueBinCount>   h_range_table{};
  HlsVec3                              target_hls{};
};

struct EditorPanelColorWheelValue {
  ColorWheelControl lift{};
  ColorWheelControl gamma{};
  ColorWheelControl gain{};
  ColorWheelControl offset{};
};

struct EditorPanelColorTempValue {
  std::string mode         = "as_shot";
  float       custom_cct   = 6500.0f;
  float       custom_tint  = 0.0f;
  float       as_shot_cct  = 6500.0f;
  float       as_shot_tint = 0.0f;
};

/// Color Grade CAT02 white balance: illuminant CCT (Kelvin) and tint on the RAW WB scale.
struct EditorPanelGradeWhiteBalanceValue {
  float temperature = 0.0f;
  float tint        = 0.0f;
};

struct EditorPanelRawDecodeValue {
  std::string method                  = "default";
  bool        highlights_reconstruct  = true;
};

struct EditorPanelOdtValue {
  std::string method           = "open_drt";
  std::string encoding_space   = "rec709";
  std::string encoding_eotf    = "gamma_2_2";
  std::string limiting_space   = "rec709";
  float       peak_luminance   = 100.0f;
  std::string look_preset      = "standard";
  std::string tonescale_preset = "use_look_preset";
  std::string creative_white   = "use_look_preset";
  FilmPrintParams film_print{};
  /// True when the document has a RAW white balance for scene light As Shot (`RawWhiteBalanceCct`
  /// has a value). The panel shows As Shot only then (issue 672). Not a Model field.
  bool            film_as_shot_available = false;
};

struct EditorPanelLensValue {
  bool        enabled = false;
  std::string lens_maker;
  std::string lens_model;
};

/// Develop `input` object of a raster document. `raster` is false for a RAW document, whose panel
/// shows the RAW controls instead.
struct EditorPanelInputProfileValue {
  bool        raster           = false;
  std::string profile_override = "auto";
  /// ICC 'desc' text of the source profile; empty when no ICC profile was used.
  std::string profile_description;
  /// Serialized RasterColorOrigin, for example "icc_matrix_shaper" or "default_srgb".
  std::string origin;
  /// "display_referred" or "scene_linear".
  std::string referral;
};

/// DCP profile reference of the Develop node (`camera_profile_file`). `selected` is false when the
/// import profile applies. `resolved` is false for a stored reference that the DCP library could
/// not bind, whose render fails (plan decision P-4). `raster` is true for a raster document, which
/// has no camera profile. `embedded_default` is true when the import profile is the embedded
/// profile of a DNG file (the default entry reads "Embedded", else "Built-in"). `has_tone_curve`
/// is true when the bound profile has a `ProfileToneCurve`, and `profile_tone` is the stored
/// switch of the profile tone stage (dcp_profile_tone_curve_study.md, section 6.4); both are
/// false without a reference.
struct EditorPanelCameraProfileFileValue {
  bool        raster           = false;
  bool        embedded_default = false;
  bool        selected         = false;
  bool        resolved         = false;
  bool        has_tone_curve   = false;
  bool        profile_tone     = false;
  std::string camera_model;
  std::string profile_name;
};

/// The film format of the DRT node, as its document identifier (`16mm`, `35mm`, or `70mm`).
struct EditorPanelFilmFormatValue {
  std::string film_format = "35mm";
};

struct EditorPanelGeometryValue {
  NormalizedRect  crop_rect{};
  float           rotation_degrees = 0.0f;
  std::string     aspect_preset;
  CropAspectRatio aspect_ratio{};
};

using EditorPanelFieldValue =
    std::variant<EditorPanelScalarValue, EditorPanelNestedScalarValue, EditorPanelLutValue,
                 EditorPanelCurveValue, EditorPanelHlsValue, EditorPanelColorWheelValue,
                 EditorPanelColorTempValue, EditorPanelRawDecodeValue, EditorPanelOdtValue,
                 EditorPanelLensValue, EditorPanelGeometryValue, EditorPanelGradeWhiteBalanceValue,
                 EditorPanelInputProfileValue, EditorPanelFilmFormatValue,
                 EditorPanelCameraProfileFileValue>;

/**
 * @brief One panel field copied from a Graph Node Model.
 *
 * @p source names the NodeId / AdjustmentInstanceId that was read. This is not a
 * live Model pointer and is not a writable parameter mirror.
 */
struct EditorPanelFieldPresentation {
  std::string             field_key;
  EditorParameterTarget   source{};
  EditorPanelFieldValue   value{};
};

/**
 * @brief Load-only panel values copied at the session/render owner boundary.
 *
 * Displayed fields: @p fields, each a named panel value plus the instance that
 * produced it. Source owner: PipelineDocument, read on the thread that writes it (the
 * editor session owner thread). Consistency: every field was read in one owner-thread
 * step. GUI delivery: this struct is a value copy with no Model pointers.
 * Drop it when @p session_generation does not match the live session, or when a
 * newer copy for the same session replaces it.
 */
struct EditorPanelProjection {
  std::uint64_t                              session_generation = 0;
  std::vector<EditorPanelFieldPresentation>  fields;
};

/**
 * @brief Concrete panel read: supported field, destination panel, Model getter.
 *
 * A new panel adds one of these. It does not extend a global JSON parser.
 */
struct EditorPanelAdapter {
  std::string_view field_key;
  std::string_view panel_id;
  auto (*read)(const PipelineDocument& document, const EditorParameterTarget& target,
               EditorPanelFieldPresentation* out, std::string* error) -> bool;
  /// True when the field is owned by a Color Grade node; the current-panel
  /// projection omits it when the document has no Color Grade.
  bool requires_color_grade = false;
};

/**
 * @brief Ordered adapter list used by panel projection.
 *
 * Production() is the built-in editor panels. Add() registers one more adapter
 * on a copy; it does not mutate the process-wide production table.
 */
class EditorPanelAdapterTable {
 public:
  [[nodiscard]] static auto Production() -> EditorPanelAdapterTable;

  void Add(EditorPanelAdapter adapter);

  [[nodiscard]] auto Find(std::string_view field_key) const -> const EditorPanelAdapter*;
  [[nodiscard]] auto Adapters() const -> std::span<const EditorPanelAdapter>;

 private:
  std::vector<EditorPanelAdapter> adapters_;
};

/**
 * @brief True when @p projection was produced for the live session generation.
 *
 * @param projection Copied panel values. No Model pointers.
 * @param session_generation Live session identity, typically the active image
 *        load request. Independent of history revision.
 */
[[nodiscard]] auto EditorPanelProjectionIsCurrent(const EditorPanelProjection& projection,
                                                  std::uint64_t session_generation) -> bool;

/**
 * @brief Replace or insert one field in @p projection by field_key.
 *
 * Used after a single live edit so only that field is re-read. Other fields
 * keep their previously copied values.
 */
void UpsertEditorPanelField(EditorPanelProjection* projection, EditorPanelFieldPresentation field);

/**
 * @brief Read one panel field from the Graph Node identified by @p target.
 *
 * Looks up the actual AdjustmentInstanceId on @p target. Does not infer
 * PrimaryGrade or the first instance of an operator type.
 *
 * @pre No other thread writes @p document during the call.
 * @pre @p target is a complete production target.
 * @return false when the node, instance, or adapter is missing. @p out is
 *         left unchanged.
 */
auto ReadEditorPanelField(const PipelineDocument& document, const EditorParameterTarget& target,
                          EditorPanelFieldPresentation* out, std::string* error) -> bool;

auto ReadEditorPanelField(const PipelineDocument& document, const EditorParameterTarget& target,
                          const EditorPanelAdapterTable& table, EditorPanelFieldPresentation* out,
                          std::string* error) -> bool;

/**
 * @brief Current-panel NodeId / instance ids for every production adapter.
 *
 * This is the existing-context helper. Selected-node routing supplies its own
 * target list to ProjectEditorPanelFields instead of calling this function.
 *
 * @return nullopt when a production panel owner is missing.
 */
[[nodiscard]] auto CurrentPanelProjectionTargets(const PipelineDocument& document,
                                                 std::string* error)
    -> std::optional<std::vector<EditorParameterTarget>>;

/**
 * @brief Read every supplied target in one owner-scoped pass.
 *
 * Failure leaves @p out unchanged. Missing or wrong-owner instances fail
 * explicitly; PrimaryGrade is not substituted.
 *
 * @pre No other thread writes @p document during the call.
 */
auto ProjectEditorPanelFields(const PipelineDocument& document,
                              std::span<const EditorParameterTarget> targets,
                              std::uint64_t session_generation, EditorPanelProjection* out,
                              std::string* error) -> bool;

auto ProjectEditorPanelFields(const PipelineDocument& document,
                              std::span<const EditorParameterTarget> targets,
                              std::uint64_t session_generation, const EditorPanelAdapterTable& table,
                              EditorPanelProjection* out, std::string* error) -> bool;

/**
 * @brief Project every production panel field from current-panel owners.
 *
 * @pre No other thread writes @p document during the call.
 */
auto ProjectCurrentPanelFields(const PipelineDocument& document, std::uint64_t session_generation,
                               EditorPanelProjection* out, std::string* error) -> bool;

}  // namespace alcedo
