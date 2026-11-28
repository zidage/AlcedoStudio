//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <unordered_set>
#include <vector>

#include "sleeve/node_cache.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "storage/store/sleeve/folder_entry_store.hpp"
#include "type/type.hpp"

namespace alcedo {

/// One folder of the folder tree at one path (Sleeve Node Cache Plan, SN3). The fields are
/// copies of the folder fields that the read makes under the file system lock, so the entry
/// stays valid after the lock ends and holds no node of the cache (#1178).
struct FolderTreeEntry {
  /// The path of the folder from the library root, for example `/Trip/Day 1`.
  std::filesystem::path     path_;
  /// The element id of the folder.
  sl_element_id_t           folder_id_ = 0;
  /// The name of the folder: the last part of path_.
  file_name_t               name_;
  /// The mount of the folder; empty for a library folder.
  std::optional<mount_id_t> mount_id_;
};

/// One link of a file into a folder (Sleeve Node Cache Plan, SN5).
struct FileLink {
  sl_element_id_t file_id                                   = 0;
  sl_element_id_t folder_id                                 = 0;

  auto            operator==(const FileLink&) const -> bool = default;
  auto            operator<=>(const FileLink&) const        = default;
};

/**
 * @brief Loads the elements that the NodeCache of a FileSystem does not hold.
 *
 * @details Each loaded element goes into the cache. An element that the cache holds keeps its
 * live object, so the changes of the current write stay visible. All folders are resident:
 * LoadFolderTree puts each folder into the cache when the project opens, and each folder then
 * holds the names and ids of its child folders. A folder holds no file entry: the file members
 * of a folder are its `FolderContent` rows plus its pending sets (FileIds, HeldFiles), and these
 * reads load no file element. Not thread-safe: the FileSystem owner serializes the calls.
 */
class NodeStorageHandler {
 public:
  NodeStorageHandler(ElementStore& element_store, FolderEntryStore& folder_entry_store,
                     NodeCache& nodes);

  /// Puts @p new_element into the cache (NodeCache::Insert).
  void AddToStorage(std::shared_ptr<SleeveElement> new_element);
  /**
   * @brief Loads every stored folder into the cache and gives each folder its child folders,
   *        with one FolderEntryStore::ReadFolderTree call.
   *
   * @details Call once, after the root is in the cache and before any other operation. A folder
   * with more than one parent gets one child entry in each parent; the load does not change a
   * reference count and marks no folder modified. The walk from the root, and then from each
   * folder that the root does not reach, uses an explicit stack. A link to a folder on the
   * current walk path closes a cycle: the load skips the link and writes one warning with both
   * ids. A link whose child name the parent already holds for another folder is skipped with one
   * warning. No file element loads.
   * @throws The store error of FolderEntryStore::ReadFolderTree, after one error log line with
   *         its text; the cache then holds the root only.
   */
  void LoadFolderTree();
  /**
   * @brief The ids of the live files that @p folder holds, in id order: its stored file rows
   *        with the pending sets applied (section 3.3 of the plan). Loads no element.
   *
   * @details One FolderEntryStore::ReadFolderFiles statement; none for a folder with no stored
   * row (UNSYNC). A file that the cache holds as DELETED is not in the result.
   * @throws The store error of the read. Nothing changes.
   */
  auto FileIds(const SleeveFolder& folder) const -> std::vector<sl_element_id_t>;
  /**
   * @brief The ids of @p file_ids that @p folder holds as live files. Loads no element.
   *
   * @details A pending link or unlink decides an id from memory. The other ids go to one
   * FolderEntryStore::ReadHeldFiles statement; no statement when memory decides each id or the
   * folder has no stored row (UNSYNC).
   * @throws The store error of the read. Nothing changes.
   */
  auto HeldFiles(const SleeveFolder& folder, std::span<const sl_element_id_t> file_ids) const
      -> std::unordered_set<sl_element_id_t>;
  /**
   * @brief The ids of @p ids that name live files, in their first order, without repeats.
   *        Loads no element (Sleeve Node Cache Plan, SN5).
   *
   * @details The cache decides an id that it holds. The other ids go to one HeldFiles read of
   * @p root, the library root, which holds a link of each live file (D2).
   * @throws The store error of the read. Nothing changes.
   */
  auto LiveFileIds(const SleeveFolder& root, std::span<const sl_element_id_t> ids) const
      -> std::vector<sl_element_id_t>;
  /**
   * @brief The links of the live files with the mount id @p mount_id into @p folders, ordered by
   *        file id and then by folder id (Sleeve Node Cache Plan, SN5).
   *
   * @details One FolderEntryStore::ReadMountFileFolders statement gives the stored links; none
   * when each folder of @p folders is UNSYNC. A stored link counts when its folder is in
   * @p folders and has no pending change for the file. The file of a link is decided from the
   * cache when the cache holds it (its mount id, a DELETED flag), and from its stored mount id
   * otherwise, so no stored link loads an element. Each pending file link of a folder counts
   * when its file has the mount id; a file that the cache does not hold is then loaded.
   * @param folders Live folders; the caller gives the folders of one mount.
   * @throws The store error of the read or of a load. Nothing changes.
   */
  auto MountFileLinks(mount_id_t mount_id, std::span<const std::shared_ptr<SleeveFolder>> folders)
      -> std::vector<FileLink>;
  /**
   * @brief The element @p id: the cached element, or the stored element, which then goes into
   *        the cache.
   * @throws The store error of ElementStore::GetElementById when no row has the id.
   */
  auto GetElement(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  /**
   * @brief The live child folders of @p folder, in the order of the folder entries. Reads memory
   *        only: each folder is resident.
   */
  auto ChildFolders(const SleeveFolder& folder) const -> std::vector<std::shared_ptr<SleeveFolder>>;
  /**
   * @brief The live child folders of @p folder as entries at @p folder_path / name, in the
   *        order of the folder entries. Reads memory only.
   */
  auto ChildFolderEntries(const SleeveFolder&          folder,
                          const std::filesystem::path& folder_path) const
      -> std::vector<FolderTreeEntry>;
  /**
   * @brief Every live folder below @p root at each of its paths, parents before children, in
   *        the order of the folder entries. Reads memory only.
   *
   * @details A folder with two parents is in the result once under each parent. The walk uses
   * an explicit stack and skips a child folder that is on the current walk path, so a cycle that
   * a write created ends the walk of that branch. @p root itself is not in the result; the paths
   * start at `/`.
   */
  auto FolderTree(const SleeveFolder& root) const -> std::vector<FolderTreeEntry>;

 private:
  /// True when the cache holds @p id as a DELETED element. Reads memory only.
  auto              IsDeletedInCache(sl_element_id_t id) const -> bool;

  ElementStore&     element_store_;
  FolderEntryStore& folder_entry_store_;
  NodeCache&        nodes_;
};

}  // namespace alcedo
