//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"
#include "utils/cache/lru_cache.hpp"

namespace alcedo {

/**
 * @brief Build the committed pipeline graph snapshot of one image from storage.
 *
 * Reads the materialized history state: the immutable root, and either the checkpoint whose
 * labels equal the materialized head and chain or a replay of the first-parent chain. Binds the
 * image camera profile. Never reads the element pipeline JSON.
 *
 * @throws std::runtime_error when the image has no history root (every imported image has one, so
 *         this is a broken invariant, not a supported state), when the root or history cannot be
 *         decoded, or when replay fails.
 */
[[nodiscard]] auto LoadCommittedSnapshotFromStorage(Storage& storage, sl_element_id_t element_id)
    -> std::shared_ptr<const PipelineGraphSnapshot>;

/**
 * @brief CPU value cache of committed pipeline graph snapshots, one entry per image.
 *
 * Purpose: thumbnails, analysis, and later export render only committed history states. This
 * cache is where they get them without touching the editor's live document or any executor.
 *
 * Two sources fill an entry:
 * - the editor, which publishes the snapshot of each committed state of the image it holds
 *   (@ref Publish). While the editor holds the image, that entry is returned without reading
 *   storage, because the editor is the only writer of the image's history and may have commits
 *   that are journaled but not yet materialized;
 * - storage (@ref LoadCommittedSnapshotFromStorage) for every other image. A stored entry is
 *   reused while its head and chain equal the materialized labels in storage, so writers that
 *   persist history directly (Paste to library images) need no invalidation call.
 *
 * Captured state: immutable snapshots only; eviction does no I/O and holds no GPU resource.
 * Thread: every method is safe to call from any thread. Storage reads run without the cache lock.
 */
class CommittedSnapshotCache {
 public:
  static constexpr std::size_t kDefaultCapacity = 16;

  explicit CommittedSnapshotCache(std::shared_ptr<Storage> storage,
                                  std::size_t              capacity = kDefaultCapacity);

  /**
   * @brief Committed snapshot of @p element_id.
   * @param editor_holds_image True while the editor session owns the image's history.
   * @throws std::runtime_error from @ref LoadCommittedSnapshotFromStorage.
   */
  [[nodiscard]] auto Acquire(sl_element_id_t element_id, bool editor_holds_image)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /**
   * @brief Snapshot of the materialized history state of @p element_id.
   *
   * Unlike @ref Acquire, an editor publication is returned only when its head and chain equal the
   * materialized labels in storage: commits that exist only in the editor's journal are excluded.
   * @throws std::runtime_error from @ref LoadCommittedSnapshotFromStorage.
   */
  [[nodiscard]] auto AcquireMaterialized(sl_element_id_t element_id)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /**
   * @brief Store the editor's snapshot of a committed state of its image.
   * @param editor_holds_image When false (the editor already released the image) the entry is
   *        stored like a storage-built one and checked against storage on the next acquire.
   * @throws std::invalid_argument when @p snapshot is null or not committed.
   */
  void Publish(std::shared_ptr<const PipelineGraphSnapshot> snapshot, bool editor_holds_image);

  /**
   * @brief The editor released @p element_id: check its entry against storage from now on.
   * @return The last snapshot the editor published for the image, or null when it published none.
   */
  auto EndEditorPublication(sl_element_id_t element_id)
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /// Last snapshot the editor published for the image it holds, or null when it published none.
  [[nodiscard]] auto EditorPublished(sl_element_id_t element_id) const
      -> std::shared_ptr<const PipelineGraphSnapshot>;

  /// Drop the entry of a deleted image.
  void Forget(sl_element_id_t element_id);

  /// Test/instrumentation: storage builds since construction.
  [[nodiscard]] auto StorageLoadCount() const -> std::size_t;

 private:
  /// Return the first candidate whose labels equal the materialized labels in storage, else build
  /// the snapshot from storage and store it. Called without the cache lock.
  auto MatchStoredOrLoad(sl_element_id_t                              element_id,
                         std::shared_ptr<const PipelineGraphSnapshot> published,
                         std::shared_ptr<const PipelineGraphSnapshot> stored)
      -> std::shared_ptr<const PipelineGraphSnapshot>;
  void                     StoreLocked(std::shared_ptr<const PipelineGraphSnapshot> snapshot);

  std::shared_ptr<Storage> storage_;
  mutable std::mutex       mutex_;
  /// Snapshots published by the editor for the image it holds. Outside the LRU: evicting one
  /// would fall back to storage, which lags the editor's journaled commits, and the editor
  /// publishes each committed state only once.
  std::unordered_map<sl_element_id_t, std::shared_ptr<const PipelineGraphSnapshot>> published_;
  /// Storage-built snapshots and snapshots of released images, checked against storage on use.
  LRUCache<sl_element_id_t, sl_element_id_t>                                        stored_order_;
  std::unordered_map<sl_element_id_t, std::shared_ptr<const PipelineGraphSnapshot>> stored_;
  std::size_t storage_load_count_ = 0;
};

}  // namespace alcedo
