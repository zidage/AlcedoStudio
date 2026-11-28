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
#include <stdexcept>
#include <string>
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

/// What SleeveServiceImpl::DeleteFilesEverywhereKeepingMounted deleted and kept.
struct LibraryRemoval {
  std::vector<std::shared_ptr<SleeveFile>> deleted_files_{};
  /// The live mounted files of the request, in its order. They stay in the library.
  std::vector<sl_element_id_t>             kept_mounted_ids_{};
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
  std::string sync_restore_error_;

  /// The sync itself. The caller holds `fs_lock_`: it reads the dirty list of the node cache and
  /// moves the written elements to the clean list. On SQL failure it restores committed state.
  auto SyncLocked() -> SyncResult;

 public:
  SleeveServiceImpl() = delete;
  SleeveServiceImpl(std::shared_ptr<Storage> storage_service,
                    const std::filesystem::path&   db_path,
                    sl_element_id_t                start_id = 0);

  // Each fs_lock_ scope ends with FileSystem::EvictToLimit (Sleeve Node Cache Plan, SN7), after
  // the result is complete: an element that the result holds stays in the cache, so the caller
  // keeps its identity. An operation that throws skips the eviction; the next scope applies it.

  template <typename TResult>
  auto Read(std::function<TResult(FileSystem&)> operation) -> TResult {
    std::lock_guard<std::mutex> lock(fs_lock_);
    if (!sync_restore_error_.empty()) {
      throw std::runtime_error(sync_restore_error_);
    }
    if constexpr (std::is_void_v<TResult>) {
      operation(*fs_);
      fs_->EvictToLimit();
      return;
    } else {
      TResult result = operation(*fs_);
      fs_->EvictToLimit();
      return result;
    }
  }

  template <typename TResult>
  auto Write(std::function<TResult(FileSystem&)> operation)
      -> std::conditional_t<std::is_void_v<TResult>, SyncResult,
                            std::pair<TResult, SyncResult>> {
    std::lock_guard<std::mutex> lock(fs_lock_);

    if (!sync_restore_error_.empty()) {
      throw std::runtime_error(sync_restore_error_);
    }

    if constexpr (std::is_void_v<TResult>) {
      operation(*fs_);
      SyncResult sync_result = SyncLocked();  // void: only return SyncResult
      fs_->EvictToLimit();
      return sync_result;
    } else {
      TResult    result      = operation(*fs_);
      SyncResult sync_result = SyncLocked();
      fs_->EvictToLimit();
      return {result, sync_result};
    }
  }

  template <typename TResult>
  auto Write_NoSync(std::function<TResult(FileSystem&)> operation) -> TResult {
    std::lock_guard<std::mutex> lock(fs_lock_);
    if (!sync_restore_error_.empty()) {
      throw std::runtime_error(sync_restore_error_);
    }

    if constexpr (std::is_void_v<TResult>) {
      operation(*fs_);
      fs_->EvictToLimit();
      return;
    } else {
      TResult result = operation(*fs_);
      fs_->EvictToLimit();
      return result;
    }
  }


  /// Writes the changed elements to the database in one `fs_lock_` scope. A caller on any
  /// thread can call it. If restoration fails, further access requires reopening the project.
  auto Sync() -> SyncResult override;

  /**
   * @brief The one structure check: true when the user can change the structure of @p folder.
   *
   * It reads `mount_id_` of @p folder only (D6). A mounted folder has a value. The user
   * operations below throw SleeveOperationError with kMountedFolderStructureIsReadOnly for a
   * folder where this is false; the scan entry (the mounted folder group) does not use it.
   */
  [[nodiscard]] static auto CanEditFolderStructure(const SleeveElement& folder) -> bool;
  /// Return the folder ID, or an empty value if the path names no folder.
  /// Throws SleeveOperationError when the folder at @p folder_path is mounted.
  auto                      CheckFolderStructureEditable(const std::filesystem::path& folder_path)
      -> std::optional<sl_element_id_t>;

  auto GetCurrentID() const -> sl_element_id_t;
  auto ResolveElement(const std::filesystem::path& path) -> std::shared_ptr<SleeveElement>;
  auto ResolveFolder(const std::filesystem::path& path) -> std::shared_ptr<SleeveFolder>;
  /// The live child folders and the live files of the folder at @p folder_path, under the read
  /// lock. Reads the file ids with one store read and loads each file element of the folder.
  auto ListFolderEntries(const std::filesystem::path& folder_path)
      -> std::vector<std::shared_ptr<SleeveElement>>;
  /// The live child folders of the folder at @p folder_path under the read lock; loads no file
  /// element (see FileSystem::ListChildFolders). The entries are values that the read copies
  /// under the lock. Throws as that function does.
  auto ListChildFolders(const std::filesystem::path& folder_path) -> std::vector<FolderTreeEntry>;
  /// Every live folder of the library at each of its paths, parents before children, under the
  /// read lock (see FileSystem::ListFolderTree). The entries are values that the read copies
  /// under the lock. Reads memory only.
  auto ListFolderTree() -> std::vector<FolderTreeEntry>;
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
  /// Refuses the whole request when one live file of @p file_ids is mounted.
  auto DeleteFilesEverywhere(std::span<const sl_element_id_t> file_ids)
      -> std::pair<std::vector<std::shared_ptr<SleeveFile>>, SyncResult>;
  /// Deletes the ordinary files of @p file_ids from the library; the mounted files stay
  /// (#1135). Throws like DeleteFilesEverywhere when every id names a mounted file.
  auto DeleteFilesEverywhereKeepingMounted(std::span<const sl_element_id_t> file_ids)
      -> std::pair<LibraryRemoval, SyncResult>;
  auto DuplicateFileToFolder(sl_element_id_t file_id, sl_element_id_t folder_id)
      -> std::pair<std::shared_ptr<SleeveFile>, SyncResult>;
  /// Removes the folder at @p target_path (FileSystem::Delete). A path names a folder only.
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
  /// See alcedo::ReadMountFolders: memory only, no file element and no store read.
  auto ReadMountFolders(sl_element_id_t root_folder_id, mount_id_t mount_id)
      -> std::vector<MountTreeFolder>;
  /// See ReadMountTree.
  auto ReadMountTree(sl_element_id_t root_folder_id, mount_id_t mount_id) -> MountTree;
  /// See alcedo::ApplyMountTreeChanges. Throws like it, before the sync.
  auto ApplyMountTreeChanges(const MountTreeChanges& changes) -> SyncResult;
  /// See alcedo::CreateMountRootFolder. Throws like it, before the sync.
  auto CreateMountRootFolder(const file_name_t& name, mount_id_t mount_id)
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
