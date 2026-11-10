//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "storage/store/database.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "storage/store/mask_asset/mask_asset_store.hpp"
#include "storage/store/store_types.hpp"

namespace alcedo {

/**
 * @brief One connection of the project database, held under the database lock, and the stores
 *        that run on it.
 *
 * Storage::OpenSession creates it. The constructor opens a connection and takes the database lock;
 * the destructor releases the lock and closes the connection. Every call of a store from a session
 * runs on that connection, so several calls form one locked unit, for example a read of the stored
 * edit state followed by the write that depends on it.
 *
 * The stores are valid only for the lifetime of the session: keep no reference to them after it
 * ends. The lock is recursive, so a thread that holds a session can still call the stores of
 * Storage (ElementStore and the others), which take the same lock. Another thread waits until the
 * session ends.
 *
 * A session cannot be copied or moved: its stores refer to its connection.
 */
class StorageSession {
 public:
  /// Opens a connection of @p database and takes its lock. Throws std::runtime_error when the
  /// connection cannot be opened.
  explicit StorageSession(Database& database);
  StorageSession(const StorageSession&)                    = delete;
  StorageSession(StorageSession&&)                         = delete;
  auto operator=(const StorageSession&) -> StorageSession& = delete;
  auto operator=(StorageSession&&) -> StorageSession&      = delete;
  ~StorageSession()                                        = default;

  /// Mini-Git commit graph rows (EditCommit, VersionRef, ImageEditState, PipelineRoot).
  auto CommitGraph() -> CommitGraphStore&;

  /// AI Segmentation Mask R8 assets (MaskAsset).
  auto MaskAssets() -> MaskAssetStore&;

  /**
   * @brief Write the history root of each image in @p roots and its element pipeline JSON
   *        (PipelineParam) in one transaction.
   *
   * @param roots Roots from CommitGraphStore::EncodeRootPipeline.
   * @param element_pipeline_json Pipeline document JSON of each image, in the order of @p roots.
   * @throws std::invalid_argument when the two sizes differ; std::runtime_error when an image
   *         already has a root or a write fails. Nothing is written then.
   */
  void InsertImageRoots(std::span<const EncodedRootPipeline> roots,
                        std::vector<std::string>             element_pipeline_json);

 private:
  ConnectionGuard                        guard_;
  std::unique_lock<std::recursive_mutex> lock_;
  CommitGraphStore                       commit_graph_;
  MaskAssetStore                         mask_assets_;
};

}  // namespace alcedo
