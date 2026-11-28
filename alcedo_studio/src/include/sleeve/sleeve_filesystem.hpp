//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "path_resolver.hpp"
#include "sleeve/node_cache.hpp"
#include "sleeve/node_storage_handler.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "sleeve/storage.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "type/type.hpp"
#include "utils/id/id_generator.hpp"

namespace alcedo {
class FileSystem {
 private:
  /// The in-memory nodes and the dirty list (Sleeve Node Cache Plan, SN2).
  NodeCache                            nodes_;
  std::shared_ptr<SleeveFolder>        root_;

  // ID Generation
  IncrID::IDGenerator<sl_element_id_t> id_gen_;
  /** @name Database interaction */
  ///@{
  std::filesystem::path                db_path_;
  std::filesystem::path                meta_path_;
  Storage&                             storage_service_;
  NodeStorageHandler                   storage_handler_;
  PathResolver                         resolver_;
  ///@}

  /// The live folder @p folder_id; throws std::runtime_error otherwise.
  auto LiveFolder(sl_element_id_t folder_id) -> std::shared_ptr<SleeveFolder>;
  /// The live file @p file_id (loaded when the cache does not hold it); throws
  /// std::runtime_error otherwise.
  auto LiveFile(sl_element_id_t file_id) -> std::shared_ptr<SleeveElement>;
  /// Decrement the child folders of a folder that lost its last parent, recursively.
  void ReleaseChildFolders(const std::shared_ptr<SleeveFolder>& folder);

 public:
  // FileSystem(std::filesystem::path db_path, sl_element_id_t start_id);
  FileSystem(std::filesystem::path db_path, Storage& storage_service,
             sl_element_id_t start_id);

  /**
   * @brief Puts the root into the node cache. For a stored root, loads every folder with one
   *        NodeStorageHandler::LoadFolderTree call; a new root has no stored folder.
   * @throws std::runtime_error when the root is already set, and the store error of the folder
   *         tree read (NodeStorageHandler::LoadFolderTree logs it).
   */
  auto InitRoot() -> bool;

  /**
   * @brief Creates an element named @p filename in the folder at @p dest. A folder gets the
   *        first name of `filename`, `filename@`, ... that no child folder of @p dest holds. A
   *        file keeps @p filename (a label) and is linked to @p dest and to the library root.
   * @throws std::runtime_error when @p dest names no folder.
   */
  auto Create(std::filesystem::path dest, std::wstring filename, ElementType type)
      -> std::shared_ptr<SleeveElement>;
  /// Creates a file named @p name in the library root. Any name is accepted (a label).
  auto CreateFileInLibrary(file_name_t name) -> std::shared_ptr<SleeveFile>;
  /**
   * @brief Links the file @p file_id to the folder @p folder_id. Records the link only: the
   *        sync keeps an existing row, so a repeated link needs no read and keeps one row. Loads
   *        no other file element; the name of the file has no rule.
   * @throws std::runtime_error when @p file_id is not a live file or @p folder_id is not a live
   *         folder.
   */
  void LinkFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id);
  /**
   * @brief Unlinks the file @p file_id from the folder @p folder_id. The pending sets decide the
   *        membership from memory; otherwise one FolderEntryStore::ReadHeldFiles read does.
   * @throws std::runtime_error when @p folder_id is the root, when an id names no live element of
   *         its kind, or when the file is not a member, and the store error of the read. A throw
   *         changes nothing.
   */
  void UnlinkFileFromFolder(sl_element_id_t file_id, sl_element_id_t folder_id);
  /**
   * @brief Unlinks each live file of @p file_ids that the folder holds, with at most one store
   *        read for the whole batch. Ids that name no live file or no member are ignored. Loads
   *        no element (NodeStorageHandler::HeldFiles gives live files only).
   * @return The unlinked ids, in the order of @p file_ids.
   * @throws std::runtime_error when @p folder_id is the root or not a live folder, and the store
   *         error of the read. A throw changes nothing.
   */
  auto UnlinkFilesFromFolder(std::span<const sl_element_id_t> file_ids,
                             sl_element_id_t folder_id) -> std::vector<sl_element_id_t>;
  /**
   * @brief Links each live file of @p file_ids to the folder @p folder_id, as LinkFileToFolder
   *        does, and loads no file element (Sleeve Node Cache Plan, SN5).
   *
   * @details NodeStorageHandler::LiveFileIds decides the ids: the cache decides an id that it
   * holds, and one membership read of the library root decides the others. Ids that name no
   * live file are ignored.
   * @return The linked ids, in the order of @p file_ids, without repeats.
   * @throws std::runtime_error when @p folder_id is not a live folder, and the store error of
   *         the read. A throw changes nothing.
   */
  auto LinkFilesToFolder(std::span<const sl_element_id_t> file_ids, sl_element_id_t folder_id)
      -> std::vector<sl_element_id_t>;
  /// A new file with the name and the image of the file @p file_id, linked to @p folder_id and
  /// to the library root.
  auto DuplicateFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id)
      -> std::shared_ptr<SleeveFile>;
  /// Marks the file DELETED and forgets its pending links. The sync removes the element and all
  /// its rows.
  void DeleteFileEverywhere(sl_element_id_t file_id);
  auto DeleteFilesEverywhere(std::span<const sl_element_id_t> file_ids)
      -> std::vector<sl_element_id_t>;
  /// Removes the folder at @p target from its parent. Child folders that no other parent holds
  /// go with it; the files stay in the library.
  void Delete(std::filesystem::path target);
  void Delete(sl_element_id_t target_id);
  /**
   * @brief Renames the folder at @p target. The folder keeps its id, its child folders, and
   *        its album membership.
   * @throws std::runtime_error when @p target is the root or not a live folder, when
   *         @p new_name is empty, or when the parent holds another element named @p new_name.
   */
  void RenameFolder(const std::filesystem::path& target, const file_name_t& new_name);
  /**
   * @brief Moves the folder at @p target into the folder at @p new_parent. The folder keeps
   *        its id, its name, its child folders, and its album membership.
   * @throws std::runtime_error when @p target is the root or not a live folder, when
   *         @p new_parent is not a folder, is @p target, or lies inside @p target, or when
   *         @p new_parent holds another element with the name of the folder.
   */
  void MoveFolder(const std::filesystem::path& target, const std::filesystem::path& new_parent);
  /**
   * @brief Creates a folder named @p name in the folder @p parent_id and returns it.
   * @throws std::runtime_error when @p parent_id is not a live folder, when @p name is empty, or
   *         when the parent holds an element named @p name.
   */
  auto CreateFolderIn(sl_element_id_t parent_id, const file_name_t& name)
      -> std::shared_ptr<SleeveFolder>;
  /**
   * @brief Gives the file @p file_id the name @p name. A file name is a label: other files can
   *        hold it. The file keeps its id and its links. No change when the file holds the name.
   * @throws std::runtime_error when @p file_id is not a live file or @p name is empty.
   */
  void RenameFile(sl_element_id_t file_id, const file_name_t& name);
  /**
   * @brief Removes the folder @p folder_id from its parent @p parent_id, as Delete does. Child
   *        folders that no other parent holds are deleted with it; files stay in the library.
   * @throws std::runtime_error when @p parent_id is not a live folder or does not hold
   *         @p folder_id, or @p folder_id is the library root.
   */
  void RemoveFolderFrom(sl_element_id_t parent_id, sl_element_id_t folder_id);
  auto Get(std::filesystem::path target, bool write) -> std::shared_ptr<SleeveElement>;
  auto Get(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  /**
   * @brief The members of the folder @p folder_id: its child folder ids in the order of their
   *        entries, then its file ids (ListFileIds). Loads no file element.
   * @throws std::runtime_error when @p folder_id is not a live folder, and the store error.
   */
  auto ListFolderContent(sl_element_id_t folder_id) -> std::vector<sl_element_id_t>;
  /**
   * @brief The ids of the live files that the folder @p folder_id holds, in id order: one store
   *        read with the pending changes of the current write applied. Loads no file element.
   * @throws std::runtime_error when @p folder_id is not a live folder, and the store error.
   */
  auto ListFileIds(sl_element_id_t folder_id) -> std::vector<sl_element_id_t>;
  /**
   * @brief The links of the live files with the mount id @p mount_id into the folders
   *        @p folder_ids, ordered by file id and then by folder id: one store read with the
   *        pending changes of the current write applied (NodeStorageHandler::MountFileLinks).
   *        Loads no file element of a stored link.
   * @throws std::runtime_error when an id of @p folder_ids is not a live folder, and the store
   *         error. Nothing changes.
   */
  auto ListMountFileLinks(mount_id_t mount_id, std::span<const sl_element_id_t> folder_ids)
      -> std::vector<FileLink>;
  /**
   * @brief The live child folders of the folder at @p folder_path as entries at
   *        @p folder_path / name, in the order of its entries. Reads memory only: all folders
   *        are resident (Sleeve Node Cache Plan, SN3).
   * @throws std::runtime_error when @p folder_path names no live folder.
   */
  auto ListChildFolders(const std::filesystem::path& folder_path) -> std::vector<FolderTreeEntry> {
    return storage_handler_.ChildFolderEntries(*resolver_.ResolveFolder(folder_path), folder_path);
  }
  /// Every live folder below the root at each of its paths, parents before children. Reads
  /// memory only (see NodeStorageHandler::FolderTree).
  [[nodiscard]] auto ListFolderTree() const -> std::vector<FolderTreeEntry> {
    return storage_handler_.FolderTree(*root_);
  }
  /// The in-memory nodes. Reads memory only.
  [[nodiscard]] auto Nodes() const -> const NodeCache& { return nodes_; }
  /**
   * @brief Removes the least recently used clean file elements that no caller holds, down to the
   *        limit (NodeCache::EvictToLimit). Returns the number of removed elements.
   * @pre Call under the lock that serializes this file system, at the end of its scope, while
   *      the result of the operation still holds its elements.
   */
  auto               EvictToLimit() -> std::size_t { return nodes_.EvictToLimit(); }
  /// Sets the limit of clean file elements (NodeCache::SetLimit). Evicts nothing.
  void               SetCleanFileLimit(std::size_t limit) { nodes_.SetLimit(limit); }

  auto ApplyFilterToFolder(const std::filesystem::path&       folder_path,
                           const std::shared_ptr<FilterCombo> filter)
      -> std::vector<std::shared_ptr<SleeveElement>>;

  /// Adds the folder at @p from as a child folder of the folder at @p dest (copy-on-write: the
  /// folder gets a second parent).
  void Copy(std::filesystem::path from, std::filesystem::path dest);

  [[deprecated("Deprecated, use the one from SleeveService instead")]] void SyncToDB();
  [[deprecated("Deprecated, use the one from SleeveService instead")]] void WriteSleeveMeta(
      const std::filesystem::path& meta_path);
  [[deprecated("Deprecated, use the one from SleeveService instead")]] void ReadSleeveMeta(
      const std::filesystem::path& meta_path);

  auto GetCurrentID() -> sl_element_id_t { return id_gen_.GetCurrentID(); }

  /// The elements with a change that no sync wrote, in the order they changed (the dirty list
  /// of the node cache). Reads memory only.
  [[nodiscard]] auto DirtyElements() const -> std::vector<std::shared_ptr<SleeveElement>> {
    return nodes_.DirtyElements();
  }
  /**
   * @brief Records that a committed transaction wrote @p written. A DELETED element leaves the
   *        node cache; each other element becomes SYNCED and a folder clears its pending content
   *        changes.
   * @pre Each element of @p written is in the node cache. Call only after the commit.
   */
  void MarkWritten(std::span<const std::shared_ptr<SleeveElement>> written);

  /// Restores committed fields and child folders of the affected cache entries in place after a
  /// failed sync. Discards newly created elements and preserves allocated IDs. Throws if the
  /// restore fails. The caller holds the SleeveService lock.
  void RestoreAfterFailedSync(std::span<const std::shared_ptr<SleeveElement>> affected);

  auto Tree(const std::filesystem::path& path) -> std::wstring;
};
};  // namespace alcedo
