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
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"

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

struct EditorPanelLutValue {
  std::string cube_path;
};

struct EditorPanelCurveValue {
  std::vector<CurvePoint> points;
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
};

struct EditorPanelColorTempValue {
  std::string mode         = "as_shot";
  float       custom_cct   = 6500.0f;
  float       custom_tint  = 0.0f;
  float       as_shot_cct  = 6500.0f;
  float       as_shot_tint = 0.0f;
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
};

struct EditorPanelLensValue {
  bool enabled = false;
};

struct EditorPanelGeometryValue {
  NormalizedRect crop_rect{};
  float          rotation_degrees = 0.0f;
  bool           expand_to_fit    = true;
};

using EditorPanelFieldValue =
    std::variant<EditorPanelScalarValue, EditorPanelNestedScalarValue, EditorPanelLutValue,
                 EditorPanelCurveValue, EditorPanelHlsValue, EditorPanelColorWheelValue,
                 EditorPanelColorTempValue, EditorPanelRawDecodeValue, EditorPanelOdtValue,
                 EditorPanelLensValue, EditorPanelGeometryValue>;

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
 * produced it. Source owner: PipelineDocument, read while the caller holds the
 * executor render lock. Consistency: every field was read before that lock was
 * released. GUI delivery: this struct is a value copy with no Model pointers.
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
 * @pre Caller holds the executor render lock when @p document is live.
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
 * @pre Caller holds the executor render lock when @p document is live.
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
 * @pre Caller holds the executor render lock when @p document is live.
 */
auto ProjectCurrentPanelFields(const PipelineDocument& document, std::uint64_t session_generation,
                               EditorPanelProjection* out, std::string* error) -> bool;

}  // namespace alcedo
