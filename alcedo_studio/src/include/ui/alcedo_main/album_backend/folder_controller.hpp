//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class ImportExportHandler;
class LibraryModule;
class ProjectModule;
class SearchController;
class StatsEngine;
class IUiStatusSink;

/// Owns lazy folder-tree state and handles folder CRUD operations.
class FolderController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList folders READ Folders NOTIFY FoldersChanged)
  Q_PROPERTY(uint currentFolderId READ CurrentFolderId NOTIFY FolderSelectionChanged)
  Q_PROPERTY(QString currentFolderPath READ CurrentFolderPath NOTIFY FolderSelectionChanged)

 public:
  FolderController(ProjectModule* project, LibraryModule* library, IUiStatusSink* status,
                   QObject* parent = nullptr);
  /// Waits for a running photo count read, which posts its result to this object.
  ~FolderController() override;

  void BindCollaborators(StatsEngine* stats, SearchController* search,
                         ImportExportHandler* import_export);

  /// Loads the whole folder tree and starts one photo count read (RefreshImageCounts).
  void               ReloadTree(const std::filesystem::path& preferredFolderPath);
  /// Reads the photo count of every folder and of the whole project on the library query
  /// worker, with one statement grouped by folder id, and republishes `folders` with the
  /// counts on the UI thread. Callers that add photos to a folder or remove them call it after
  /// the change is written: import, delete, remove from an album, and "Add to".
  void               RefreshImageCounts();
  void               RebuildFolderView();
  void               ApplyFolderSelection(uint folderUiId, bool emitSignal);
  [[nodiscard]] auto CurrentFolderFsPath() const -> std::filesystem::path;
  [[nodiscard]] auto CurrentFolderElementId() const -> std::optional<sl_element_id_t>;
  [[nodiscard]] auto FolderElementIdForUiId(uint folderUiId) const
      -> std::optional<sl_element_id_t>;
  /// Library path of the folder row @p folderUiId (0 = the root collection), or nothing when no
  /// loaded row has that id.
  [[nodiscard]] auto FolderFsPathForUiId(uint folderUiId) const
      -> std::optional<std::filesystem::path> {
    return TryGetPathForUiId(folderUiId);
  }

  Q_INVOKABLE void SelectFolder(uint folderUiId);
  /// Creates a folder inside the current folder.
  Q_INVOKABLE void CreateFolder(const QString& folderName);
  /// Creates a folder inside @p parentFolderUiId (0 = top level). Returns the element id of
  /// the new folder, or 0 when the folder is not created. The selection does not change.
  Q_INVOKABLE uint CreateFolderIn(uint parentFolderUiId, const QString& folderName);
  /// Renames @p folderUiId. The folder keeps its element id; ui ids are reassigned because the
  /// tree reloads. Returns false and reports the reason through the status sink on refusal.
  Q_INVOKABLE bool RenameFolder(uint folderUiId, const QString& newName);
  /// Moves @p folderUiId into @p newParentFolderUiId (0 = top level). The folder keeps its
  /// element id, children, and images. A move into its own subtree is refused.
  Q_INVOKABLE bool MoveFolder(uint folderUiId, uint newParentFolderUiId);
  Q_INVOKABLE void DeleteFolder(uint folderUiId);

  [[nodiscard]] auto Folders() const -> QVariantList { return folders_; }
  [[nodiscard]] auto folders() const -> const QVariantList& { return folders_; }
  [[nodiscard]] auto CurrentFolderId() const -> uint {
    return static_cast<uint>(current_folder_ui_id_);
  }
  [[nodiscard]] auto current_folder_id() const -> uint32_t { return current_folder_ui_id_; }
  [[nodiscard]] auto current_folder_path() const -> const std::filesystem::path& {
    return current_folder_path_;
  }
  [[nodiscard]] auto CurrentFolderPath() const -> QString { return current_folder_path_text_; }
  [[nodiscard]] auto current_folder_path_text() const -> const QString& {
    return current_folder_path_text_;
  }

  [[nodiscard]] auto folder_entries() const -> const std::vector<ExistingFolderEntry>& {
    return folder_entries_;
  }

  void ClearState();

 signals:
  void FoldersChanged();
  void FolderSelectionChanged();
  void folderSelectionChanged();

 private:
  struct FolderNodeState {
    uint32_t              ui_id_      = 0;
    sl_element_id_t       element_id_ = 0;
    file_name_t           folder_name_{};
    std::filesystem::path folder_path_{};
    int                   depth_    = 0;
    bool                  expanded_ = false;
  };

  enum class FolderEdit { Create, Rename, Move, Delete };

  [[nodiscard]] auto PathKey(const std::filesystem::path& path) const -> std::wstring;
  /// Reports the reason through the status sink and returns false while a project loads, no
  /// project is open, or an import or export runs.
  [[nodiscard]] bool CheckFolderEditAllowed(FolderEdit edit) const;
  /// Trimmed @p name, or nothing (with a status message) when it is empty or holds a separator.
  [[nodiscard]] auto ValidatedFolderName(const QString& name) const -> std::optional<QString>;
  /// Saves and packages the project; false when either step fails.
  [[nodiscard]] bool SaveProjectState() const;
  void               ReportFolderEditDone(const i18n::LocalizedText& message, bool save_ok) const;
  /// Loads the children of @p folderPath and of every folder below it.
  void               LoadSubtree(const std::filesystem::path& folderPath);
  void               EnsureRootNode();
  void               ResetTreeState();
  void               EnsurePathExpanded(const std::filesystem::path& folderPath);
  void               LoadChildren(const std::filesystem::path& parentPath);
  void               AppendVisibleEntries(const std::filesystem::path&      folderPath,
                                          std::vector<ExistingFolderEntry>& out) const;
  [[nodiscard]] auto TryGetPathForUiId(uint folderUiId) const
      -> std::optional<std::filesystem::path>;
  [[nodiscard]] auto EnsureNode(const std::filesystem::path& folderPath,
                                const file_name_t& folderName, int depth) -> FolderNodeState&;
  /// Photo count of the row of @p folder; -1 before the first count read of the project.
  [[nodiscard]] auto    RowImageCount(const ExistingFolderEntry& folder) const -> qint64;
  /// Takes the result of the count read @p generation unless a newer read replaced it. On
  /// @p error the rows show no count.
  void                  ApplyImageCounts(std::uint64_t generation, uint64_t project_count,
                                         std::unordered_map<sl_element_id_t, uint64_t> folder_counts,
                                         const QString&                                error);
  void                  DropImageCounts();

  ProjectModule*        project_       = nullptr;
  LibraryModule*        library_       = nullptr;
  IUiStatusSink*        status_        = nullptr;
  StatsEngine*          stats_         = nullptr;
  SearchController*     search_        = nullptr;
  ImportExportHandler*  import_export_ = nullptr;

  std::unordered_map<std::wstring, FolderNodeState>           nodes_by_path_{};
  std::unordered_map<std::wstring, std::vector<std::wstring>> child_keys_by_path_{};
  std::unordered_map<uint32_t, std::wstring>                  path_key_by_ui_id_{};
  std::unordered_set<std::wstring>                            loaded_paths_{};
  std::vector<ExistingFolderEntry>                            folder_entries_{};
  QVariantList                                                folders_{};
  std::filesystem::path                                       current_folder_path_{};
  QString                                                     current_folder_path_text_{};
  uint32_t                                                    current_folder_ui_id_ = 0;
  uint32_t                                                    next_folder_ui_id_    = 1;
  /// Result of the newest applied count read. A tree reload keeps it until the next read
  /// replaces it; closing the project drops it.
  bool                                                        image_counts_loaded_  = false;
  uint64_t                                                    project_image_count_  = 0;
  std::unordered_map<sl_element_id_t, uint64_t>               image_count_by_folder_{};
};

}  // namespace alcedo::ui
