//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <optional>
#include <string>

#include "app/editor_adjustment_types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Validate and apply one parameter patch to the existing live Model.
 *
 * The JSON input is the application/history boundary only. Supplied keys, types, finite numbers,
 * and array dimensions are checked before a typed Model update is built; the update then reaches
 * the owning Model through its focused operation without a full Model JSON read/merge/reload.
 * Model operations normalize values and update dirty bits; topology and other Models stay intact.
 * Compound input is parsed completely before any owner field is changed.
 * @pre Caller holds the shared executor render lock; target is complete and is not Mask.
 * @param document Live document to mutate in place.
 * @param target Explicit node/adjustment identity; no missing identity is inferred.
 * @param params Field-specific JSON boundary object. Persistence/history JSON is accepted at this
 *        boundary and converted to a typed operation before mutation.
 * @param error Optional failure detail.
 * @return false for an invalid target or parameter; no history or persistence is performed.
 */
auto ApplyEditorParameterPatch(PipelineDocument& document, const EditorParameterTarget& target,
                               const nlohmann::json& params, std::string* error) -> bool;

/**
 * @brief Read the current model JSON for @p target at the persistence/history boundary.
 *
 * ApplyEditorParameterPatch does not use this function to update a live Model. It remains the
 * JSON boundary required by history, project transfer, and persistence callers.
 *
 * @return false when the node or instance is missing.
 */
auto ReadEditorParameterJson(const PipelineDocument& document, const EditorParameterTarget& target,
                             nlohmann::json* json, std::string* error) -> bool;

/**
 * @brief Stable JSON dump of @p document for comparisons in tests.
 */
[[nodiscard]] auto CanonicalPipelineDocumentJson(const PipelineDocument& document) -> std::string;

/**
 * @brief True when graph Validate and ValidateImageBackbone are both empty.
 */
auto PipelineDocumentPassesValidation(const PipelineDocument& document, std::string* error) -> bool;

/**
 * @brief Fill a current-panel target from @p field_key and the live document.
 *
 * Ordinary Grade fields target the Default Grade (`grade.primary`, else the first
 * backbone Grade). Clarity, Sharpen, Halation, and Film Grain target DRT/Post.
 * Develop, geometry, and `odt` target their existing owners. Instance ids come from
 * the live Models, not from concatenating the field key.
 *
 * @param document Live document used to resolve node and instance ids.
 * @param field_key Current-panel field key, such as `exposure` or `clarity`.
 * @param error Optional failure detail.
 * @return Complete target, or nullopt when the field or owner Model is missing.
 */
[[nodiscard]] auto CompleteCurrentPanelParameterTarget(const PipelineDocument& document,
                                                       std::string field_key, std::string* error)
    -> std::optional<EditorParameterTarget>;

}  // namespace alcedo
