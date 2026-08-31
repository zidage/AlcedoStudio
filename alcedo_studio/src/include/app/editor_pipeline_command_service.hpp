//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>

#include "app/editor_adjustment_types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Validate and apply one parameter patch to the existing live Model.
 *
 * Supplied keys, types, finite numbers and array dimensions are checked before setters.
 * Model setters normalize values and update dirty bits; topology and other Models stay intact.
 * A throwing setter restores only the affected Model's previous parameters.
 * @pre Caller holds the shared executor render lock; target is complete and is not Mask.
 * @param document Live document to mutate in place.
 * @param target Explicit node/adjustment identity; no missing identity is inferred.
 * @param params Partial Model JSON object.
 * @param error Optional failure detail, including restoration errors.
 * @return false for an invalid target or parameter; no history or persistence is performed.
 */
auto ApplyEditorParameterPatch(PipelineDocument& document, const EditorParameterTarget& target,
                               const nlohmann::json& params, std::string* error) -> bool;

/**
 * @brief Read the current model JSON for @p target.
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
 * @brief Apply a validated patch in place; equivalent to ApplyEditorParameterPatch.
 *
 * @pre Caller holds the shared executor render lock. No graph validation or copy occurs.
 */
auto PublishEditorParameterPatch(PipelineDocument& live, const EditorParameterTarget& target,
                                 const nlohmann::json& params, std::string* error) -> bool;

}  // namespace alcedo
