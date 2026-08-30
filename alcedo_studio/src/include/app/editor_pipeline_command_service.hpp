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
 * @brief Write one typed parameter patch onto a candidate document.
 *
 * Merges @p params into the current model JSON, then LoadJson. Does not fill a
 * missing target. Caller clones the live document first and discards the
 * candidate on failure.
 *
 * @pre @p target is complete and is not a Mask write.
 * @param candidate Document to mutate.
 * @param target Production write identity.
 * @param params JSON object of model fields (for example exposure_ev).
 * @param error Optional failure text.
 * @return false when the node, instance, or JSON is invalid.
 *
 * Thread: caller owns live-document serialization (editor history queue).
 */
auto ApplyEditorParameterPatch(PipelineDocument& candidate, const EditorParameterTarget& target,
                               const nlohmann::json& params, std::string* error) -> bool;

/**
 * @brief Read the current model JSON for @p target.
 *
 * @return false when the node or instance is missing.
 */
auto ReadEditorParameterJson(const PipelineDocument& document, const EditorParameterTarget& target,
                             nlohmann::json* json, std::string* error) -> bool;

/**
 * @brief Stable JSON dump of @p document for hash comparisons in tests and rollback.
 */
[[nodiscard]] auto CanonicalPipelineDocumentJson(const PipelineDocument& document) -> std::string;

/**
 * @brief True when graph Validate and ValidateImageBackbone are both empty.
 */
auto PipelineDocumentPassesValidation(const PipelineDocument& document, std::string* error) -> bool;

/**
 * @brief Clone, apply, validate, and replace @p live when all steps succeed.
 *
 * On failure @p live is unchanged.
 */
auto PublishEditorParameterPatch(PipelineDocument& live, const EditorParameterTarget& target,
                                 const nlohmann::json& params, std::string* error) -> bool;

}  // namespace alcedo
