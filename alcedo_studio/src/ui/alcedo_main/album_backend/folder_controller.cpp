//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/folder_controller.hpp"

#include <QDebug>
#include <QUrl>
#include <algorithm>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/sleeve_error.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/import_export.hpp"
#include "ui/alcedo_main/album_backend/library_module.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/album_backend/project_module.hpp"
#include "ui/alcedo_main/album_backend/search_controller.hpp"
#include "ui/alcedo_main/album_backend/search_request_worker.hpp"
#include "ui/alcedo_main/album_backend/stats_engine.hpp"
#include "ui/alcedo_main/album_backend/ui_status_sink.hpp"
#include "utils/diagnostics/app_logging.hpp"
#include "utils/string/convert.hpp"

namespace alcedo::ui {

#define PL_TEXT(text, ...)                     \
  i18n::MakeLocalizedText(ALCEDO_I18N_CONTEXT, \
                          QT_TRANSLATE_NOOP(ALCEDO_I18N_CONTEXT, text) __VA_OPT__(, ) __VA_ARGS__)

FolderController::FolderController(ProjectModule* project, LibraryModule* library,
                                   IUiStatusSink* status, QObject* parent)
    : QObject(parent), project_(project), library_(library), status_(status) {
  current_folder_path_      = album_util::RootFsPath();
  current_folder_path_text_ = album_util::RootPathText();
}

FolderController::~FolderController() {
  if (library_ != nullptr) {
    library_->query_worker().InvalidateAndWait(SearchRequestKind::kFolderCounts);
    library_->query_worker().InvalidateAndWait(SearchRequestKind::kFolderTree);
  }
}

void FolderController::BindCollaborators(StatsEngine* stats, SearchController* search,
                                         ImportExportHandler* import_export) {
  stats_         = stats;
  search_        = search;
  import_export_ = import_export;
}

auto FolderController::PathKey(const std::filesystem::path& path) const -> std::wstring {
  auto normalized = path.lexically_normal();
  if (normalized.empty()) {
    normalized = album_util::RootFsPath();
  }
  return normalized.generic_wstring();
}

void FolderController::EnsureRootNode() {
  const auto root_path = album_util::RootFsPath();
  const auto key       = PathKey(root_path);
  if (nodes_by_path_.contains(key)) {
    return;
  }

  FolderNodeState root;
  root.ui_id_       = 0;
  root.element_id_  = kLibraryRootFolderId;
  root.folder_path_ = root_path;
  root.depth_       = 0;
  root.expanded_    = true;
  nodes_by_path_.emplace(key, root);
  path_key_by_ui_id_[0] = key;
}

void FolderController::ResetTreeState() {
  nodes_by_path_.clear();
  child_keys_by_path_.clear();
  path_key_by_ui_id_.clear();
  tree_loaded_ = false;
  folder_entries_.clear();
  folders_.clear();
  next_folder_ui_id_ = 1;
  EnsureRootNode();
}

auto FolderController::EnsureNode(const std::filesystem::path& folderPath,
                                  const file_name_t& folderName, int depth) -> FolderNodeState& {
  const auto key = PathKey(folderPath);
  if (const auto it = nodes_by_path_.find(key); it != nodes_by_path_.end()) {
    it->second.folder_name_ = folderName;
    it->second.folder_path_ = folderPath;
    it->second.depth_       = depth;
    return it->second;
  }

  FolderNodeState node;
  node.ui_id_                           = next_folder_ui_id_++;
  node.element_id_                      = 0;
  node.folder_name_                     = folderName;
  node.folder_path_                     = folderPath;
  node.depth_                           = depth;

  auto [it, _]                          = nodes_by_path_.emplace(key, std::move(node));
  path_key_by_ui_id_[it->second.ui_id_] = key;
  return it->second;
}

void FolderController::ReloadTree(const std::filesystem::path& preferredFolderPath) {
  auto proj   = project_ != nullptr ? project_->handler().project() : nullptr;
  auto browse = proj ? proj->GetAlbumBrowseService() : nullptr;
  if (library_ == nullptr || !browse) {
    return;
  }
  // The root row and its element id stay available while the read runs.
  EnsureRootNode();
  if (!preferredFolderPath.empty()) {
    reload_selection_path_ = preferredFolderPath;
  }
  tree_read_pending_ = true;

  // The destructor waits for a running job, and Qt drops the posted result of a destroyed
  // receiver, so the job and the result may use `this`. The vector that ListFolderTree returns
  // moves to the UI thread; no other object keeps the tree.
  auto job           = [this, browse = std::move(browse)](std::uint64_t generation) {
    std::vector<AlbumFolderView> folders;
    QString                      error;
    try {
      folders = browse->ListFolderTree();
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
      if (error.isEmpty()) {
        error = QStringLiteral("the read failed with no message");
      }
    }
    QMetaObject::invokeMethod(
        this,
        [this, generation, folders = std::move(folders), error]() mutable {
          ApplyTree(generation, std::move(folders), error);
        },
        Qt::QueuedConnection);
  };
  library_->query_worker().Submit(SearchRequestKind::kFolderTree, std::move(job));
}

void FolderController::ApplyTree(std::uint64_t generation, std::vector<AlbumFolderView> folders,
                                 const QString& error) {
  // Why the check exists (AGENTS.md, executable interleaving): the read of project A runs on the
  // query worker while the UI thread switches the project (ProjectHandler: project_ = B, then
  // ClearState through clear_project_ui_state, then ReloadTree for B). The queued result of A
  // then arrives after ClearState, and without the check the rows of A, their selection, and a
  // Library reload with a folder id of A apply to project B. ClearState invalidates the read;
  // FolderTests.TreeReadOfAClosedProjectDoesNotFillTheTree drives that order. A newer ReloadTree
  // also replaces an older read; the newer read applies the current tree.
  if (!library_->query_worker().IsCurrent(SearchRequestKind::kFolderTree, generation)) {
    return;
  }
  tree_read_pending_         = false;
  const auto selected_before = SelectedNodeElementId();
  auto       target_path     = reload_selection_path_.value_or(current_folder_path_);
  reload_selection_path_.reset();

  ResetTreeState();
  if (!error.isEmpty()) {
    // The tree stays unloaded, so the next selection reads it again.
    qCWarning(diag::libraryLog).noquote()
        << "FolderController: the folder tree did not load:" << error;
  } else {
    // Parents come before their children, so the parent node of each folder exists.
    for (const auto& folder : folders) {
      const auto parent_key = PathKey(folder.folder_path_.parent_path());
      const auto parent_it  = nodes_by_path_.find(parent_key);
      if (parent_it == nodes_by_path_.end()) {
        continue;
      }
      const int depth   = parent_it->second.depth_ + 1;
      auto&     child   = EnsureNode(folder.folder_path_, folder.folder_name_, depth);
      child.element_id_ = folder.folder_id_;
      child.mount_id_   = folder.mount_id_;
      child_keys_by_path_[parent_key].push_back(PathKey(child.folder_path_));
    }
    tree_loaded_ = true;
  }

  if (!nodes_by_path_.contains(PathKey(target_path))) {
    target_path = album_util::RootFsPath();
  }
  // The sidebar tree and the "Add to" menu list every folder, so each folder shows expanded.
  for (auto& [key, node] : nodes_by_path_) {
    node.expanded_ = true;
  }
  const auto node_it = nodes_by_path_.find(PathKey(target_path));
  ApplyFolderSelection(node_it != nodes_by_path_.end() ? node_it->second.ui_id_ : 0, true);
  if (SelectedNodeElementId() != selected_before) {
    ReloadScopeOfMovedSelection();
  }
  RefreshImageCounts();
}

auto FolderController::SelectedNodeElementId() const -> std::optional<sl_element_id_t> {
  const auto it = nodes_by_path_.find(PathKey(current_folder_path_));
  if (it == nodes_by_path_.end()) {
    return std::nullopt;
  }
  return it->second.element_id_;
}

void FolderController::ReloadScopeOfMovedSelection() {
  if (stats_) stats_->ClearFilters();
  library_->ReloadCurrentFolder();
  if (stats_) emit stats_->StatsFilterChanged();
}

void FolderController::EnsurePathExpanded(const std::filesystem::path& folderPath) {
  EnsureRootNode();

  auto normalized = folderPath.lexically_normal();
  if (normalized.empty()) {
    normalized = album_util::RootFsPath();
  }

  auto current                                  = album_util::RootFsPath();
  nodes_by_path_.at(PathKey(current)).expanded_ = true;
  for (const auto& part : normalized.relative_path()) {
    current /= part;
    auto it = nodes_by_path_.find(PathKey(current));
    if (it == nodes_by_path_.end()) {
      break;
    }
    it->second.expanded_ = true;
  }
}

void FolderController::AppendVisibleEntries(const std::filesystem::path&      folderPath,
                                            std::vector<ExistingFolderEntry>& out) const {
  const auto key = PathKey(folderPath);
  const auto it  = nodes_by_path_.find(key);
  if (it == nodes_by_path_.end()) {
    return;
  }

  out.push_back({it->second.ui_id_, it->second.element_id_, it->second.folder_name_,
                 it->second.folder_path_, it->second.depth_, it->second.expanded_,
                 it->second.mount_id_});

  if (!it->second.expanded_) {
    return;
  }

  const auto child_it = child_keys_by_path_.find(key);
  if (child_it == child_keys_by_path_.end()) {
    qCDebug(diag::libraryLog).noquote()
        << "FolderController: no children for" << QString::fromStdString(conv::ToBytes(key));
    return;
  }

  for (const auto& child_key : child_it->second) {
    const auto node_it = nodes_by_path_.find(child_key);
    if (node_it == nodes_by_path_.end()) {
      continue;
    }
    AppendVisibleEntries(node_it->second.folder_path_, out);
  }
}

void FolderController::RebuildFolderView() {
  std::vector<ExistingFolderEntry> next_entries;
  AppendVisibleEntries(album_util::RootFsPath(), next_entries);
  folder_entries_ = std::move(next_entries);

  QVariantList next;
  next.reserve(static_cast<qsizetype>(folder_entries_.size()));

  for (const auto& folder : folder_entries_) {
    const QString name = folder.ui_id_ == 0 ? PL_TEXT("Root").Render()
                                            : album_util::WStringToQString(folder.folder_name_);
    const auto key             = PathKey(folder.folder_path_);
    const auto child_it        = child_keys_by_path_.find(key);
    const int  child_count     = child_it != child_keys_by_path_.end()
                                     ? static_cast<int>(child_it->second.size())
                                     : 0;
    uint       parent_ui_id      = 0;
    uint       parent_element_id = 0;
    if (folder.ui_id_ != 0) {
      const auto parent_it = nodes_by_path_.find(PathKey(folder.folder_path_.parent_path()));
      if (parent_it != nodes_by_path_.end()) {
        parent_ui_id      = parent_it->second.ui_id_;
        parent_element_id = static_cast<uint>(parent_it->second.element_id_);
      }
    }
    // parentFolderId of the root row is 0 as well; the root row is the one with folderId 0.
    QVariantMap row{{"folderId", folder.ui_id_},
                    {"elementId", static_cast<uint>(folder.element_id_)},
                    {"parentFolderId", parent_ui_id},
                    {"parentElementId", parent_element_id},
                    {"name", name},
                    {"depth", folder.depth_},
                    {"path", album_util::FolderPathToDisplay(folder.folder_path_)},
                    {"deletable", folder.ui_id_ != 0},
                    {"childCount", child_count},
                    {"hasChildren", child_count > 0},
                    {"imageCount", RowImageCount(folder)},
                    {"expanded", folder.expanded_}};
    AddMountFields(folder, row);
    next.push_back(std::move(row));
  }

  folders_ = std::move(next);
  emit FoldersChanged();
}

auto FolderController::RowImageCount(const ExistingFolderEntry& folder) const -> qint64 {
  if (!image_counts_loaded_) {
    return -1;
  }
  // The root row is the Library row: the photos directly in the root and in the collections,
  // without the mount trees (#1076). All Photos reads allPhotosCount.
  if (folder.ui_id_ == 0) {
    return static_cast<qint64>(library_root_image_count_);
  }
  const auto it = image_count_by_folder_.find(folder.element_id_);
  return it != image_count_by_folder_.end() ? static_cast<qint64>(it->second) : 0;
}

void FolderController::RefreshImageCounts() {
  auto proj   = project_ != nullptr ? project_->handler().project() : nullptr;
  auto browse = proj ? proj->GetAlbumBrowseService() : nullptr;
  if (library_ == nullptr || !browse) {
    return;
  }
  // The destructor waits for a running job, and Qt drops the posted result of a destroyed
  // receiver, so the job and the result may use `this`.
  auto job = [this, browse = std::move(browse),
              mounts = proj->GetMountService()](std::uint64_t generation) {
    FolderImageCounts         counts;
    std::vector<MountSummary> summaries;
    QString                   error;
    try {
      counts = browse->ReadFolderImageCounts();
      if (mounts) {
        summaries = mounts->ListMounts();
      }
    } catch (const std::exception& e) {
      error = QString::fromUtf8(e.what());
    }
    QMetaObject::invokeMethod(
        this,
        [this, generation, counts = std::move(counts), summaries = std::move(summaries),
         error]() mutable {
          if (!library_->query_worker().IsCurrent(SearchRequestKind::kFolderCounts, generation)) {
            return;
          }
          if (error.isEmpty()) {
            ApplyMountSummaries(std::move(summaries));
          }
          ApplyImageCounts(generation, std::move(counts), error);
        },
        Qt::QueuedConnection);
  };
  library_->query_worker().Submit(SearchRequestKind::kFolderCounts, std::move(job));
}

void FolderController::ApplyImageCounts(std::uint64_t generation, FolderImageCounts counts,
                                        const QString& error) {
  if (!library_->query_worker().IsCurrent(SearchRequestKind::kFolderCounts, generation)) {
    // A newer read, or a project close, replaced this one.
    return;
  }
  if (!error.isEmpty()) {
    // A failed read shows no count rather than an old one.
    qCWarning(diag::libraryLog).noquote() << "Collection photo count read failed:" << error;
    image_counts_loaded_ = false;
    image_count_by_folder_.clear();
  } else {
    image_counts_loaded_      = true;
    project_image_count_      = counts.project_photo_count_;
    library_root_image_count_ = counts.library_root_photo_count_;
    image_count_by_folder_    = std::move(counts.folder_photo_counts_);
  }
  RebuildFolderView();
}

void FolderController::DropImageCounts() {
  if (library_ != nullptr) {
    library_->query_worker().Invalidate(SearchRequestKind::kFolderCounts);
  }
  image_counts_loaded_      = false;
  project_image_count_      = 0;
  library_root_image_count_ = 0;
  image_count_by_folder_.clear();
  mount_summaries_.clear();
  present_mounted_ids_.clear();
  missing_mounted_ids_.clear();
}

auto FolderController::TryGetPathForUiId(uint folderUiId) const
    -> std::optional<std::filesystem::path> {
  const auto key_it = path_key_by_ui_id_.find(folderUiId);
  if (key_it == path_key_by_ui_id_.end()) {
    return std::nullopt;
  }
  const auto node_it = nodes_by_path_.find(key_it->second);
  if (node_it == nodes_by_path_.end()) {
    return std::nullopt;
  }
  return node_it->second.folder_path_;
}

void FolderController::ApplyFolderSelection(uint folderUiId, bool emitSignal) {
  auto path_opt  = TryGetPathForUiId(folderUiId);
  auto next_path = path_opt.has_value() ? path_opt.value() : album_util::RootFsPath();
  EnsurePathExpanded(next_path);

  const auto     key          = PathKey(next_path);
  const auto     it           = nodes_by_path_.find(key);
  const uint32_t next_ui_id   = it != nodes_by_path_.end() ? it->second.ui_id_ : 0;

  // A folder other than the root is never the Library row.
  const bool     root_changed = next_ui_id != 0 && std::exchange(library_root_selected_, false);
  const bool     id_changed   = current_folder_ui_id_ != next_ui_id || root_changed;
  const bool     path_changed = current_folder_path_ != next_path;
  current_folder_ui_id_       = next_ui_id;
  current_folder_path_        = next_path;

  const QString next_path_ui  = album_util::FolderPathToDisplay(current_folder_path_);
  const bool    text_changed  = current_folder_path_text_ != next_path_ui;
  current_folder_path_text_   = next_path_ui;

  qCDebug(diag::libraryLog).noquote() << "FolderController: folder selected:" << next_path_ui;

  RebuildFolderView();

  if (emitSignal || id_changed || path_changed || text_changed) {
    emit FolderSelectionChanged();
    emit MountStateChanged();
    emit folderSelectionChanged();
  }
}

auto FolderController::CurrentFolderFsPath() const -> std::filesystem::path {
  return current_folder_path_;
}

auto FolderController::CurrentFolderElementId() const -> std::optional<sl_element_id_t> {
  if (project_ == nullptr || !project_->handler().project()) {
    return std::nullopt;
  }
  return SelectedNodeElementId();
}

auto FolderController::FolderElementIdForUiId(uint folderUiId) const
    -> std::optional<sl_element_id_t> {
  if (project_ == nullptr || !project_->handler().project()) {
    return std::nullopt;
  }
  const auto key_it = path_key_by_ui_id_.find(folderUiId);
  if (key_it == path_key_by_ui_id_.end()) {
    return std::nullopt;
  }
  const auto node_it = nodes_by_path_.find(key_it->second);
  if (node_it == nodes_by_path_.end()) {
    return std::nullopt;
  }
  return node_it->second.element_id_;
}

auto FolderController::FolderUiIdForElementId(const sl_element_id_t elementId) const
    -> std::optional<uint> {
  for (const auto& [key, node] : nodes_by_path_) {
    if (node.element_id_ == elementId) {
      return node.ui_id_;
    }
  }
  return std::nullopt;
}

void FolderController::SelectFolder(uint folderUiId) {
  auto& ph = project_->handler();
  if (ph.project_loading() || !ph.project()) {
    return;
  }

  library_root_selected_ = false;
  // The selection of the user replaces the target of a pending tree read.
  reload_selection_path_.reset();
  ApplyFolderSelection(folderUiId, true);
  ReloadSelectedScope();
  if (!tree_loaded_ && !tree_read_pending_) {
    // The last tree read failed: read it again.
    ReloadTree();
  }
}

void FolderController::SelectLibraryRoot() {
  auto& ph = project_->handler();
  if (ph.project_loading() || !ph.project()) {
    return;
  }

  library_root_selected_ = true;
  reload_selection_path_.reset();
  ApplyFolderSelection(0, true);
  ReloadSelectedScope();
  if (!tree_loaded_ && !tree_read_pending_) {
    ReloadTree();
  }
}

void FolderController::ReloadSelectedScope() {
  if (stats_) stats_->ClearFilters();
  if (search_) search_->ClearSearchState(false);
  library_->ReloadCurrentFolder();
  if (stats_) emit stats_->StatsFilterChanged();
  if (search_) emit search_->SearchStateChanged();
}

bool FolderController::CheckFolderEditAllowed(FolderEdit edit) const {
  auto& ph = project_->handler();
  if (ph.project_loading()) {
    status_->SetTaskState(PL_TEXT("Project is loading. Please wait."), 0, false);
    return false;
  }
  if (!ph.project()) {
    status_->SetTaskState(PL_TEXT("No project is loaded."), 0, false);
    return false;
  }
  auto*      ie            = import_export_;
  const bool import_active =
      ie && ie->current_import_job() && !ie->current_import_job()->IsCancelationAcked();
  const bool export_active = ie && ie->export_inflight();
  if (!import_active && !export_active) {
    return true;
  }
  switch (edit) {
    case FolderEdit::Create:
      status_->SetTaskState(import_active
                                ? PL_TEXT("Cannot create folder while import is running.")
                                : PL_TEXT("Cannot create folder while export is running."),
                            0, false);
      break;
    case FolderEdit::Rename:
      status_->SetTaskState(import_active
                                ? PL_TEXT("Cannot rename folder while import is running.")
                                : PL_TEXT("Cannot rename folder while export is running."),
                            0, false);
      break;
    case FolderEdit::Move:
      status_->SetTaskState(import_active
                                ? PL_TEXT("Cannot move folder while import is running.")
                                : PL_TEXT("Cannot move folder while export is running."),
                            0, false);
      break;
    case FolderEdit::Delete:
      status_->SetTaskState(import_active
                                ? PL_TEXT("Cannot delete folder while import is running.")
                                : PL_TEXT("Cannot delete folder while export is running."),
                            0, false);
      break;
  }
  return false;
}

auto FolderController::ValidatedFolderName(const QString& name) const -> std::optional<QString> {
  const QString trimmed = name.trimmed();
  if (trimmed.isEmpty()) {
    status_->SetTaskState(PL_TEXT("Folder name cannot be empty."), 0, false);
    return std::nullopt;
  }
  if (trimmed.contains('/') || trimmed.contains('\\')) {
    status_->SetTaskState(PL_TEXT("Folder name cannot contain '/' or '\\'."), 0, false);
    return std::nullopt;
  }
  return trimmed;
}

bool FolderController::SaveProjectState() const {
  auto& ph = project_->handler();
  try {
    if (!ph.meta_path().empty()) {
      ph.project()->SaveProject(ph.meta_path());
    }
    QString ignored_error;
    return ph.PackageCurrentProjectFiles(&ignored_error);
  } catch (...) {
    return false;
  }
}

void FolderController::ReportFolderEditDone(const i18n::LocalizedText& message,
                                            bool                       save_ok) const {
  auto msg = message;
  if (!save_ok) {
    msg = PL_TEXT("%1 Project state save failed.", message.Render());
  }
  status_->SetServiceMessage(msg);
  status_->SetTaskState(msg, 100, false);
  status_->ScheduleIdleTaskStateReset(1200);
}

namespace {

auto IsAtOrBelow(const std::filesystem::path& path, const std::filesystem::path& prefix) -> bool {
  const auto normalized  = path.lexically_normal();
  const auto prefix_norm = prefix.lexically_normal();
  return std::mismatch(prefix_norm.begin(), prefix_norm.end(), normalized.begin(),
                       normalized.end())
             .first == prefix_norm.end();
}

/// @p path with its @p old_prefix replaced by @p new_prefix; @p path itself when it does not
/// lie at or below @p old_prefix.
auto RemapFolderPath(const std::filesystem::path& path, const std::filesystem::path& old_prefix,
                     const std::filesystem::path& new_prefix) -> std::filesystem::path {
  if (!IsAtOrBelow(path, old_prefix)) {
    return path;
  }
  const auto normalized = path.lexically_normal();
  const auto old_norm   = old_prefix.lexically_normal();
  const auto rest =
      std::mismatch(old_norm.begin(), old_norm.end(), normalized.begin(), normalized.end())
          .second;
  auto remapped = new_prefix.lexically_normal();
  for (auto it = rest; it != normalized.end(); ++it) {
    remapped /= *it;
  }
  return remapped;
}

}  // namespace

void FolderController::CreateFolder(const QString& folderName) {
  static_cast<void>(CreateFolderIn(current_folder_ui_id_, folderName));
}

uint FolderController::CreateFolderIn(uint parentFolderUiId, const QString& folderName) {
  if (!CheckFolderEditAllowed(FolderEdit::Create)) {
    return 0;
  }
  const auto name = ValidatedFolderName(folderName);
  if (!name.has_value()) {
    return 0;
  }
  const auto parent_path = TryGetPathForUiId(parentFolderUiId);
  if (!parent_path.has_value()) {
    status_->SetTaskState(PL_TEXT("Failed to create folder."), 0, false);
    return 0;
  }

  auto browse = project_->handler().project()->GetAlbumBrowseService();
  if (!browse) {
    status_->SetTaskState(PL_TEXT("Folder service is unavailable."), 0, false);
    return 0;
  }

  std::optional<AlbumFolderView> created;
  try {
    created = browse->CreateFolder(parent_path.value(), name->toStdWString());
  } catch (const SleeveOperationError&) {
    status_->SetTaskState(PL_TEXT("The structure of a mounted folder is read-only."), 0, false);
    return 0;
  }
  if (!created.has_value()) {
    status_->SetTaskState(PL_TEXT("Failed to create folder."), 0, false);
    return 0;
  }

  const bool save_ok = SaveProjectState();
  library_->ReloadFolderTree();
  ReportFolderEditDone(
      PL_TEXT("Created folder %1", album_util::WStringToQString(created->folder_name_)),
      save_ok);
  return static_cast<uint>(created->folder_id_);
}

bool FolderController::RenameFolder(uint folderUiId, const QString& newName) {
  if (!CheckFolderEditAllowed(FolderEdit::Rename)) {
    return false;
  }
  const auto name = ValidatedFolderName(newName);
  if (!name.has_value()) {
    return false;
  }
  const auto path_opt = TryGetPathForUiId(folderUiId);
  if (!path_opt.has_value() || folderUiId == 0) {
    status_->SetTaskState(PL_TEXT("Root folder cannot be renamed."), 0, false);
    return false;
  }

  auto browse = project_->handler().project()->GetAlbumBrowseService();
  if (!browse) {
    status_->SetTaskState(PL_TEXT("Folder service is unavailable."), 0, false);
    return false;
  }

  const auto result = browse->RenameFolder(path_opt.value(), name->toStdWString());
  switch (result.status_) {
    case AlbumFolderEditStatus::Done:
      break;
    case AlbumFolderEditStatus::NameTaken:
      status_->SetTaskState(PL_TEXT("A folder named %1 already exists here.", name.value()), 0,
                            false);
      return false;
    case AlbumFolderEditStatus::InvalidTarget:
    case AlbumFolderEditStatus::Failed:
      status_->SetTaskState(PL_TEXT("Failed to rename folder."), 0, false);
      return false;
    case AlbumFolderEditStatus::StructureIsReadOnly:
      status_->SetTaskState(PL_TEXT("The structure of a mounted folder is read-only."), 0, false);
      return false;
  }

  const bool save_ok = SaveProjectState();
  library_->ReloadFolderTree(
      RemapFolderPath(current_folder_path_, path_opt.value(), result.folder_path_));
  ReportFolderEditDone(PL_TEXT("Renamed folder to %1", name.value()), save_ok);
  return true;
}

bool FolderController::MoveFolder(uint folderUiId, uint newParentFolderUiId) {
  if (!CheckFolderEditAllowed(FolderEdit::Move)) {
    return false;
  }
  const auto path_opt   = TryGetPathForUiId(folderUiId);
  const auto parent_opt = TryGetPathForUiId(newParentFolderUiId);
  if (!path_opt.has_value() || folderUiId == 0) {
    status_->SetTaskState(PL_TEXT("Root folder cannot be moved."), 0, false);
    return false;
  }
  if (!parent_opt.has_value()) {
    status_->SetTaskState(PL_TEXT("Failed to move folder."), 0, false);
    return false;
  }

  auto browse = project_->handler().project()->GetAlbumBrowseService();
  if (!browse) {
    status_->SetTaskState(PL_TEXT("Folder service is unavailable."), 0, false);
    return false;
  }

  const auto result = browse->MoveFolder(path_opt.value(), parent_opt.value());
  switch (result.status_) {
    case AlbumFolderEditStatus::Done:
      break;
    case AlbumFolderEditStatus::NameTaken:
      status_->SetTaskState(PL_TEXT("The destination already has a folder with this name."), 0,
                            false);
      return false;
    case AlbumFolderEditStatus::InvalidTarget:
      status_->SetTaskState(PL_TEXT("A folder cannot move into itself or into its subfolder."),
                            0, false);
      return false;
    case AlbumFolderEditStatus::Failed:
      status_->SetTaskState(PL_TEXT("Failed to move folder."), 0, false);
      return false;
    case AlbumFolderEditStatus::StructureIsReadOnly:
      status_->SetTaskState(PL_TEXT("The structure of a mounted folder is read-only."), 0, false);
      return false;
  }

  const bool save_ok = SaveProjectState();
  library_->ReloadFolderTree(
      RemapFolderPath(current_folder_path_, path_opt.value(), result.folder_path_));
  ReportFolderEditDone(PL_TEXT("Folder moved."), save_ok);
  return true;
}

void FolderController::DeleteFolder(uint folderUiId) {
  if (!CheckFolderEditAllowed(FolderEdit::Delete)) {
    return;
  }
  const auto path_opt = TryGetPathForUiId(folderUiId);
  if (!path_opt.has_value() || path_opt.value() == album_util::RootFsPath()) {
    status_->SetTaskState(PL_TEXT("Root folder cannot be deleted."), 0, false);
    return;
  }

  const auto folder_path = path_opt.value();
  const auto parent_path =
      folder_path.parent_path().empty() ? album_util::RootFsPath() : folder_path.parent_path();
  // Deleting a folder that holds the current folder selects its parent; any other folder keeps
  // the current selection. The tree read then clears the filters and reloads the Library when
  // the selection moves (ApplyTree).
  const bool deleted_holds_current = IsAtOrBelow(current_folder_path_, folder_path);
  const auto next_path             = deleted_holds_current ? parent_path : std::filesystem::path{};

  auto       browse                = project_->handler().project()->GetAlbumBrowseService();
  bool       deleted               = false;
  try {
    deleted = browse && browse->DeleteFolder(folder_path);
  } catch (const SleeveOperationError&) {
    status_->SetTaskState(PL_TEXT("The structure of a mounted folder is read-only."), 0, false);
    return;
  }
  if (!deleted) {
    status_->SetTaskState(PL_TEXT("Failed to delete folder."), 0, false);
    return;
  }

  const bool save_ok = SaveProjectState();
  library_->ReloadFolderTree(next_path);
  ReportFolderEditDone(PL_TEXT("Folder deleted."), save_ok);
}

void FolderController::ClearState() {
  if (library_ != nullptr) {
    // A tree read of the closed project applies nothing (ApplyTree).
    library_->query_worker().Invalidate(SearchRequestKind::kFolderTree);
  }
  tree_read_pending_ = false;
  reload_selection_path_.reset();
  nodes_by_path_.clear();
  child_keys_by_path_.clear();
  path_key_by_ui_id_.clear();
  tree_loaded_ = false;
  folder_entries_.clear();
  folders_.clear();
  current_folder_path_      = album_util::RootFsPath();
  current_folder_path_text_ = album_util::RootPathText();
  current_folder_ui_id_     = 0;
  next_folder_ui_id_        = 1;
  library_root_selected_    = false;
  DropImageCounts();
  DismissArrivedVolume();
}

void FolderController::BindMountScans(MountScanCoordinator* mount_scans) {
  if (mount_scans_ != nullptr) {
    disconnect(mount_scans_, &MountScanCoordinator::VolumeWithoutMountArrived, this, nullptr);
    disconnect(mount_scans_, &MountScanCoordinator::MountPointRemoved, this, nullptr);
  }
  mount_scans_ = mount_scans;
  if (mount_scans_ != nullptr) {
    connect(mount_scans_, &MountScanCoordinator::VolumeWithoutMountArrived, this,
            &FolderController::SetArrivedVolume);
    connect(mount_scans_, &MountScanCoordinator::MountPointRemoved, this,
            &FolderController::HandleMountPointRemoved);
  }
}

void FolderController::SetArrivedVolume(const QString& volumeName, const QString& mountPoint,
                                        bool hasDcimDirectory, const QString& volumeIdentity) {
  const auto root = album_util::InputToPath(mountPoint);
  if (!root.has_value()) {
    return;
  }
  arrived_mount_point_ = *root;
  arrived_identity_    = volumeIdentity.toStdString();
  const auto to_url = [](const std::filesystem::path& path) {
    return QUrl::fromLocalFile(album_util::PathToQString(path));
  };
  arrived_volume_ =
      QVariantMap{{QStringLiteral("name"), volumeName},
                  {QStringLiteral("mountPoint"), album_util::MountPointDisplay(*root)},
                  {QStringLiteral("hasDcim"), hasDcimDirectory},
                  {QStringLiteral("rootUrl"), to_url(*root)},
                  {QStringLiteral("dcimUrl"), hasDcimDirectory ? to_url(*root / L"DCIM") : QUrl{}}};
  emit ArrivedVolumeChanged();
}

void FolderController::DismissArrivedVolume() {
  if (arrived_volume_.isEmpty()) {
    return;
  }
  arrived_volume_.clear();
  arrived_mount_point_.clear();
  arrived_identity_.clear();
  emit ArrivedVolumeChanged();
}

bool FolderController::TakeArrivedVolume() {
  if (arrived_volume_.isEmpty()) {
    return false;
  }
  const auto    mount_point = arrived_mount_point_;
  const auto    identity    = arrived_identity_;
  const QString name        = arrived_volume_.value(QStringLiteral("name")).toString();
  const QString shown       = name.isEmpty()
                                  ? arrived_volume_.value(QStringLiteral("mountPoint")).toString()
                                  : name;
  DismissArrivedVolume();
  // A volume that replaced the one of the card at the same mount point has other files.
  if (mount_scans_ == nullptr || !mount_scans_->IsArrivedVolumeAt(mount_point, identity)) {
    status_->SetTaskState(PL_TEXT("%1 is no longer connected.", shown), 0, false);
    return false;
  }
  return true;
}

void FolderController::HandleMountPointRemoved(const QString& mountPoint) {
  if (arrived_volume_.isEmpty()) {
    return;
  }
  const auto removed = album_util::InputToPath(mountPoint);
  if (removed.has_value() && *removed == arrived_mount_point_) {
    DismissArrivedVolume();
  }
}

}  // namespace alcedo::ui

#undef PL_TEXT
