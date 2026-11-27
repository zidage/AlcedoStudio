//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

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

  /// The live folder @p folder_id with its children loaded; throws std::runtime_error otherwise.
  auto LiveFolder(sl_element_id_t folder_id) -> std::shared_ptr<SleeveFolder>;
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

  auto Create(std::filesystem::path dest, std::wstring filename, ElementType type)
      -> std::shared_ptr<SleeveElement>;
  auto CreateFileInLibrary(file_name_t name) -> std::shared_ptr<SleeveFile>;
  void LinkFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id);
  void UnlinkFileFromFolder(sl_element_id_t file_id, sl_element_id_t folder_id);
  auto UnlinkFilesFromFolder(std::span<const sl_element_id_t> file_ids,
                             sl_element_id_t folder_id) -> std::vector<sl_element_id_t>;
  auto DuplicateFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id)
      -> std::shared_ptr<SleeveFile>;
  void DeleteFileEverywhere(sl_element_id_t file_id);
  auto DeleteFilesEverywhere(std::span<const sl_element_id_t> file_ids)
      -> std::vector<sl_element_id_t>;
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
   * @brief Gives the file @p file_id the first name of @p base_name, `base_name@`,
   *        `base_name@@`, ... that no other element holds in the library root, in the folder
   *        @p folder_id, or in a loaded folder that holds the file. Each loaded folder that holds
   *        the file keeps it under the new name. The file keeps its id and its links.
   * @return The name of the file after the call; no change when that is its current name.
   * @throws std::runtime_error when @p file_id is not a live file, @p folder_id is not a live
   *         folder, or @p base_name is empty.
   */
  auto RenameFile(sl_element_id_t file_id, const file_name_t& base_name, sl_element_id_t folder_id)
      -> file_name_t;
  /**
   * @brief Removes the folder @p folder_id from its parent @p parent_id, as Delete does. Child
   *        folders that no other parent holds are deleted with it; files stay in the library.
   * @throws std::runtime_error when @p parent_id is not a live folder or does not hold
   *         @p folder_id, or @p folder_id is the library root.
   */
  void RemoveFolderFrom(sl_element_id_t parent_id, sl_element_id_t folder_id);
  auto Get(std::filesystem::path target, bool write) -> std::shared_ptr<SleeveElement>;
  auto Get(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  auto ListFolderContent(const std::filesystem::path& folder_path, bool write = false)
      -> std::vector<sl_element_id_t>;
  auto ListFolderContent(sl_element_id_t folder_id) -> std::vector<sl_element_id_t>;
  /**
   * @brief The live child folders of the folder at @p folder_path, in the order of its entries.
   *        Reads memory only: all folders are resident (Sleeve Node Cache Plan, SN3).
   * @throws std::runtime_error when @p folder_path names no live folder.
   */
  auto ListChildFolders(const std::filesystem::path& folder_path)
      -> std::vector<std::shared_ptr<SleeveFolder>> {
    return storage_handler_.ChildFolders(*resolver_.ResolveFolder(folder_path));
  }
  /// Every live folder below the root at each of its paths, parents before children. Reads
  /// memory only (see NodeStorageHandler::FolderTree).
  [[nodiscard]] auto ListFolderTree() const -> std::vector<FolderTreeEntry> {
    return storage_handler_.FolderTree(*root_);
  }
  /// The in-memory nodes. Reads memory only.
  [[nodiscard]] auto Nodes() const -> const NodeCache& { return nodes_; }

  auto ApplyFilterToFolder(const std::filesystem::path&       folder_path,
                           const std::shared_ptr<FilterCombo> filter)
      -> std::vector<std::shared_ptr<SleeveElement>>;

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

  auto Tree(const std::filesystem::path& path) -> std::wstring;
};
};  // namespace alcedo
