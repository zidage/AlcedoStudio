//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mount_tree.hpp"

#include <algorithm>
#include <exception>
#include <format>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

auto IsLive(const std::shared_ptr<SleeveElement>& element) -> bool {
  return element && element->IsLive();
}

/// The parent directory and the last name of a relative path.
auto SplitRelativePath(const std::wstring& relative_path) -> std::pair<std::wstring, std::wstring> {
  const auto separator = relative_path.rfind(L'/');
  if (separator == std::wstring::npos) {
    return {std::wstring{}, relative_path};
  }
  return {relative_path.substr(0, separator), relative_path.substr(separator + 1)};
}

auto JoinRelativePath(const std::wstring& parent, const std::wstring& name) -> std::wstring {
  return parent.empty() ? name : parent + L'/' + name;
}

auto LiveMountRoot(FileSystem& fs, sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> std::shared_ptr<SleeveFolder> {
  const auto root = fs.Get(root_folder_id);
  if (!IsLive(root) || root->type_ != ElementType::FOLDER || root->mount_id_ != mount_id) {
    throw std::runtime_error(
        std::format("MountTree: the element {} is not the live root folder of the mount {}",
                    root_folder_id, mount_id));
  }
  return std::static_pointer_cast<SleeveFolder>(root);
}

/// The folders of the mount by relative path; the empty path names the mount root.
class MountFolderIndex {
 public:
  MountFolderIndex(sl_element_id_t root_folder_id, const MountTree& tree) {
    folder_at_.emplace(std::wstring{}, root_folder_id);
    for (const auto& folder : tree.folders) {
      folder_at_.emplace(folder.relative_path, folder.folder_id);
    }
  }

  [[nodiscard]] auto Contains(const std::wstring& directory) const -> bool {
    return folder_at_.contains(directory);
  }

  [[nodiscard]] auto At(const std::wstring& directory) const -> sl_element_id_t {
    const auto it = folder_at_.find(directory);
    if (it == folder_at_.end()) {
      throw std::runtime_error(std::format("MountTree: the directory '{}' has no mounted folder",
                                           conv::ToBytes(directory)));
    }
    return it->second;
  }

  void Add(const std::wstring& directory, sl_element_id_t folder_id) {
    folder_at_[directory] = folder_id;
  }
  void Remove(const std::wstring& directory) { folder_at_.erase(directory); }

 private:
  std::unordered_map<std::wstring, sl_element_id_t> folder_at_;
};

/**
 * The home links of the mounted files during one apply step: the links of the tree read, then
 * each change of the step. The placements and the folder removals read it instead of loading
 * the file elements of a folder (Sleeve Node Cache Plan, SN5).
 */
class MountLinkIndex {
 public:
  explicit MountLinkIndex(const MountTree& tree) {
    for (const auto& [file_id, folder_ids] : tree.home_folders) {
      for (const auto folder_id : folder_ids) {
        Add(file_id, folder_id);
      }
    }
  }

  /// The folders that hold @p file_id, in id order.
  [[nodiscard]] auto FoldersOf(sl_element_id_t file_id) const -> std::vector<sl_element_id_t> {
    return Sorted(folders_of_, file_id);
  }
  /// The mounted files that @p folder_id holds, in id order.
  [[nodiscard]] auto FilesOf(sl_element_id_t folder_id) const -> std::vector<sl_element_id_t> {
    return Sorted(files_of_, folder_id);
  }

  void Add(sl_element_id_t file_id, sl_element_id_t folder_id) {
    folders_of_[file_id].insert(folder_id);
    files_of_[folder_id].insert(file_id);
  }
  void Remove(sl_element_id_t file_id, sl_element_id_t folder_id) {
    folders_of_[file_id].erase(folder_id);
    files_of_[folder_id].erase(file_id);
  }

 private:
  using IdSets = std::unordered_map<sl_element_id_t, std::unordered_set<sl_element_id_t>>;

  static auto Sorted(const IdSets& sets, sl_element_id_t key) -> std::vector<sl_element_id_t> {
    const auto it = sets.find(key);
    if (it == sets.end()) {
      return {};
    }
    std::vector<sl_element_id_t> ids(it->second.begin(), it->second.end());
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  IdSets folders_of_;
  IdSets files_of_;
};

/// The unlinks of the placements of one apply step, by folder, sent with one call each.
using PendingUnlinks = std::map<sl_element_id_t, std::vector<sl_element_id_t>>;

void PlaceFile(FileSystem& fs, const MountFolderIndex& folders, MountLinkIndex& links,
               PendingUnlinks& unlinks, mount_id_t mount_id,
               const MountTreeFilePlacement& placement) {
  const auto target = folders.At(placement.directory);
  // The placement changes the file, so its element loads.
  auto       file   = fs.Get(placement.file_id);
  if (!IsLive(file) || file->type_ != ElementType::FILE) {
    throw std::runtime_error(
        std::format("MountTree: the element {} is not a live file", placement.file_id));
  }
  if (file->mount_id_ != mount_id) {
    file->mount_id_ = mount_id;
    file->MarkModified();
  }
  for (const auto folder_id : links.FoldersOf(placement.file_id)) {
    if (folder_id != target) {
      unlinks[folder_id].push_back(placement.file_id);
      links.Remove(placement.file_id, folder_id);
    }
  }
  fs.RenameFile(placement.file_id, placement.file_name);
  fs.LinkFileToFolder(placement.file_id, target);
  links.Add(placement.file_id, target);
}

void RemoveFolder(FileSystem& fs, MountFolderIndex& folders, MountLinkIndex& links,
                  const std::wstring& directory) {
  if (directory.empty() || !folders.Contains(directory)) {
    return;
  }
  const auto folder_id = folders.At(directory);
  const auto parent_id = folders.At(SplitRelativePath(directory).first);
  const auto moved_ids = links.FilesOf(folder_id);
  // One membership read of the library root and one of the folder; no file element loads.
  fs.LinkFilesToFolder(moved_ids, parent_id);
  fs.UnlinkFilesFromFolder(moved_ids, folder_id);
  for (const auto file_id : moved_ids) {
    links.Remove(file_id, folder_id);
    links.Add(file_id, parent_id);
  }
  fs.RemoveFolderFrom(parent_id, folder_id);
  folders.Remove(directory);
}

}  // namespace

auto ReadMountFolders(FileSystem& fs, sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> std::vector<MountTreeFolder> {
  const auto                   root = LiveMountRoot(fs, root_folder_id, mount_id);
  std::vector<MountTreeFolder> folders;
  std::vector<std::pair<std::shared_ptr<SleeveFolder>, std::wstring>> pending{
      {root, std::wstring{}}};
  while (!pending.empty()) {
    auto [folder, folder_path] = std::move(pending.back());
    pending.pop_back();
    // A folder holds its child folders only, and each folder is resident (SN3, SN4).
    for (const auto child_id : folder->ListElements()) {
      const auto child = fs.Get(child_id);
      if (!IsLive(child) || child->type_ != ElementType::FOLDER || child->mount_id_ != mount_id) {
        continue;
      }
      auto relative_path = JoinRelativePath(folder_path, child->element_name_);
      folders.push_back(MountTreeFolder{relative_path, child_id});
      pending.emplace_back(std::static_pointer_cast<SleeveFolder>(child), std::move(relative_path));
    }
  }
  std::sort(folders.begin(), folders.end(),
            [](const MountTreeFolder& lhs, const MountTreeFolder& rhs) {
              return lhs.relative_path < rhs.relative_path;
            });
  return folders;
}

auto ReadMountTree(FileSystem& fs, sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> MountTree {
  MountTree tree;
  tree.folders = ReadMountFolders(fs, root_folder_id, mount_id);
  std::vector<sl_element_id_t> folder_ids{root_folder_id};
  folder_ids.reserve(tree.folders.size() + 1);
  for (const auto& folder : tree.folders) {
    folder_ids.push_back(folder.folder_id);
  }
  std::sort(folder_ids.begin(), folder_ids.end());
  folder_ids.erase(std::unique(folder_ids.begin(), folder_ids.end()), folder_ids.end());
  for (const auto& link : fs.ListMountFileLinks(mount_id, folder_ids)) {
    tree.home_folders[link.file_id].push_back(link.folder_id);
  }
  return tree;
}

void ApplyMountTreeChanges(FileSystem& fs, const MountTreeChanges& changes) {
  const auto       tree = ReadMountTree(fs, changes.root_folder_id, changes.mount_id);
  MountFolderIndex folders(changes.root_folder_id, tree);
  MountLinkIndex   links(tree);

  for (const auto& directory : changes.folders_to_add) {
    if (directory.empty() || folders.Contains(directory)) {
      continue;
    }
    const auto [parent_directory, name] = SplitRelativePath(directory);
    const auto parent_id                = folders.At(parent_directory);
    auto       folder                   = fs.CreateFolderIn(parent_id, name);
    folder->mount_id_ = changes.mount_id;
    folders.Add(directory, folder->element_id_);
  }
  PendingUnlinks unlinks;
  for (const auto& placement : changes.placements) {
    PlaceFile(fs, folders, links, unlinks, changes.mount_id, placement);
  }
  // One membership read for each folder that a placed file leaves.
  for (const auto& [folder_id, file_ids] : unlinks) {
    fs.UnlinkFilesFromFolder(file_ids, folder_id);
  }
  for (const auto& directory : changes.folders_to_remove) {
    RemoveFolder(fs, folders, links, directory);
  }
}

auto CreateMountRootFolder(FileSystem& fs, const file_name_t& name, mount_id_t mount_id)
    -> std::shared_ptr<SleeveFolder> {
  // The child folders of the root are resident, and only folder names are unique (D8).
  const auto parent = std::static_pointer_cast<SleeveFolder>(fs.Get(kLibraryRootFolderId));
  if (!IsLive(parent) || parent->type_ != ElementType::FOLDER) {
    throw std::runtime_error("MountTree: the library root cannot be read");
  }
  file_name_t folder_name = name;
  while (parent->Contains(folder_name)) {
    folder_name += L"@";
  }
  auto folder       = fs.CreateFolderIn(kLibraryRootFolderId, folder_name);
  folder->mount_id_ = mount_id;
  return folder;
}

void RemoveMountTree(FileSystem& fs, sl_element_id_t root_folder_id, mount_id_t mount_id,
                     std::span<const sl_element_id_t> file_ids) {
  // The removal changes each file of the mount (its mount id goes), so these elements load.
  const auto make_ordinary = [&fs, mount_id](sl_element_id_t file_id) {
    auto file = fs.Get(file_id);
    if (IsLive(file) && file->type_ == ElementType::FILE && file->mount_id_ == mount_id) {
      file->mount_id_.reset();
      file->MarkModified();
    }
  };
  for (const auto file_id : file_ids) {
    make_ordinary(file_id);
  }

  std::shared_ptr<SleeveElement> root;
  try {
    root = fs.Get(root_folder_id);
  } catch (const std::exception&) {
    // An earlier removal deleted the folders and stopped before the mount record was removed.
    return;
  }
  if (!IsLive(root) || root->type_ != ElementType::FOLDER || root->mount_id_ != mount_id) {
    return;
  }
  // The files above are ordinary in memory now, so the tree holds only the other files.
  const auto tree = ReadMountTree(fs, root_folder_id, mount_id);
  for (const auto& [file_id, _] : tree.home_folders) {
    make_ordinary(file_id);
  }
  // Delete keeps the files of a folder in the library; only the folders of the mount go.
  fs.Delete(root_folder_id);
}

}  // namespace alcedo
