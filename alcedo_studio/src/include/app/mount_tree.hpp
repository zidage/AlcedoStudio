//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "sleeve/sleeve_filesystem.hpp"
#include "type/type.hpp"

namespace alcedo {

/// One mounted folder below the mount root: its element and its relative path (the folder names
/// from the mount root, joined with `/`; Mounted Folder Plan, P3).
struct MountTreeFolder {
  std::wstring    relative_path;
  sl_element_id_t folder_id                                        = 0;

  auto            operator==(const MountTreeFolder&) const -> bool = default;
};

/**
 * @brief The folders and the home links of one mount, as the sleeve tree holds them.
 *
 * A read result of ReadMountTree for one scan step; the caller drops it after the step.
 */
struct MountTree {
  /// The mounted folders below the mount root, sorted by relative path.
  std::vector<MountTreeFolder>                                      folders      = {};
  /// For each mounted file of the mount: the mounted folders of the mount that hold it. The
  /// invariant is exactly one (the home link); a scan restores it.
  std::unordered_map<sl_element_id_t, std::vector<sl_element_id_t>> home_folders = {};
};

/// The home folder and the name of one mounted file after a scan step: a moved, adopted, or
/// returned file.
struct MountTreeFilePlacement {
  sl_element_id_t file_id = 0;
  /// Relative path of the home folder; empty for the mount root.
  std::wstring    directory;
  /// File name on the disk. The element takes it with FileSystem::RenameFile (a file name is a
  /// label, so other files can hold it).
  file_name_t     file_name;
};

/**
 * @brief The tree part of one scan batch (Mounted Folder Plan, section 2.4 and MF4 step 3).
 *
 * The apply step runs the parts in this order: add folders (a parent before its children), place
 * files, remove folders (a child before its parent).
 */
struct MountTreeChanges {
  mount_id_t                          mount_id          = 0;
  sl_element_id_t                     root_folder_id    = 0;
  /// Relative paths of new mounted folders, sorted ascending.
  std::vector<std::wstring>           folders_to_add    = {};
  std::vector<MountTreeFilePlacement> placements        = {};
  /// Relative paths of mounted folders to remove, sorted descending.
  std::vector<std::wstring>           folders_to_remove = {};
};

/**
 * @brief Read the mounted folders of the mount @p mount_id, sorted by relative path. Reads
 *        memory only: each folder is resident (Sleeve Node Cache Plan, SN3 and SN5).
 *
 * The walk starts at @p root_folder_id and enters only the child folders that hold
 * @p mount_id. An ordinary folder below the mount is skipped.
 *
 * @throws std::runtime_error when @p root_folder_id is not a live folder of the mount.
 * Thread safety: call with the lock of the FileSystem owner (SleeveServiceImpl).
 */
auto ReadMountFolders(FileSystem& fs, sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> std::vector<MountTreeFolder>;

/**
 * @brief Read the mounted folders and the home links of the mount @p mount_id.
 *
 * The folders come from ReadMountFolders. The home links come from one row query of the links
 * of the mount folders with the pending changes of the current write applied
 * (FileSystem::ListMountFileLinks), so no file element of a stored link loads. An ordinary file
 * in a mounted folder is skipped.
 *
 * @throws std::runtime_error when @p root_folder_id is not a live folder of the mount, and the
 *         store error of the query.
 * Thread safety: call with the lock of the FileSystem owner (SleeveServiceImpl).
 */
auto ReadMountTree(FileSystem& fs, sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> MountTree;

/**
 * @brief Apply @p changes to the sleeve tree in memory. The caller syncs the tree afterwards.
 *
 * - A new folder gets the mount id and the name of its directory. A file with that name in the
 *   parent keeps its name (only folder names are unique among the child folders of a parent).
 * - A placed file gets the mount id, is linked into the folder of its directory, loses each other
 *   mounted folder of the mount, and gets the name of its file on the disk.
 * - Before a folder is removed, each mounted file that it still holds moves to its parent (the
 *   nearest folder that stays; Mounted Folder Plan, section 2.4).
 *
 * Only the element of a placed file loads. The step keeps the home links of its tree read
 * current through the placements and the removals; the unlinks of the placements go to one
 * FileSystem::UnlinkFilesFromFolder call for each folder, and a removed folder moves its files
 * with FileSystem::LinkFilesToFolder and UnlinkFilesFromFolder (Sleeve Node Cache Plan, SN5).
 *
 * @throws std::runtime_error when a directory has no folder, or when a FileSystem operation
 *         fails. The tree can then hold a part of the changes; the next scan computes the rest
 *         again (Mounted Folder Plan, MF4 completion record).
 */
void ApplyMountTreeChanges(FileSystem& fs, const MountTreeChanges& changes);

/**
 * @brief Create the root folder of the mount @p mount_id in the library root
 *        (kLibraryRootFolderId). An ordinary collection never holds a mount (#1076).
 *
 * The folder takes @p name, or `name@`, `name@@`, ... when the library root holds a child folder
 * with that name. The relative paths of the mount start below this folder, so its name is free.
 *
 * @throws std::runtime_error when the library root cannot be read.
 */
auto CreateMountRootFolder(FileSystem& fs, const file_name_t& name, mount_id_t mount_id)
    -> std::shared_ptr<SleeveFolder>;

/**
 * @brief Make each file of the mount an ordinary file and delete the folders of the mount.
 *
 * @param file_ids The stored mounted files of the mount. The files that the tree holds as home
 *        links are converted too. Each file stays in the library and in its ordinary folders.
 *
 * When the root folder is no longer in the tree (an earlier removal stopped after its sync), only
 * the files of @p file_ids are converted.
 * @throws std::runtime_error when a FileSystem operation fails.
 */
void RemoveMountTree(FileSystem& fs, sl_element_id_t root_folder_id, mount_id_t mount_id,
                     std::span<const sl_element_id_t> file_ids);

}  // namespace alcedo
