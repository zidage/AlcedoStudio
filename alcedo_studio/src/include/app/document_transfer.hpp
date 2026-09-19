//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "json.hpp"

namespace alcedo {

/**
 * @brief Capture transferable Color Grades, Masks, and DRT/Post from @p document.
 *
 * Omits Develop, RAW metadata, geometry, history, Version ids, and UI state.
 *
 * @param document Source DAG. Must have at least one Color Grade on the backbone.
 * @return Validated package with a computed fingerprint.
 * @throws std::runtime_error when the document, owners, or referenced assets fail.
 */
[[nodiscard]] auto CaptureDocumentTransfer(const PipelineDocument& document)
    -> AdjustmentTransferPackage;

/**
 * @brief Parse a portable transfer document. Rejects operator-list packages,
 *        unknown keys, unknown versions, and non-canonical dumps.
 *
 * @throws std::runtime_error when @p json is not a canonical transfer document.
 */
[[nodiscard]] auto ImportDocumentTransfer(const nlohmann::json& json) -> AdjustmentTransferPackage;

/**
 * @brief Canonical JSON for @p package. Key order is stable. Includes fingerprint.
 */
[[nodiscard]] auto ExportDocumentTransfer(const AdjustmentTransferPackage& package)
    -> nlohmann::json;

/**
 * @brief Hash of the canonical package without the fingerprint field.
 */
[[nodiscard]] auto DocumentTransferFingerprint(const AdjustmentTransferPackage& package)
    -> std::string;

/**
 * @brief Validate package schema, Grade JSON, DRT/Post JSON, and asset descriptors.
 *
 * @throws std::runtime_error on the first failed rule. Does not mutate @p package.
 */
void ValidateDocumentTransfer(const AdjustmentTransferPackage& package);

}  // namespace alcedo
