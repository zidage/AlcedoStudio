//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/image_geometry_model.hpp"
#include "edit/operators/models/cat02_white_balance_model.hpp"
#include "edit/operators/models/color_wheel_model.hpp"
#include "edit/operators/models/curve_model.hpp"
#include "edit/operators/models/hls_model.hpp"
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
 * @brief Replacement tone-curve control points for one Curve Model.
 *
 * Points are the edited curve only. They are not a copy of unrelated Grade
 * parameters.
 */
struct EditorCurveWrite {
  std::vector<CurvePoint> points;
};

/**
 * @brief LUT cube path for one LMT Model. Empty path is identity.
 */
struct EditorLutWrite {
  std::string cube_path;
};

/**
 * @brief One field write as a concrete Model operation.
 *
 * The variant enumerates actual editor operations. It is not a generic method
 * name plus argument map. Queue entries hold this value; JSON exists only at
 * history, project, WAL, and remaining QML collection boundaries.
 */
using EditorParameterWrite = std::variant<
    EditorScalarWrite, EditorEnumWrite, EditorToggleWrite, EditorCurveWrite, EditorLutWrite,
    HlsUpdate, ColorWheelUpdate, Cat02WhiteBalanceUpdate, SharpenUpdate, DevelopRawDecodeUpdate,
    DevelopColorTemperatureUpdate, DevelopLensCalibrationUpdate, DrtParameterUpdate,
    ImageGeometryUpdate>;

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
 * @brief Apply one typed field operation to the live document owner.
 *
 * Compound operations are already complete values. The Model update runs once
 * after target validation. Topology and other Models stay intact.
 *
 * @pre Caller holds the shared executor render lock. @p target is complete and
 *      is not Mask.
 * @param document Live document to mutate in place.
 * @param target Explicit node/adjustment identity; no missing identity is inferred.
 * @param write Operation produced by a control or by ParseEditorParameterWrite.
 * @param error Optional failure detail.
 * @return false for an invalid target, type mismatch, or Model rejection.
 */
auto ApplyEditorParameterWrite(PipelineDocument& document, const EditorParameterTarget& target,
                               const EditorParameterWrite& write, std::string* error) -> bool;

}  // namespace alcedo
