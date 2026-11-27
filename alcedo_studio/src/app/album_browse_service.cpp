//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/album_browse_service.hpp"

#include <algorithm>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "storage/query/filter_compiler.hpp"
#include "utils/logging/log.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

auto BuildFolderView(const FolderTreeEntry& entry) -> AlbumFolderView {
  AlbumFolderView out;
  out.folder_id_   = entry.folder_id_;
  out.folder_name_ = entry.name_;
  out.folder_path_ = entry.path_;
  out.mount_id_    = entry.mount_id_;
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

auto IsRootFolderPath(const std::filesystem::path& path) -> bool {
  const auto normalized = path.lexically_normal();
  return normalized.empty() || !normalized.has_relative_path();
}

/// Appends the files of @p files that have an image to out.deleted_files_.
void AppendFileViews(const std::vector<std::shared_ptr<SleeveFile>>& files,
                     sl_element_id_t folder_id, AlbumDeleteResult& out) {
  out.deleted_files_.reserve(out.deleted_files_.size() + files.size());
  for (const auto& file : files) {
    if (file && file->image_id_ != 0) {
      out.deleted_files_.push_back(BuildFileView({}, file, folder_id));
    }
  }
}

/// Puts each id of @p requested_ids that the request neither deleted nor kept as a mounted
/// photo into out.failed_element_ids_.
void CollectFailedIds(const std::vector<sl_element_id_t>& requested_ids, AlbumDeleteResult& out) {
  std::unordered_set<sl_element_id_t> done_ids;
  done_ids.reserve((out.deleted_files_.size() + out.kept_mounted_element_ids_.size()) * 2 + 1);
  for (const auto& file : out.deleted_files_) {
    done_ids.insert(file.element_id_);
  }
  done_ids.insert(out.kept_mounted_element_ids_.begin(), out.kept_mounted_element_ids_.end());
  for (const auto element_id : requested_ids) {
    if (!done_ids.contains(element_id)) {
      out.failed_element_ids_.push_back(element_id);
    }
  }
}

/// Deletes @p delete_ids from the library. The mounted photos of the request stay, and the
/// others are deleted (#1135); a request of only mounted photos is refused.
void DeleteFromLibrary(SleeveServiceImpl& sleeve, const std::vector<sl_element_id_t>& delete_ids,
                       AlbumDeleteResult& out) {
  try {
    auto [removal, sync] = sleeve.DeleteFilesEverywhereKeepingMounted(delete_ids);
    if (sync.success_) {
      AppendFileViews(removal.deleted_files_, 0, out);
      out.kept_mounted_element_ids_ = std::move(removal.kept_mounted_ids_);
    }
  } catch (const SleeveOperationError& e) {
    out.refusal_ = e.Code();
  } catch (...) {
  }
  CollectFailedIds(delete_ids, out);
}

}  // namespace

auto AlbumBrowseService::ListFolders(const std::filesystem::path& folder_path) const
    -> std::vector<AlbumFolderView> {
  std::vector<AlbumFolderView> folders;
  if (!sleeve_service_) {
    return folders;
  }

  try {
    const auto child_folders = sleeve_service_->ListChildFolders(folder_path);
    folders.reserve(child_folders.size());
    for (const auto& entry : child_folders) {
      folders.push_back(BuildFolderView(entry));
    }
  } catch (const std::exception& e) {
    ALCEDO_LOG_WARN(Library) << "AlbumBrowseService: the folders of "
                             << conv::ToBytes(folder_path.generic_wstring())
                             << " did not list: " << e.what();
    throw;
  }

  return folders;
}

auto AlbumBrowseService::ListFolderTree() const -> std::vector<AlbumFolderView> {
  std::vector<AlbumFolderView> folders;
  if (!sleeve_service_) {
    return folders;
  }

  try {
    const auto tree = sleeve_service_->ListFolderTree();
    folders.reserve(tree.size());
    for (const auto& entry : tree) {
      folders.push_back(BuildFolderView(entry));
    }
  } catch (const std::exception& e) {
    ALCEDO_LOG_WARN(Library) << "AlbumBrowseService: the folder tree did not list: " << e.what();
    throw;
  }

  return folders;
}

auto AlbumBrowseService::ListFilesInFolderById(sl_element_id_t folder_id) const
    -> std::vector<AlbumFileView> {
  return ListFilesInFolderById(folder_id, 0, 0);
}

auto AlbumBrowseService::ListFilesInFolderById(sl_element_id_t folder_id, size_t offset,
                                               size_t                           limit,
                                               const std::optional<FilterNode>& filter) const
    -> std::vector<AlbumFileView> {
  std::vector<AlbumFileView> files;
  if (!sleeve_service_) {
    return files;
  }

  try {
    const auto  storage = sleeve_service_->GetStorage();
    const auto& ctrl    = storage->GetElementStore();
    const auto  entries =
        ctrl.ListFilesInFolderPage(folder_id, offset, limit, CompileFilterPredicate(filter));
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

auto AlbumBrowseService::CountFilesInFolderById(sl_element_id_t                  folder_id,
                                                const std::optional<FilterNode>& filter) const
    -> size_t {
  if (!sleeve_service_) {
    return 0;
  }

  try {
    const auto  storage = sleeve_service_->GetStorage();
    const auto& ctrl    = storage->GetElementStore();
    return ctrl.CountFilesInFolder(folder_id, CompileFilterPredicate(filter));
  } catch (...) {
    return 0;
  }
}

auto AlbumBrowseService::ReadProjectOverview() const -> ProjectOverviewCounts {
  if (!sleeve_service_) {
    throw std::runtime_error("Project overview read requires a sleeve service.");
  }
  return sleeve_service_->GetStorage()->GetElementStore().ReadProjectOverview();
}

void AlbumBrowseService::SetQueryThreadObserver(QueryThreadObserver observer) {
  std::lock_guard lock(query_thread_observer_mutex_);
  query_thread_observer_ = std::move(observer);
}

auto AlbumBrowseService::ElementStoreForRead(std::string_view operation) const -> ElementStore& {
  QueryThreadObserver observer;
  {
    std::lock_guard lock(query_thread_observer_mutex_);
    observer = query_thread_observer_;
  }
  if (observer) {
    observer(operation);
  }
  if (!sleeve_service_) {
    throw std::runtime_error("Album query requires an open project.");
  }
  const auto storage = sleeve_service_->GetStorage();
  if (!storage) {
    throw std::runtime_error("Album query requires project storage.");
  }
  return storage->GetElementStore();
}

auto AlbumBrowseService::ReadAlbumQuery(
    const AlbumScope& scope, const std::optional<FilterNode>& filter,
    const AlbumQueryOptions& options, const std::string& active_semantic_model_key,
    const AlbumQueryRead&                        read,
    const std::optional<SemanticQueryEmbedding>& semantic_relevance) const -> AlbumQueryResult {
  auto& store = ElementStoreForRead("ReadAlbumQuery");
  return store.ReadAlbumQuery(scope, CompileFilterPredicate(filter), options,
                              active_semantic_model_key, read, semantic_relevance);
}

auto AlbumBrowseService::ReadAlbumFilePosition(
    const AlbumScope& scope, const std::optional<FilterNode>& filter,
    const AlbumQueryOptions& options, const std::string& active_semantic_model_key,
    sl_element_id_t file_id, const std::optional<AlbumGroupKey>& preferred_group,
    const std::optional<SemanticQueryEmbedding>& semantic_relevance) const
    -> std::optional<AlbumFilePosition> {
  auto& store = ElementStoreForRead("ReadAlbumFilePosition");
  return store.ReadAlbumFilePosition(scope, CompileFilterPredicate(filter), options,
                                     active_semantic_model_key, file_id, preferred_group,
                                     semantic_relevance);
}

auto AlbumBrowseService::ReadAlbumFileIds(
    const AlbumScope& scope, const std::optional<FilterNode>& filter,
    const AlbumQueryOptions& options, const std::string& active_semantic_model_key, int64_t begin,
    int64_t end, const std::optional<SemanticQueryEmbedding>& semantic_relevance) const
    -> std::vector<sl_element_id_t> {
  auto& store = ElementStoreForRead("ReadAlbumFileIds");
  return store.ReadAlbumFileIds(scope, CompileFilterPredicate(filter), options,
                                active_semantic_model_key, begin, end, semantic_relevance);
}

auto AlbumBrowseService::ReadAlbumFileRows(std::span<const sl_element_id_t> file_ids) const
    -> std::vector<SearchResultRow> {
  auto& store = ElementStoreForRead("ReadAlbumFileRows");
  return store.ListSearchResultRows(file_ids);
}

auto AlbumBrowseService::ReadFolderImageCounts() const -> FolderImageCounts {
  return ElementStoreForRead("ReadFolderImageCounts").ReadFolderImageCounts();
}

auto AlbumBrowseService::ReadLastEditedFile() const -> std::optional<LastEditedFile> {
  return ElementStoreForRead("ReadLastEditedFile").ReadLastEditedFile();
}

auto AlbumBrowseService::IsValidFolderName(const file_name_t& name) -> bool {
  if (name.empty() || name == L"." || name == L"..") {
    return false;
  }
  return std::none_of(name.begin(), name.end(), [](const wchar_t ch) {
    return ch == L'/' || ch == L'\\' || ch < 0x20 || ch == 0x7F;
  });
}

auto AlbumBrowseService::CreateFolder(const std::filesystem::path& parent_folder_path,
                                      const file_name_t&           name) -> AlbumFolderEditResult {
  AlbumFolderEditResult out;
  if (!sleeve_service_) {
    return out;
  }
  if (!IsValidFolderName(name)) {
    out.status_ = AlbumFolderEditStatus::InvalidName;
    return out;
  }
  try {
    try {
      (void)sleeve_service_->ResolveFolder(parent_folder_path);
    } catch (...) {
      out.status_ = AlbumFolderEditStatus::UnknownFolder;
      return out;
    }
    const auto [created, sync] = sleeve_service_->CreateFolderEntry(parent_folder_path, name);
    if (!created.has_value()) {
      out.status_ = AlbumFolderEditStatus::NameTaken;
      return out;
    }
    if (!sync.success_) {
      return out;
    }
    out.folder_ = BuildFolderView(*created);
  } catch (const SleeveOperationError&) {
    out.status_ = AlbumFolderEditStatus::StructureIsReadOnly;
    return out;
  } catch (...) {
    return out;
  }
  out.status_ = AlbumFolderEditStatus::Done;
  return out;
}

auto AlbumBrowseService::DeleteFolder(const std::filesystem::path& folder_path)
    -> AlbumFolderEditResult {
  AlbumFolderEditResult out;
  if (!sleeve_service_) {
    return out;
  }
  if (IsRootFolderPath(folder_path)) {
    out.status_ = AlbumFolderEditStatus::RootFolder;
    return out;
  }
  try {
    try {
      (void)sleeve_service_->ResolveFolder(folder_path);
    } catch (...) {
      out.status_ = AlbumFolderEditStatus::UnknownFolder;
      return out;
    }
    if (!sleeve_service_->DeletePath(folder_path).success_) {
      return out;
    }
  } catch (const SleeveOperationError&) {
    out.status_ = AlbumFolderEditStatus::StructureIsReadOnly;
    return out;
  } catch (...) {
    return out;
  }
  out.status_ = AlbumFolderEditStatus::Done;
  return out;
}

auto AlbumBrowseService::RenameFolder(const std::filesystem::path& folder_path,
                                      const file_name_t& new_name) -> AlbumFolderEditResult {
  AlbumFolderEditResult out;
  if (!sleeve_service_) {
    return out;
  }
  if (IsRootFolderPath(folder_path)) {
    out.status_ = AlbumFolderEditStatus::RootFolder;
    return out;
  }
  if (!IsValidFolderName(new_name)) {
    out.status_ = AlbumFolderEditStatus::InvalidName;
    return out;
  }

  const auto      parent_path = folder_path.lexically_normal().parent_path();
  sl_element_id_t folder_id   = 0;
  try {
    std::shared_ptr<SleeveFolder> parent;
    std::shared_ptr<SleeveFolder> folder;
    try {
      parent = sleeve_service_->ResolveFolder(parent_path);
      folder = sleeve_service_->ResolveFolder(folder_path);
    } catch (...) {
      out.status_ = AlbumFolderEditStatus::UnknownFolder;
      return out;
    }
    folder_id = folder->element_id_;
    if (const auto existing = parent->GetElementIdByName(new_name);
        existing.has_value() && existing.value() != folder_id) {
      out.status_ = AlbumFolderEditStatus::NameTaken;
      return out;
    }
    if (!sleeve_service_->RenameFolder(folder_path, new_name).success_) {
      return out;
    }
  } catch (const SleeveOperationError&) {
    out.status_ = AlbumFolderEditStatus::StructureIsReadOnly;
    return out;
  } catch (...) {
    return out;
  }

  // The rename passed the structure check, so the folder is not mounted.
  out.status_ = AlbumFolderEditStatus::Done;
  out.folder_ = AlbumFolderView{.folder_id_   = folder_id,
                                .folder_name_ = new_name,
                                .folder_path_ = parent_path / new_name,
                                .mount_id_    = std::nullopt};
  return out;
}

auto AlbumBrowseService::MoveFolder(const std::filesystem::path& folder_path,
                                    const std::filesystem::path& new_parent_path)
    -> AlbumFolderEditResult {
  AlbumFolderEditResult out;
  if (!sleeve_service_) {
    return out;
  }
  const auto source      = folder_path.lexically_normal();
  const auto destination = IsRootFolderPath(new_parent_path)
                               ? std::filesystem::path{L"/"}.lexically_normal()
                               : new_parent_path.lexically_normal();
  if (IsRootFolderPath(folder_path)) {
    out.status_ = AlbumFolderEditStatus::RootFolder;
    return out;
  }

  const file_name_t name      = source.filename().wstring();
  sl_element_id_t   folder_id = 0;
  try {
    std::shared_ptr<SleeveFolder> folder;
    std::shared_ptr<SleeveFolder> dest;
    try {
      folder = sleeve_service_->ResolveFolder(source);
      dest   = sleeve_service_->ResolveFolder(destination);
    } catch (...) {
      out.status_ = AlbumFolderEditStatus::UnknownFolder;
      return out;
    }
    folder_id = folder->element_id_;
    const auto source_end =
        std::mismatch(source.begin(), source.end(), destination.begin(), destination.end()).first;
    if (source_end == source.end()) {
      out.status_ = AlbumFolderEditStatus::MoveIntoOwnSubtree;
      return out;
    }
    if (source.parent_path() == destination) {
      // A move to the current parent changes nothing; a mounted folder still refuses it.
      sleeve_service_->CheckFolderStructureEditable(source);
    } else {
      if (dest->Contains(name)) {
        out.status_ = AlbumFolderEditStatus::NameTaken;
        return out;
      }
      if (!sleeve_service_->MoveFolder(source, destination).success_) {
        return out;
      }
    }
  } catch (const SleeveOperationError&) {
    out.status_ = AlbumFolderEditStatus::StructureIsReadOnly;
    return out;
  } catch (...) {
    return out;
  }

  // The move passed the structure check, so the folder is not mounted.
  out.status_ = AlbumFolderEditStatus::Done;
  out.folder_ = AlbumFolderView{.folder_id_   = folder_id,
                                .folder_name_ = name,
                                .folder_path_ = destination / name,
                                .mount_id_    = std::nullopt};
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

  DeleteFromLibrary(*sleeve_service_, delete_ids, out);

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

  if (folder_id == 0) {
    DeleteFromLibrary(*sleeve_service_, delete_ids, out);
  } else {
    try {
      auto [deleted_files, sync] = sleeve_service_->DeleteFilesFromFolder(delete_ids, folder_id);
      if (sync.success_) {
        AppendFileViews(deleted_files, folder_id, out);
      }
    } catch (const SleeveOperationError& e) {
      out.refusal_ = e.Code();
    } catch (...) {
    }
    CollectFailedIds(delete_ids, out);
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

  // The photos that the target already holds (#1190). A mounted or missing target has none
  // here: the link of each photo below then reports the refusal or the failure.
  std::unordered_set<sl_element_id_t> members;
  try {
    members = sleeve_service_->Read<std::unordered_set<sl_element_id_t>>(
        [target_folder_id](FileSystem& fs) -> std::unordered_set<sl_element_id_t> {
          const auto folder = fs.Get(target_folder_id);
          if (!folder || folder->type_ != ElementType::FOLDER || !folder->IsLive() ||
              !SleeveServiceImpl::CanEditFolderStructure(*folder)) {
            return {};
          }
          const auto ids = fs.ListFileIds(target_folder_id);
          return {ids.begin(), ids.end()};
        });
  } catch (...) {
    // The membership read failed: no photo is linked.
    for (const auto element_id : element_ids) {
      if (element_id != 0) {
        out.failed_element_ids_.push_back(element_id);
      }
    }
    return out;
  }

  std::unordered_set<sl_element_id_t> seen;
  seen.reserve(element_ids.size() * 2 + 1);

  for (const auto element_id : element_ids) {
    if (element_id == 0 || !seen.insert(element_id).second) {
      continue;
    }
    if (members.contains(element_id)) {
      out.already_linked_element_ids_.push_back(element_id);
      continue;
    }

    try {
      const auto file = sleeve_service_->Read<std::shared_ptr<SleeveFile>>(
          [element_id](FileSystem& fs) -> std::shared_ptr<SleeveFile> {
            const auto element = fs.Get(element_id);
            if (!element || element->type_ != ElementType::FILE || !element->IsLive()) {
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
    } catch (const SleeveOperationError& e) {
      // The target folder refuses every file: the request changes nothing.
      out.refusal_ = e.Code();
      out.failed_element_ids_.push_back(element_id);
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
