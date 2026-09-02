//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <functional>
#include <string>

#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo {

class MaskStore;

/**
 * @brief Optional apply observers and Mask-asset resolution.
 *
 * @p after_successful_change runs after each applied change while the caller
 * still holds the render lock. Tests use it to observe intermediate documents
 * and to inject a later-change failure. @p mask_store is required for
 * @ref ReplaceMaskAssetChange so both stored keys can be loaded.
 */
struct PipelineHistoryApplyContext {
  MaskStore* mask_store = nullptr;
  std::function<void(std::size_t applied_count)> after_successful_change;
};

/**
 * @brief Apply one typed batch to the live document in @p direction.
 *
 * Validates the expected current side of each change before mutation. If change
 * @c n fails, inverse-applies changes @c n-1 through @c 1. Does not own UI,
 * storage, or rendering. Does not take the render lock; the caller must hold it.
 *
 * @param document Live writable document for the image.
 * @param batch Validated typed batch. Empty batches are rejected by Validate.
 * @param direction Forward uses stored order and after values. Inverse reverses
 *        both order and before/after sides.
 * @param error Optional failure detail, including restoration errors.
 * @param context Optional Mask store and apply observer.
 * @return false when validation, apply, or rollback fails. On false the document
 *         matches the pre-call hash unless inverse restoration itself fails.
 * @pre Caller holds the shared executor render lock.
 */
auto ApplyPipelineEditBatch(PipelineDocument& document, const PipelineEditBatch& batch,
                            PipelineEditApplyDirection direction, std::string* error,
                            const PipelineHistoryApplyContext& context = {}) -> bool;

}  // namespace alcedo
