//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "edit/history/commit_types.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_asset.hpp"

namespace alcedo {

class CommitGraph;
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
 *         matches the pre-call hash: failed changes are inverse-applied when
 *         possible, and a pre-call clone is restored when graph validation fails
 *         after a structural batch.
 * @pre Caller holds the shared executor render lock.
 */
auto ApplyPipelineEditBatch(PipelineDocument& document, const PipelineEditBatch& batch,
                            PipelineEditApplyDirection direction, std::string* error,
                            const PipelineHistoryApplyContext& context = {}) -> bool;

/**
 * @brief Clone @p root_document and apply first-parent typed batch commits in order.
 *
 * Typed batches use @ref ApplyPipelineEditBatch. A failed change leaves the returned document unset;
 * the clone is discarded. Does not take the render lock.
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

/**
 * @brief First-parent commits from the image root to @p head, oldest first.
 *
 * @throws std::runtime_error when a reachable commit is missing from @p graph.
 */
[[nodiscard]] auto FirstParentCommitsForHead(const CommitGraph& graph, head_commit_hash_t head)
    -> std::vector<EditCommit>;

/**
 * @brief Persistent Brush keys referenced by Color Grade Masks on @p document.
 *
 * Radial and Linear Gradient sources are omitted. Empty optional keys are omitted.
 */
[[nodiscard]] auto CollectPersistentMaskAssetKeys(const PipelineDocument& document)
    -> std::vector<MaskAssetKey>;

/**
 * @brief Load every persistent Mask key referenced by @p document.
 *
 * Empty key sets succeed even when @p mask_store is null. A non-empty set with a
 * null store is an error. A missing or corrupt file is an error. Does not delete
 * files or consult GPU caches.
 *
 * @param document Replayed or live DAG whose Brush keys must resolve.
 * @param mask_store Persistent Mask store, or null when the document has no keys.
 * @param error Optional failure detail.
 * @return false when a referenced asset cannot be loaded.
 */
auto VerifyPersistentMaskAssets(const PipelineDocument& document, MaskStore* mask_store,
                                std::string* error) -> bool;

}  // namespace alcedo
