//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <opencv2/core/hal/interface.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <type_traits>
#include <vector>

#include "app/mount_tree.hpp"
#include "app/sleeve_error.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "sleeve/storage.hpp"
#include "type/type.hpp"

namespace alcedo {
// An interaction level below SleeveManager (Future Sleeve API for UI) and above ImagePoolManager
// and FileSystem

struct SyncResult {
  bool        success_         = true;
  uint32_t    elements_synced_ = 0;
  std::string message_         = "";
};

class SleeveService {
 public:
  virtual ~SleeveService() = default;

  template <typename TResult>
  auto Read(std::function<TResult(FileSystem&)> operation) -> TResult;

  template <typename TResult>
  auto Write(std::function<TResult(FileSystem&)> operation)
      -> std::conditional_t<std::is_void_v<TResult>, SyncResult,
                            std::pair<TResult, SyncResult>>;

  virtual auto Sync() -> SyncResult = 0;
};

class SleeveServiceImpl final : public SleeveService {
 private:
  std::shared_ptr<Storage> storage_;
  std::unique_ptr<FileSystem>     fs_;
  std::filesystem::path           db_path_;

  mutable std::mutex fs_lock_;  // Use a fat lock for now, but I have no plan to design a
                                // fine-grained concurrency model for FS

 public:
  SleeveServiceImpl() = delete;
  SleeveServiceImpl(std::shared_ptr<Storage> storage_service,
                    const std::filesystem::path&   db_path,
                    sl_element_id_t                start_id = 0);

  template <typename TResult>
  auto Read(std::function<TResult(FileSystem&)> operation) -> TResult {
    std::lock_guard<std::mutex> lock(fs_lock_);
    if constexpr (std::is_void_v<TResult>) {
      operation(*fs_);
      return;
    } else {
      return operation(*fs_);
    }
  }

  template <typename TResult>
  auto Write(std::function<TResult(FileSystem&)> operation)
      -> std::conditional_t<std::is_void_v<TResult>, SyncResult,
                            std::pair<TResult, SyncResult>> {
    std::lock_guard<std::mutex> lock(fs_lock_);

    if constexpr (std::is_void_v<TResult>) {
      operation(*fs_);
      return Sync();  // void: only return SyncResult
    } else {
      TResult    result      = operation(*fs_);
      SyncResult sync_result = Sync();
      return {result, sync_result};
    }
  }

  template <typename TResult>
  auto Write_NoSync(std::function<TResult(FileSystem&)> operation) -> TResult {
    std::lock_guard<std::mutex> lock(fs_lock_);

    if constexpr (std::is_void_v<TResult>) {
      operation(*fs_);
      return;
    } else {
      TResult result = operation(*fs_);
      return result;
    }
  }


  auto Sync() -> SyncResult override;

  /**
   * @brief The one structure check: true when the user can change the structure of @p folder.
   *
   * It reads `mount_id_` of @p folder only (D6). A mounted folder has a value. The user
   * operations below throw SleeveOperationError with kMountedFolderStructureIsReadOnly for a
   * folder where this is false; the scan entry (the mounted folder group) does not use it.
   */
  [[nodiscard]] static auto CanEditFolderStructure(const SleeveElement& folder) -> bool;
  /// Throws SleeveOperationError when the folder at @p folder_path is mounted. A path that names
  /// no folder passes: the operation that follows reports it.
  void                      CheckFolderStructureEditable(const std::filesystem::path& folder_path);

  auto GetCurrentID() const -> sl_element_id_t;
  auto ResolveElement(const std::filesystem::path& path) -> std::shared_ptr<SleeveElement>;
  auto ResolveFolder(const std::filesystem::path& path) -> std::shared_ptr<SleeveFolder>;
  auto ResolveFile(const std::filesystem::path& path) -> std::shared_ptr<SleeveFile>;
  auto ListFolderEntries(const std::filesystem::path& folder_path)
      -> std::vector<std::shared_ptr<SleeveElement>>;
  /** @name User operations
   * Each checks its target before any change and throws SleeveOperationError (Mounted Folder
   * Plan, MF5): a folder that gains, loses, or changes an element must pass
   * CanEditFolderStructure; a file that leaves the library must be an ordinary file.
   * MoveFolder checks the folder and the new parent. A mounted file links into an ordinary
   * folder, and leaves an ordinary folder, like an ordinary file (D8).
   */
  ///@{
  auto CreateFolder(const std::filesystem::path& parent_path, const file_name_t& name)
      -> std::pair<std::shared_ptr<SleeveFolder>, SyncResult>;
  auto CreateFileInLibrary(const file_name_t& name)
      -> std::pair<std::shared_ptr<SleeveFile>, SyncResult>;
  auto LinkFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id) -> SyncResult;
  auto DeleteFileFromFolder(sl_element_id_t file_id, sl_element_id_t folder_id) -> SyncResult;
  auto DeleteFilesFromFolder(std::span<const sl_element_id_t> file_ids,
                             sl_element_id_t                  folder_id)
      -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult>;
  auto DeleteFileEverywhere(sl_element_id_t file_id) -> SyncResult;
  auto DeleteFilesEverywhere(std::span<const sl_element_id_t> file_ids)
      -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult>;
  auto DuplicateFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id)
      -> std::pair<std::shared_ptr<SleeveFile>, SyncResult>;
  auto DeletePath(const std::filesystem::path& target_path) -> SyncResult;
  auto DeleteElement(sl_element_id_t target_id) -> SyncResult;
  /// See FileSystem::RenameFolder. Throws like it; syncs the change before it returns.
  auto RenameFolder(const std::filesystem::path& folder_path, const file_name_t& new_name)
      -> SyncResult;
  /// See FileSystem::MoveFolder. Throws like it; syncs the change before it returns.
  auto MoveFolder(const std::filesystem::path& folder_path,
                  const std::filesystem::path& new_parent_path) -> SyncResult;

  /** @name Mounted folders (Mounted Folder Plan, MF4)
   * The scan entry. These operations do not run the structure check of MF5: the scan is the
   * only writer of the structure of a mount. Each runs in one `fs_lock_` scope; the writes then
   * sync the tree. A failed sync leaves the changes in memory with their sync flags, and the next
   * sync writes them.
   */
  ///@{
  /// See ReadMountTree.
  auto ReadMountTree(sl_element_id_t root_folder_id, mount_id_t mount_id) -> MountTree;
  /// See alcedo::ApplyMountTreeChanges. Throws like it, before the sync.
  auto ApplyMountTreeChanges(const MountTreeChanges& changes) -> SyncResult;
  /// See alcedo::CreateMountRootFolder. Throws like it, before the sync.
  auto CreateMountRootFolder(sl_element_id_t parent_id, const file_name_t& name,
                             mount_id_t mount_id)
      -> std::pair<std::shared_ptr<SleeveFolder>, SyncResult>;
  /// See alcedo::RemoveMountTree. Throws like it, before the sync.
  auto RemoveMountTree(sl_element_id_t root_folder_id, mount_id_t mount_id,
                       std::span<const sl_element_id_t> file_ids) -> SyncResult;
  /**
   * @brief Delete the missing photos @p file_ids of the mount @p mount_id from every folder
   *        (Remove Missing Photos, MF8). FileSystem::DeleteFilesEverywhere without the rule of
   *        MF5 that keeps a mounted photo in the library.
   * @return The deleted files.
   * @throws std::invalid_argument, before any change, when an id is not a live file of the
   *         mount whose source record is missing.
   */
  auto DeleteMissingMountedFiles(mount_id_t mount_id, std::span<const sl_element_id_t> file_ids)
      -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult>;
  ///@}

  auto GetStorage() -> std::shared_ptr<Storage> {
    return storage_;
  }
};
};  // namespace alcedo
