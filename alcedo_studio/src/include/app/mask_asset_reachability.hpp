//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

#include "edit/history/edit_commit.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/mask/mask_store.hpp"

namespace alcedo {

/**
 * @brief GPU-free Mask-asset owners for one reachability scan.
 *
 * Callers collect every durable and recovery owner: immutable roots, live and
 * checkpoint documents, Version first-parent commits, redo-suffix commits, and
 * unmaterialized WAL records. @p extra_keys holds additional reachable keys
 * from an in-progress authoring buffer; pass empty when there is no authoring
 * buffer.
 */
struct MaskAssetReachabilityScan {
  std::vector<const PipelineDocument*>     documents;
  std::vector<const PipelineEditBatch*>    batches;
  std::vector<const EditCommit*>           commits;
  std::vector<const MiniGitJournalRecord*> wal_records;
  std::vector<MaskAssetKey>                extra_keys;
};

/**
 * @brief Paths removed and every deletion or validation failure.
 *
 * Failures include corrupt files that were left in place. Does not include
 * reachable published assets.
 */
struct MaskAssetMaintenanceReport {
  std::vector<std::filesystem::path> removed_paths;
  std::vector<std::string>           failures;
};

/**
 * @brief Brush keys stored on @p batch (Add/Remove Grade, Add/Remove Mask,
 *        ReplaceMaskSource, ReplaceMaskAsset).
 */
[[nodiscard]] auto CollectMaskAssetKeysFromBatch(const PipelineEditBatch& batch)
    -> std::set<MaskAssetKey>;

/**
 * @brief Brush keys stored on a typed-batch commit payload.
 */
[[nodiscard]] auto CollectMaskAssetKeysFromCommit(const EditCommit& commit)
    -> std::set<MaskAssetKey>;

/**
 * @brief Union of every Brush key referenced by @p scan.
 *
 * Does not inspect GPU or host caches. Does not delete files.
 */
[[nodiscard]] auto CollectReachableMaskAssetKeys(const MaskAssetReachabilityScan& scan)
    -> std::set<MaskAssetKey>;

/**
 * @brief Delete store-root files whose validated keys are absent from @p reachable.
 *
 * Lists only the store root (not recursively). Skips temporary publish files.
 * Derives the key from the file name, validates the file with @ref MaskStore::Load,
 * and removes only that exact path. Corrupt candidates are reported and left in
 * place. Does not remove the store root directory.
 *
 * @pre No editor or recovery operation can add a new reference during this call.
 * @param store Persistent Mask store whose @ref MaskStore::Root is scanned.
 * @param reachable Final reachable key set from @ref CollectReachableMaskAssetKeys.
 */
[[nodiscard]] auto DeleteUnreachableMaskAssetFiles(MaskStore&                    store,
                                                   const std::set<MaskAssetKey>& reachable)
    -> MaskAssetMaintenanceReport;

}  // namespace alcedo
