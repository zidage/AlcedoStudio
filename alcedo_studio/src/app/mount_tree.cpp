//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/mount_tree.hpp"

#include <algorithm>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
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

auto LiveFileOfMount(FileSystem& fs, sl_element_id_t file_id) -> std::shared_ptr<SleeveElement> {
  auto file = fs.Get(file_id);
  if (!IsLive(file) || file->type_ != ElementType::FILE) {
    throw std::runtime_error(std::format("MountTree: the element {} is not a live file", file_id));
  }
  return file;
}

/// A mounted file can hold a free name of FileSystem::RenameFile (`name@`) that a new directory
/// has on the disk. A folder takes the name of its directory (P3), so the file takes the next
/// free name.
void FreeNameForFolder(FileSystem& fs, sl_element_id_t parent_id, const std::wstring& name,
                       mount_id_t mount_id) {
  fs.ListFolderContent(parent_id);
  const auto parent = std::static_pointer_cast<SleeveFolder>(fs.Get(parent_id));
  const auto holder = parent->GetElementIdByName(name);
  if (!holder.has_value()) {
    return;
  }
  const auto element = fs.Get(*holder);
  if (IsLive(element) && element->type_ == ElementType::FILE && element->mount_id_ == mount_id) {
    fs.RenameFile(*holder, name + L"@", parent_id);
  }
}

void PlaceFile(FileSystem& fs, const MountTree& tree, const MountFolderIndex& folders,
               mount_id_t mount_id, const MountTreeFilePlacement& placement) {
  const auto target = folders.At(placement.directory);
  auto       file   = LiveFileOfMount(fs, placement.file_id);
  if (file->mount_id_ != mount_id) {
    file->mount_id_ = mount_id;
    file->MarkModified();
  }
  if (const auto it = tree.home_folders.find(placement.file_id); it != tree.home_folders.end()) {
    for (const auto folder_id : it->second) {
      if (folder_id != target) {
        fs.UnlinkFileFromFolder(placement.file_id, folder_id);
      }
    }
  }
  fs.RenameFile(placement.file_id, placement.file_name, target);
  fs.LinkFileToFolder(placement.file_id, target);
}

void RemoveFolder(FileSystem& fs, MountFolderIndex& folders, mount_id_t mount_id,
                  const std::wstring& directory) {
  if (directory.empty() || !folders.Contains(directory)) {
    return;
  }
  const auto folder_id = folders.At(directory);
  const auto parent_id = folders.At(SplitRelativePath(directory).first);
  for (const auto child_id : fs.ListFolderContent(folder_id)) {
    auto child = fs.Get(child_id);
    if (IsLive(child) && child->type_ == ElementType::FILE && child->mount_id_ == mount_id) {
      fs.LinkFileToFolder(child_id, parent_id);
      fs.UnlinkFileFromFolder(child_id, folder_id);
    }
  }
  fs.RemoveFolderFrom(parent_id, folder_id);
  folders.Remove(directory);
}

}  // namespace

auto ReadMountTree(FileSystem& fs, sl_element_id_t root_folder_id, mount_id_t mount_id)
    -> MountTree {
  const auto root = fs.Get(root_folder_id);
  if (!IsLive(root) || root->type_ != ElementType::FOLDER || root->mount_id_ != mount_id) {
    throw std::runtime_error(
        std::format("MountTree: the element {} is not the live root folder of the mount {}",
                    root_folder_id, mount_id));
  }

  MountTree                                             tree;
  std::vector<std::pair<sl_element_id_t, std::wstring>> pending{{root_folder_id, std::wstring{}}};
  while (!pending.empty()) {
    auto [folder_id, folder_path] = std::move(pending.back());
    pending.pop_back();
    for (const auto child_id : fs.ListFolderContent(folder_id)) {
      const auto child = fs.Get(child_id);
      if (!IsLive(child) || child->mount_id_ != mount_id) {
        continue;
      }
      if (child->type_ == ElementType::FOLDER) {
        auto relative_path = JoinRelativePath(folder_path, child->element_name_);
        tree.folders.push_back(MountTreeFolder{relative_path, child_id});
        pending.emplace_back(child_id, std::move(relative_path));
      } else {
        tree.home_folders[child_id].push_back(folder_id);
      }
    }
  }
  std::sort(tree.folders.begin(), tree.folders.end(),
            [](const MountTreeFolder& lhs, const MountTreeFolder& rhs) {
              return lhs.relative_path < rhs.relative_path;
            });
  return tree;
}

void ApplyMountTreeChanges(FileSystem& fs, const MountTreeChanges& changes) {
  const auto       tree = ReadMountTree(fs, changes.root_folder_id, changes.mount_id);
  MountFolderIndex folders(changes.root_folder_id, tree);

  for (const auto& directory : changes.folders_to_add) {
    if (directory.empty() || folders.Contains(directory)) {
      continue;
    }
    const auto [parent_directory, name] = SplitRelativePath(directory);
    const auto parent_id                = folders.At(parent_directory);
    FreeNameForFolder(fs, parent_id, name, changes.mount_id);
    auto folder       = fs.CreateFolderIn(parent_id, name);
    folder->mount_id_ = changes.mount_id;
    folders.Add(directory, folder->element_id_);
  }
  for (const auto& placement : changes.placements) {
    PlaceFile(fs, tree, folders, changes.mount_id, placement);
  }
  for (const auto& directory : changes.folders_to_remove) {
    RemoveFolder(fs, folders, changes.mount_id, directory);
  }
}

auto CreateMountRootFolder(FileSystem& fs, const file_name_t& name, mount_id_t mount_id)
    -> std::shared_ptr<SleeveFolder> {
  // ListFolderContent loads the children of the root, so Contains sees each name.
  fs.ListFolderContent(kLibraryRootFolderId);
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
  const auto tree = ReadMountTree(fs, root_folder_id, mount_id);
  for (const auto& [file_id, _] : tree.home_folders) {
    make_ordinary(file_id);
  }
  // Delete keeps the files of a folder in the library; only the folders of the mount go.
  fs.Delete(root_folder_id);
}

}  // namespace alcedo
