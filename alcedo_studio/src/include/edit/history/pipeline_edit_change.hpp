//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string_view>

#include "edit/history/pipeline_edit_batch.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Reject incomplete identity, non-canonical nested JSON, and illegal values
 *        on one typed change.
 *
 * Does not mutate a document. Used by @ref PipelineEditBatch::Validate and by
 * @ref DecodePipelineEditChange after JSON collection.
 *
 * @throws std::runtime_error on the first failed rule.
 */
void               ValidatePipelineEditChange(const PipelineEditChange& change);

/**
 * @brief Encode one typed change to canonical JSON, including the `kind` field.
 *
 * @pre @ref ValidatePipelineEditChange succeeds for @p change.
 */
[[nodiscard]] auto EncodePipelineEditChange(const PipelineEditChange& change) -> nlohmann::json;

/**
 * @brief Parse one typed change from canonical JSON and validate it.
 *
 * Unknown keys, unknown kinds, and non-canonical nested payloads are rejected.
 *
 * @throws std::runtime_error when @p json is not a canonical typed change.
 */
[[nodiscard]] auto DecodePipelineEditChange(const nlohmann::json& json) -> PipelineEditChange;

/**
 * @brief True when @p change may appear on a batch with @p operation.
 *
 * Paste accepts every change except @ref PipelineEditChangeKind::NodeGraphTopologyChange.
 */
[[nodiscard]] auto PipelineEditChangeCompatible(PipelineEditOperationKind operation,
                                                PipelineEditChangeKind    change) -> bool;

/**
 * @brief Parse a stored change `kind` string.
 *
 * @throws std::runtime_error when @p text is not a published change kind.
 */
[[nodiscard]] auto PipelineEditChangeKindFromText(std::string_view text) -> PipelineEditChangeKind;

}  // namespace alcedo
