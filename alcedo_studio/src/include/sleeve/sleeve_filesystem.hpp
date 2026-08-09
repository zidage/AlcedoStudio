//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "path_resolver.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"
#include "utils/id/id_generator.hpp"

namespace alcedo {
class FileSystem {
  using NodeMapping = std::unordered_map<sl_element_id_t, std::shared_ptr<SleeveElement>>;

 private:
  // A mapping between node id and pointers to actual nodes.
  NodeMapping                          storage_;
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

 public:
  // FileSystem(std::filesystem::path db_path, sl_element_id_t start_id);
  FileSystem(std::filesystem::path db_path, Storage& storage_service,
             sl_element_id_t start_id);

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
  auto Get(std::filesystem::path target, bool write) -> std::shared_ptr<SleeveElement>;
  auto Get(sl_element_id_t id) -> std::shared_ptr<SleeveElement>;
  auto ListFolderContent(const std::filesystem::path& folder_path, bool write = false)
      -> std::vector<sl_element_id_t>;
  auto ListFolderContent(sl_element_id_t folder_id) -> std::vector<sl_element_id_t>;

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

  auto GetModifiedElements() -> std::vector<std::shared_ptr<SleeveElement>>;
  auto GetUnsyncedElements() -> std::vector<std::shared_ptr<SleeveElement>>;
  auto GetDeletedElements() -> std::vector<std::shared_ptr<SleeveElement>>;

  void GarbageCollect() { storage_handler_.GarbageCollect(); }

  auto Tree(const std::filesystem::path& path) -> std::wstring;
};
};  // namespace alcedo
