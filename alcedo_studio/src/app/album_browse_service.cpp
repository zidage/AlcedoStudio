//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/album_browse_service.hpp"

#include <algorithm>
#include <unordered_set>

#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

auto BuildFolderView(const std::filesystem::path&         parent_path,
                     const std::shared_ptr<SleeveFolder>& folder) -> AlbumFolderView {
  AlbumFolderView out;
  out.folder_id_   = folder ? folder->element_id_ : 0;
  out.folder_name_ = folder ? folder->element_name_ : file_name_t{};
  out.folder_path_ = parent_path / out.folder_name_;
  return out;
}

auto BuildFileView(const std::filesystem::path&       parent_path,
                   const std::shared_ptr<SleeveFile>& file, sl_element_id_t folder_id)
    -> AlbumFileView {
  AlbumFileView out;
  out.element_id_ = file ? file->element_id_ : 0;
  out.file_id_    = out.element_id_;
  out.image_id_   = file ? file->image_id_ : 0;
  out.folder_id_  = folder_id;
  out.scope_type_ = folder_id == 0 ? AlbumScopeType::Root : AlbumScopeType::Album;
  out.file_name_  = file ? file->element_name_ : file_name_t{};
  out.file_path_  = parent_path / out.file_name_;
  return out;
}

}  // namespace

auto AlbumBrowseService::ListFolders(const std::filesystem::path& folder_path) const
    -> std::vector<AlbumFolderView> {
  std::vector<AlbumFolderView> folders;
  if (!sleeve_service_) {
    return folders;
  }

  try {
    const auto entries = sleeve_service_->ListFolderEntries(folder_path);
    folders.reserve(entries.size());
    for (const auto& entry : entries) {
      std::cout << "[LOG] AlbumBrowseService: Found file/folder "
                << conv::ToBytes(entry->element_name_) << " in " << folder_path.string()
                << " with size " << entries.size() << std::endl;
      if (!entry || entry->type_ != ElementType::FOLDER || entry->sync_flag_ == SyncFlag::DELETED) {
        continue;
      }
      auto folder = std::dynamic_pointer_cast<SleeveFolder>(entry);
      if (!folder) {
        continue;
      }
      folders.push_back(BuildFolderView(folder_path, folder));
    }
  } catch (...) {
    return {};
  }

  return folders;
}

auto AlbumBrowseService::ListFilesInFolder(const std::filesystem::path& folder_path) const
    -> std::vector<AlbumFileView> {
  std::vector<AlbumFileView> files;
  if (!sleeve_service_) {
    return files;
  }

  try {
    const auto folder    = sleeve_service_->ResolveFolder(folder_path);
    const auto folder_id = folder ? folder->element_id_ : 0;
    const auto entries   = sleeve_service_->ListFolderEntries(folder_path);
    files.reserve(entries.size());
    for (const auto& entry : entries) {
      if (!entry || entry->type_ != ElementType::FILE || entry->sync_flag_ == SyncFlag::DELETED) {
        continue;
      }
      auto file = std::dynamic_pointer_cast<SleeveFile>(entry);
      if (!file || file->image_id_ == 0) {
        continue;
      }
      files.push_back(BuildFileView(folder_path, file, folder_id));
    }
  } catch (...) {
    return {};
  }

  return files;
}

auto AlbumBrowseService::ListFilesInFolderById(sl_element_id_t folder_id) const
    -> std::vector<AlbumFileView> {
  return ListFilesInFolderById(folder_id, 0, 0);
}

auto AlbumBrowseService::ListFilesInFolderById(
    sl_element_id_t folder_id, size_t offset, size_t limit,
    const std::optional<std::wstring>& extra_filter_where) const -> std::vector<AlbumFileView> {
  std::vector<AlbumFileView> files;
  if (!sleeve_service_) {
    return files;
  }

  try {
    const auto  storage = sleeve_service_->GetStorage();
    const auto& ctrl    = storage->GetElementStore();
    const auto  entries = ctrl.ListFilesInFolderPage(folder_id, offset, limit, extra_filter_where);
    files.reserve(entries.size());
    for (const auto& entry : entries) {
      if (entry.file_id_ == 0 || entry.image_id_ == 0) {
        continue;
      }
      AlbumFileView view;
      view.element_id_ = entry.file_id_;
      view.file_id_    = entry.file_id_;
      view.image_id_   = entry.image_id_;
      view.folder_id_  = folder_id;
      view.scope_type_ = folder_id == 0 ? AlbumScopeType::Root : AlbumScopeType::Album;
      view.file_name_  = conv::FromBytes(entry.file_name_);
      view.file_path_  = std::filesystem::path{};
      files.push_back(std::move(view));
    }
  } catch (...) {
    return {};
  }

  return files;
}

auto AlbumBrowseService::CountFilesInFolderById(
    sl_element_id_t folder_id, const std::optional<std::wstring>& extra_filter_where) const
    -> size_t {
  if (!sleeve_service_) {
    return 0;
  }

  try {
    const auto  storage = sleeve_service_->GetStorage();
    const auto& ctrl    = storage->GetElementStore();
    return ctrl.CountFilesInFolder(folder_id, extra_filter_where);
  } catch (...) {
    return 0;
  }
}

auto AlbumBrowseService::CreateFolder(const std::filesystem::path& parent_folder_path,
                                      const file_name_t& name) -> std::optional<AlbumFolderView> {
  if (!sleeve_service_) {
    return std::nullopt;
  }

  try {
    const auto result = sleeve_service_->CreateFolder(parent_folder_path, name);
    if (!result.second.success_ || !result.first) {
      return std::nullopt;
    }
    return BuildFolderView(parent_folder_path, result.first);
  } catch (...) {
    return std::nullopt;
  }
}

bool AlbumBrowseService::DeleteFolder(const std::filesystem::path& folder_path) {
  if (!sleeve_service_ || folder_path.empty() || folder_path == std::filesystem::path(L"/")) {
    return false;
  }

  try {
    return sleeve_service_->DeletePath(folder_path).success_;
  } catch (...) {
    return false;
  }
}

auto AlbumBrowseService::DeleteFiles(const std::vector<std::filesystem::path>& file_paths)
    -> AlbumDeleteResult {
  AlbumDeleteResult out;
  if (!sleeve_service_ || file_paths.empty()) {
    return out;
  }

  std::unordered_set<std::wstring> seen;
  seen.reserve(file_paths.size() * 2 + 1);

  for (const auto& path : file_paths) {
    const auto normalized = path.lexically_normal().wstring();
    if (normalized.empty() || !seen.insert(normalized).second) {
      continue;
    }

    try {
      const auto file = sleeve_service_->ResolveFile(path);
      if (!file || file->image_id_ == 0) {
        out.failed_paths_.push_back(path);
        continue;
      }

      const auto parent_folder = sleeve_service_->ResolveFolder(path.parent_path());
      const auto folder_id     = parent_folder ? parent_folder->element_id_ : 0;
      const auto view          = BuildFileView(path.parent_path(), file, folder_id);
      const auto sync          = sleeve_service_->DeletePath(path);
      if (!sync.success_) {
        out.failed_paths_.push_back(path);
        continue;
      }
      out.deleted_files_.push_back(view);
    } catch (...) {
      out.failed_paths_.push_back(path);
    }
  }

  if (filter_service_ && !out.deleted_files_.empty()) {
    filter_service_->InvalidateResultCache();
  }

  return out;
}

auto AlbumBrowseService::DeleteFilesByElementIds(const std::vector<sl_element_id_t>& element_ids)
    -> AlbumDeleteResult {
  AlbumDeleteResult out;
  if (!sleeve_service_ || element_ids.empty()) {
    return out;
  }

  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(element_ids.size() * 2 + 1);

  std::vector<sl_element_id_t> delete_ids;
  delete_ids.reserve(element_ids.size());
  for (const auto element_id : element_ids) {
    if (element_id == 0 || !seen.insert(element_id).second) {
      continue;
    }
    delete_ids.push_back(element_id);
  }

  try {
    auto [deleted_files, sync] = sleeve_service_->DeleteFilesEverywhere(delete_ids);
    if (sync.success_) {
      out.deleted_files_.reserve(deleted_files.size());
      for (const auto& file : deleted_files) {
        if (file && file->image_id_ != 0) {
          out.deleted_files_.push_back(BuildFileView({}, file, 0));
        }
      }
    }
  } catch (...) {
  }

  std::unordered_set<sl_element_id_t> deleted_ids;
  deleted_ids.reserve(out.deleted_files_.size() * 2 + 1);
  for (const auto& file : out.deleted_files_) {
    deleted_ids.insert(file.element_id_);
  }
  for (const auto element_id : delete_ids) {
    if (!deleted_ids.contains(element_id)) {
      out.failed_element_ids_.push_back(element_id);
    }
  }

  if (filter_service_ && !out.deleted_files_.empty()) {
    filter_service_->InvalidateResultCache();
  }

  return out;
}

auto AlbumBrowseService::DeleteFilesInFolderByElementIds(
    sl_element_id_t folder_id, const std::vector<sl_element_id_t>& element_ids)
    -> AlbumDeleteResult {
  AlbumDeleteResult out;
  if (!sleeve_service_ || element_ids.empty()) {
    return out;
  }

  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(element_ids.size() * 2 + 1);

  std::vector<sl_element_id_t> delete_ids;
  delete_ids.reserve(element_ids.size());
  for (const auto element_id : element_ids) {
    if (element_id == 0 || !seen.insert(element_id).second) {
      continue;
    }
    delete_ids.push_back(element_id);
  }

  try {
    auto [deleted_files, sync] =
        folder_id == 0 ? sleeve_service_->DeleteFilesEverywhere(delete_ids)
                       : sleeve_service_->DeleteFilesFromFolder(delete_ids, folder_id);
    if (sync.success_) {
      out.deleted_files_.reserve(deleted_files.size());
      for (const auto& file : deleted_files) {
        if (file && file->image_id_ != 0) {
          out.deleted_files_.push_back(BuildFileView({}, file, folder_id));
        }
      }
    }
  } catch (...) {
  }

  std::unordered_set<sl_element_id_t> deleted_ids;
  deleted_ids.reserve(out.deleted_files_.size() * 2 + 1);
  for (const auto& file : out.deleted_files_) {
    deleted_ids.insert(file.element_id_);
  }
  for (const auto element_id : delete_ids) {
    if (!deleted_ids.contains(element_id)) {
      out.failed_element_ids_.push_back(element_id);
    }
  }

  if (filter_service_ && !out.deleted_files_.empty()) {
    if (folder_id == 0) {
      filter_service_->InvalidateResultCache();
    } else {
      filter_service_->InvalidateResultCache(folder_id);
    }
  }

  return out;
}

auto AlbumBrowseService::LinkFilesToFolder(const std::vector<sl_element_id_t>& element_ids,
                                           sl_element_id_t target_folder_id) -> AlbumDeleteResult {
  AlbumDeleteResult out;
  if (!sleeve_service_ || target_folder_id == 0 || element_ids.empty()) {
    return out;
  }

  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(element_ids.size() * 2 + 1);

  for (const auto element_id : element_ids) {
    if (element_id == 0 || !seen.insert(element_id).second) {
      continue;
    }

    try {
      const auto file = sleeve_service_->Read<std::shared_ptr<SleeveFile>>(
          [element_id](FileSystem& fs) -> std::shared_ptr<SleeveFile> {
            const auto element = fs.Get(element_id);
            if (!element || element->type_ != ElementType::FILE ||
                element->sync_flag_ == SyncFlag::DELETED) {
              return nullptr;
            }
            return std::dynamic_pointer_cast<SleeveFile>(element);
          });
      if (!file || file->image_id_ == 0) {
        out.failed_element_ids_.push_back(element_id);
        continue;
      }

      const auto sync = sleeve_service_->LinkFileToFolder(element_id, target_folder_id);
      if (!sync.success_) {
        out.failed_element_ids_.push_back(element_id);
        continue;
      }
      out.deleted_files_.push_back(BuildFileView({}, file, target_folder_id));
    } catch (...) {
      out.failed_element_ids_.push_back(element_id);
    }
  }

  if (filter_service_ && !out.deleted_files_.empty()) {
    filter_service_->InvalidateResultCache(target_folder_id);
  }

  return out;
}

}  // namespace alcedo
