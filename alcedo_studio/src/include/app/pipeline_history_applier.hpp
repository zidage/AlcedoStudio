//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/edit_commit.hpp"
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

/**
 * @brief Apply one leftover ordinary payload onto the current-panel document Models.
 *
 * Maps CPU operator JSON aliases (`exposure` → `exposure_ev`, `ocio_lmt` →
 * `cube_path`) and keeps only keys the live Model already owns. Does not convert
 * old project, document, root, checkpoint, or WAL format versions.
 *
 * @pre Caller holds the shared executor render lock when @p document is live.
 */
auto ApplyLeftoverOrdinaryPayloadToDocument(PipelineDocument& document,
                                            const OrdinaryEditPayload& payload,
                                            std::string* error) -> bool;

/**
 * @brief Clone @p root_document and apply first-parent commits in order.
 *
 * Typed batches use @ref ApplyPipelineEditBatch. Leftover ordinary and merge
 * payloads (Paste/merge until that path is replaced) apply onto the document
 * Models. A failed change leaves the returned document unset; the clone is
 * discarded. Does not take the render lock.
 *
 * @param root_document Immutable image root DAG.
 * @param first_parent_commits Root-to-head first-parent commits, oldest first.
 * @param error Optional failure detail.
 * @param context Optional Mask store and apply observer.
 * @return The replayed document, or nullopt when a commit cannot be applied.
 */
[[nodiscard]] auto ReplayPipelineDocumentFromRoot(
    const PipelineDocument& root_document, const std::vector<EditCommit>& first_parent_commits,
    std::string* error, const PipelineHistoryApplyContext& context = {})
    -> std::optional<PipelineDocument>;

}  // namespace alcedo
