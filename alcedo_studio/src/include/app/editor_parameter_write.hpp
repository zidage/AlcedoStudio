//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/image_geometry_model.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
#include "edit/operators/models/lmt_model.hpp"
#include "edit/operators/models/sharpen_model.hpp"
#include "json.hpp"

namespace alcedo {

class PipelineDocument;
struct EditorParameterTarget;

/**
 * @brief Absolute scalar assignment for one operator Model field.
 *
 * @p value is the caller's local control value. The owning Model clamps and
 * normalizes it during the update. This is not a copy of live Model state.
 */
struct EditorScalarWrite {
  float value = 0.0f;
};

/**
 * @brief Absolute string choice from an enum control.
 *
 * Production compound fields (RAW method, ODT method, lens lists) parse the
 * panel's related-field object into the matching update struct instead of
 * storing this type. Tests and unmatched enum controls may enqueue this
 * payload; Model apply rejects it.
 */
struct EditorEnumWrite {
  std::string value;
};

/**
 * @brief Absolute boolean from a toggle control.
 *
 * Production compound fields parse the panel object into the matching update
 * struct. Model apply rejects this payload.
 */
struct EditorToggleWrite {
  bool value = false;
};

/**
 * @brief Replacement control points for some of the four curves of one Curve Model.
 *
 * `points` is the RGB curve; `red`, `green`, and `blue` are the channel curves. A write
 * (@ref ParseEditorParameterWrite) leaves a curve that it does not have as it is. A complete value
 * (@ref ParseEditorParameterValue: history replay) has all four curves: a channel key that the
 * stored Model JSON does not have is the identity.
 */
using EditorCurveWrite = CurveUpdate;

/**
 * @brief LUT selection and/or strength for one LMT Model.
 *
 * A selection sets the reference and its display name and keeps the strength; a strength
 * change keeps the selection. The JSON boundary (history replay, project load) always
 * produces a complete value: a missing strength reads as the 100% default. An empty
 * reference is identity.
 */
using EditorLutWrite       = LmtUpdate;

/**
 * @brief One field write as a concrete Model operation.
 *
 * The variant enumerates actual editor operations. It is not a generic method
 * name plus argument map. Queue entries hold this value; JSON exists only at
 * history, project, WAL, and remaining QML collection boundaries.
 */
using EditorParameterWrite =
    std::variant<EditorScalarWrite, EditorEnumWrite, EditorToggleWrite, EditorCurveWrite,
                 EditorLutWrite, HlsUpdate, ColorWheelUpdate, Cat02WhiteBalanceUpdate,
                 SharpenUpdate, DevelopRawDecodeUpdate, DevelopColorTemperatureUpdate,
                 DevelopLensCalibrationUpdate, DevelopInputProfileUpdate, DrtParameterUpdate,
                 ImageGeometryUpdate, DrtFilmFormatUpdate, DevelopCameraProfileFileUpdate>;

/**
 * @brief Parse a field JSON object into the matching Model operation.
 *
 * This is the history / QML collection boundary. Ordinary live writes should
 * already hold a typed operation and must not round-trip through this function.
 *
 * @pre @p params is a JSON object or null (treated as empty object).
 * @param field_key Current-panel field key, such as `exposure` or `raw_decode`.
 * @param params Field object, including full Model JSON from history replay.
 * @param error Optional failure detail.
 * @return The operation, or nullopt when keys, types, or dimensions are invalid.
 *         No Model is mutated.
 */
[[nodiscard]] auto ParseEditorParameterWrite(std::string_view field_key, const nlohmann::json& params,
                                             std::string* error)
    -> std::optional<EditorParameterWrite>;

/**
 * @brief Parse a complete stored value of one field, as a history head move restores it.
 *
 * A write (@ref ParseEditorParameterWrite) sets only the keys that it has. A complete value sets
 * every field: a key that @p value does not have takes the default of the Model. For `odt`, the
 * Model omits a key of Negative Scan at its default and the film print object of another method,
 * so each film print field that @p value does not have is the default. For `curve`, the Model omits
 * a red, green, or blue curve that is the identity, so each channel curve that @p value does not
 * have is the identity. Other fields read as a write, because their Model JSON has every key.
 *
 * @return The operation, or nullopt when keys, types, or dimensions are invalid.
 */
[[nodiscard]] auto ParseEditorParameterValue(std::string_view field_key, const nlohmann::json& value,
                                             std::string* error)
    -> std::optional<EditorParameterWrite>;

/**
 * @brief Apply one typed field operation to the live document owner.
 *
 * Compound operations are already complete values. The Model update runs once
 * after target validation. Topology and other Models stay intact.
 *
 * @pre Caller is the only writer of @p document (the editor session owner thread for its
 *      working document). @p target is complete and is not Mask.
 * @param document Live document to mutate in place.
 * @param target Explicit node/adjustment identity; no missing identity is inferred.
 * @param write Operation produced by a control or by ParseEditorParameterWrite.
 * @param error Optional failure detail.
 * @return false for an invalid target, type mismatch, or Model rejection.
 */
auto ApplyEditorParameterWrite(PipelineDocument& document, const EditorParameterTarget& target,
                               const EditorParameterWrite& write, std::string* error) -> bool;

}  // namespace alcedo
