//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/mount_service.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "ui/alcedo_main/album_backend/album_types.hpp"
#include "ui/alcedo_main/album_backend/mount_scan_coordinator.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {

class ImportExportHandler;
class LibraryModule;
class ProjectModule;
class SearchController;
class StatsEngine;
class IUiStatusSink;

/// Called on the UI thread when a mount command ended: @p error is empty on success, else the
/// exception of the operation (MountOperationError for a refusal of MountService).
using MountCommandDone = std::function<void(std::exception_ptr error)>;

/// The text of a mount command error for the status line and the automation answer.
auto                   MountCommandErrorText(std::exception_ptr error) -> QString;

/// Owns lazy folder-tree state and handles folder CRUD operations.
///
/// The sidebar tree (Sleeve Node Cache Plan, SN8): ReloadTree reads the tree with
/// AlbumBrowseService::ListFolderTree on the library query worker, so the UI thread does not
/// wait on the file system lock of the sleeve while a scan or an import writes. The rows of the
/// last applied read stay until the next read is applied on the UI thread. The element id of a
/// row and of the current folder comes from these rows, not from a path resolve.
///
/// Mounted folders (Mounted Folder Plan, MF6 and MF8): each row of `folders` carries the mount
/// fields `mountId` (0 for an ordinary row), `mountRoot`, `mountOffline`, `mountMissingCount`, and
/// `structureEditable`. The mount states come from the photo count read (RefreshImageCounts),
/// which reads MountService::ListMounts on the library query worker. The mount commands run
/// through MountScanCoordinator::RunMountOperation on the scan worker.
class FolderController final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList folders READ Folders NOTIFY FoldersChanged)
  Q_PROPERTY(uint currentFolderId READ CurrentFolderId NOTIFY FolderSelectionChanged)
  Q_PROPERTY(QString currentFolderPath READ CurrentFolderPath NOTIFY FolderSelectionChanged)
  /// True when the current folder is mounted: a file drop on the grid starts no import.
  Q_PROPERTY(bool currentFolderMounted READ CurrentFolderMounted NOTIFY MountStateChanged)
  /// With currentFolderId 0: true for the Library row (the photos directly in the library root,
  /// AlbumScope::LibraryRoot), false for All Photos (every photo of the project) (#1078).
  Q_PROPERTY(bool libraryRootSelected READ LibraryRootSelected NOTIFY FolderSelectionChanged)
  /// Photo count of All Photos: every photo of the project, mounted photos included; -1 before
  /// the first count read. The root row of `folders` carries the count of the Library row.
  Q_PROPERTY(qint64 allPhotosCount READ AllPhotosCount NOTIFY FoldersChanged)
  /// The last volume that arrived with no mount of the project (#1076), for the card of the Disk
  /// section: {name, mountPoint, hasDcim, rootUrl, dcimUrl}; empty when there is none or the
  /// user closed the card.
  Q_PROPERTY(QVariantMap arrivedVolume READ ArrivedVolume NOTIFY ArrivedVolumeChanged)

 public:
  FolderController(ProjectModule* project, LibraryModule* library, IUiStatusSink* status,
                   QObject* parent = nullptr);
  /// Waits for a running photo count read and tree read, which post their results to this object.
  ~FolderController() override;

  void BindCollaborators(StatsEngine* stats, SearchController* search,
                         ImportExportHandler* import_export);
  /// The scan coordinator of the application; the mount commands run on its worker. Its
  /// VolumeWithoutMountArrived sets arrivedVolume, and its MountPointRemoved closes the card.
  void               BindMountScans(MountScanCoordinator* mount_scans);

  /// Starts one read of the whole folder tree on the library query worker and returns. The
  /// current rows stay until the result is applied on the UI thread; a newer call replaces a
  /// read that did not apply yet. The apply selects @p preferredFolderPath, or the root when the
  /// new tree has no such folder, and then starts one photo count read (RefreshImageCounts).
  /// When the selected folder changes (a deleted or removed folder), it also clears the
  /// Inspector filters and reloads the Library.
  ///
  /// @param preferredFolderPath The folder to select after the read. Empty keeps the selection:
  ///   the path of an earlier call whose read did not apply yet, else the current folder. A
  ///   selection by the user (SelectFolder, SelectLibraryRoot) before the apply replaces it.
  void               ReloadTree(const std::filesystem::path& preferredFolderPath = {});
  /// True from a ReloadTree call until the result of its read is applied (or ClearState).
  [[nodiscard]] auto tree_read_pending() const -> bool { return tree_read_pending_; }
  /// Reads the photo count of every folder and of the whole project on the library query
  /// worker, with one statement grouped by folder id, and republishes `folders` with the
  /// counts on the UI thread. Callers that add photos to a folder or remove them call it after
  /// the change is written: import, delete, remove from an album, and "Add to".
  void               RefreshImageCounts();
  void               RebuildFolderView();
  void               ApplyFolderSelection(uint folderUiId, bool emitSignal);
  [[nodiscard]] auto CurrentFolderFsPath() const -> std::filesystem::path;
  /// The element id of the row of the current folder (kLibraryRootFolderId for the root), or
  /// nothing when no project is open or no row shows the current folder. Reads the rows only.
  [[nodiscard]] auto CurrentFolderElementId() const -> std::optional<sl_element_id_t>;
  /// The element id of the row @p folderUiId, or nothing when no project is open or no row has
  /// that id. Reads the rows only.
  [[nodiscard]] auto FolderElementIdForUiId(uint folderUiId) const
      -> std::optional<sl_element_id_t>;
  /// The loaded folder row of the collection element @p elementId, or nothing when no loaded row
  /// shows it. A tree reload gives rows new ids, so a caller that keeps a collection across
  /// reloads keeps its element id and finds the row here.
  [[nodiscard]] auto FolderUiIdForElementId(sl_element_id_t elementId) const
      -> std::optional<uint>;
  /// Library path of the folder row @p folderUiId (0 = the root collection), or nothing when no
  /// loaded row has that id.
  [[nodiscard]] auto FolderFsPathForUiId(uint folderUiId) const
      -> std::optional<std::filesystem::path> {
    return TryGetPathForUiId(folderUiId);
  }

  /// Selects the folder row @p folderUiId; 0 selects All Photos.
  Q_INVOKABLE void SelectFolder(uint folderUiId);
  /// Selects the Library row: the library root with the photos directly in it (#1078).
  Q_INVOKABLE void SelectLibraryRoot();
  /// Closes the card of the arrived volume: arrivedVolume becomes empty.
  Q_INVOKABLE void DismissArrivedVolume();
  /// Closes the card for Import or Mount of its volume (#1087). True when the volume of the card
  /// is still at its mount point (MountScanCoordinator::IsArrivedVolumeAt); else it reports that
  /// the volume is no longer connected through the status sink and returns false, and the card
  /// action stops.
  Q_INVOKABLE bool TakeArrivedVolume();
  /// Takes a volume that arrived with no mount (MountScanCoordinator::VolumeWithoutMountArrived).
  void             SetArrivedVolume(const QString& volumeName, const QString& mountPoint,
                                    bool hasDcimDirectory, const QString& volumeIdentity);
  /// Closes the card when its mount point is @p mountPoint
  /// (MountScanCoordinator::MountPointRemoved).
  void             HandleMountPointRemoved(const QString& mountPoint);
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

  /** @name Mounted folders (Mounted Folder Plan, MF6 and MF8)
   * The Q_INVOKABLE commands of the Collections panel and the image menu. Each one reports a
   * refusal or a failure through the status sink. A row id names the folder row (0 = root).
   */
  ///@{
  /// Mount the directory @p directory as a new collection in the library root, then start its
  /// first scan as a task.
  Q_INVOKABLE void MountFolder(const QUrl& directory);
  /// Scan the mount of the row now, with a task from the start.
  Q_INVOKABLE void SynchronizeMount(uint folderUiId);
  /// Remove the mount of the row; its photos stay as ordinary photos.
  Q_INVOKABLE void Unmount(uint folderUiId);
  /// Open the disk directory of a mounted row in the file manager of the system.
  Q_INVOKABLE void ShowMountedFolderInFileManager(uint folderUiId);
  /// Bind the mount of the row to the new location @p directory of its root (Locate Folder).
  Q_INVOKABLE void LocateMountFolder(uint folderUiId, const QUrl& directory);
  /// Remove the missing photos of the mount of the row from the project.
  Q_INVOKABLE void RemoveMissingPhotos(uint folderUiId);
  /// True when @p elementId is a mounted photo whose file is missing (the Locate item).
  Q_INVOKABLE bool IsMissingMountedPhoto(uint elementId) const;
  /// True when @p elementId is a mounted photo, present or missing: a present one stays in the
  /// library, and a missing one leaves it only through Remove Missing Photos. The image menu
  /// disables Delete when it is opened on such a photo (#1135).
  Q_INVOKABLE bool IsMountedPhoto(uint elementId) const;
  /// The directory where the file dialog of Locate opens for @p elementId: the last directory
  /// of the photo when it exists, else the root directory of its mount.
  Q_INVOKABLE QUrl MissingPhotoLocateFolder(uint elementId) const;
  /// Take @p file as the source of the missing mounted photo @p elementId (Locate).
  Q_INVOKABLE void LocateMissingPhoto(uint elementId, const QUrl& file);
  ///@}

  /** @name Mount commands of automation and of the Q_INVOKABLE entries above
   * Each one checks the state of the project and returns the refusal text, or starts the
   * operation and returns an empty text. @p done runs on the UI thread after the Library refresh.
   */
  ///@{
  /// @param first_scan_ended Optional; called with the result of the first scan.
  auto StartCreateMount(const std::filesystem::path&                         directory,
                        std::function<void(std::exception_ptr, mount_id_t)> done,
                        MountScanEndedCallback first_scan_ended = {}) -> QString;
  auto StartSynchronize(mount_id_t mount_id, MountScanEndedCallback ended = {}) -> QString;
  auto StartUnmount(mount_id_t mount_id, MountCommandDone done) -> QString;
  /// @param scan_ended Optional; called with the result of the scan after the relocation.
  auto StartRelocateMountRoot(mount_id_t mount_id, const std::filesystem::path& directory,
                              MountCommandDone done, MountScanEndedCallback scan_ended = {})
      -> QString;
  auto StartLocateMissingPhoto(sl_element_id_t element_id, const std::filesystem::path& file,
                               MountCommandDone done) -> QString;
  auto StartRemoveMissingPhotos(
      mount_id_t                                                               mount_id,
      std::function<void(std::exception_ptr, const MountMissingPhotoRemoval&)> done) -> QString;
  /// The mounts of the open project as the last count read found them.
  [[nodiscard]] auto MountSummaries() const -> const std::vector<MountSummary>& {
    return mount_summaries_;
  }
  ///@}

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
  [[nodiscard]] auto CurrentFolderMounted() const -> bool;
  [[nodiscard]] auto LibraryRootSelected() const -> bool { return library_root_selected_; }
  [[nodiscard]] auto AllPhotosCount() const -> qint64 {
    return image_counts_loaded_ ? static_cast<qint64>(project_image_count_) : -1;
  }
  [[nodiscard]] auto ArrivedVolume() const -> QVariantMap { return arrived_volume_; }
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
  /// The mount states of the rows or of the current folder changed.
  void MountStateChanged();
  void ArrivedVolumeChanged();

 private:
  struct FolderNodeState {
    uint32_t                  ui_id_      = 0;
    sl_element_id_t           element_id_ = 0;
    file_name_t               folder_name_{};
    std::filesystem::path     folder_path_{};
    int                       depth_    = 0;
    bool                      expanded_ = false;
    std::optional<mount_id_t> mount_id_{};
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
  /// Clears the Inspector filters and the search and reloads the Library for a new selection.
  void               ReloadSelectedScope();
  /// Takes the result of the tree read @p generation on the UI thread: replaces the nodes and the
  /// child lists with @p folders, selects the target of ReloadTree, and starts a count read. A
  /// failed read (@p error) writes one warning and keeps the root node only; the next selection
  /// by the user reads the tree again. A read that ClearState or a newer ReloadTree replaced
  /// changes nothing.
  void               ApplyTree(std::uint64_t generation, std::vector<AlbumFolderView> folders,
                               const QString& error);
  /// Element id of the node of the current folder; nothing when no node shows it.
  [[nodiscard]] auto SelectedNodeElementId() const -> std::optional<sl_element_id_t>;
  /// Clears the Inspector filters and reloads the Library after a tree read moved the selection
  /// to another folder.
  void               ReloadScopeOfMovedSelection();
  void               EnsureRootNode();
  void               ResetTreeState();
  /// Marks the folders on @p folderPath expanded.
  void               EnsurePathExpanded(const std::filesystem::path& folderPath);
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
  void ApplyImageCounts(std::uint64_t generation, FolderImageCounts counts, const QString& error);
  void                  DropImageCounts();
  /// The mount fields of the row of @p folder.
  void                  AddMountFields(const ExistingFolderEntry& folder, QVariantMap& row) const;
  /// The summary of the mount @p mount_id; null when the last count read did not list it.
  [[nodiscard]] auto    FindMountSummary(mount_id_t mount_id) const -> const MountSummary*;
  /// The mount of the row @p folderUiId; reports through the status sink when it has none.
  [[nodiscard]] auto    MountOfRow(uint folderUiId) const -> std::optional<mount_id_t>;
  /// The disk directory of a mounted row: the root directory and the path of the row below the
  /// mount root.
  [[nodiscard]] auto    MountedDirectoryOfRow(uint folderUiId) const
      -> std::optional<std::filesystem::path>;
  /// Take the mounts of a count read: rows, missing tiles, and the current folder state.
  void               ApplyMountSummaries(std::vector<MountSummary> summaries);
  /// The refusal text of a mount command, or empty when the project takes one.
  [[nodiscard]] auto MountCommandRefusal() const -> QString;
  /// Report the end of a mount command through the status sink.
  void               ReportMountCommand(std::exception_ptr error, const i18n::LocalizedText& done);

  ProjectModule*        project_       = nullptr;
  LibraryModule*        library_       = nullptr;
  IUiStatusSink*        status_        = nullptr;
  StatsEngine*          stats_         = nullptr;
  SearchController*     search_        = nullptr;
  ImportExportHandler*  import_export_ = nullptr;
  MountScanCoordinator*                                       mount_scans_   = nullptr;

  std::unordered_map<std::wstring, FolderNodeState>           nodes_by_path_{};
  std::unordered_map<std::wstring, std::vector<std::wstring>> child_keys_by_path_{};
  std::unordered_map<uint32_t, std::wstring>                  path_key_by_ui_id_{};
  /// True after an applied tree read that succeeded, until the next ResetTreeState.
  bool                                                        tree_loaded_ = false;
  /// True from ReloadTree until its read is applied.
  bool                                                        tree_read_pending_ = false;
  /// The folder that the pending tree read selects when it is applied; nothing keeps the
  /// current folder.
  std::optional<std::filesystem::path>                        reload_selection_path_{};
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
  /// The count of the root row: the photos directly in the root and in the collections.
  uint64_t                                                    library_root_image_count_ = 0;
  /// With current_folder_ui_id_ 0: the Library row rather than All Photos.
  bool                                                        library_root_selected_    = false;
  QVariantMap                                                 arrived_volume_{};
  /// The mount point and the identity of the volume of the card (#1087).
  std::filesystem::path                                       arrived_mount_point_{};
  std::string                                                 arrived_identity_{};
  std::unordered_map<sl_element_id_t, uint64_t>               image_count_by_folder_{};
  /// The mounts of the newest applied count read.
  std::vector<MountSummary>                                   mount_summaries_{};
  std::unordered_set<sl_element_id_t>                         present_mounted_ids_{};
  std::unordered_set<sl_element_id_t>                         missing_mounted_ids_{};
};

}  // namespace alcedo::ui
