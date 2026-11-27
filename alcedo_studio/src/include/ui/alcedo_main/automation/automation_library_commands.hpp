//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QString>
#include <filesystem>
#include <optional>

#include "app/album_browse_service.hpp"
#include "app/project_service.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_task_tracker.hpp"

namespace alcedo::automation {

/// Registers `library.import`, `library.folders`, `library.list`, `library.info`,
/// `library.thumbnail`, `library.thumbnails`, `library.selection.get`, `library.selection.set`,
/// `library.rate`, `library.color_label`, `library.delete_mark`, `library.color_labels`,
/// `library.color_label_name.set`, and `library.delete`. They call the owners that the GUI uses:
/// ImportExportHandler and its folder scan for the import, AlbumBrowseService (the library
/// query) for the reads, ImagePoolService for the source file and the capture metadata of
/// `library.info`, ThumbnailService for the thumbnails, LibrarySelection for the selection,
/// LibraryMutationOperations for the rating, the color labels, the Delete mark, and the delete,
/// and LibraryViewPreferences for the color label names. The selection exists only in the GUI
/// host: with @p host_mode `headless` the selection commands answer -32001. @p host and @p tracker
/// must outlive every dispatch. Returns false and writes @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationLibraryCommands(AutomationCommandRegistry& registry,
                                                     ui::ApplicationModuleHost* host,
                                                     AutomationTaskTracker*     tracker,
                                                     const QString&             host_mode,
                                                     QString* error = nullptr) -> bool;

/// Registers `library.mount`, `library.mounts`, `library.sync`, and `library.unmount` (Mounted
/// Folder Plan, MF8). They call the FolderController mount commands that the Collections panel
/// calls, and MountService::ListMounts for `library.mounts`. RegisterAutomationLibraryCommands
/// calls it. @p host must outlive every dispatch.
[[nodiscard]] auto RegisterAutomationMountCommands(AutomationCommandRegistry& registry,
                                                   ui::ApplicationModuleHost* host,
                                                   QString* error = nullptr) -> bool;

/// Registers `library.folder.create`, `library.folder.rename`, `library.folder.move`,
/// `library.folder.delete`, and `library.link` (#1190). The folder commands call the
/// FolderController folder edits that the Collections panel calls, and `library.link` calls
/// AlbumBrowseService::LinkFilesToFolder as the Add to menu does. RegisterAutomationLibraryCommands
/// calls it. @p host must outlive every dispatch.
[[nodiscard]] auto RegisterAutomationFolderCommands(AutomationCommandRegistry& registry,
                                                    ui::ApplicationModuleHost* host,
                                                    QString* error = nullptr) -> bool;

/// A folder of the open project at one of its library paths.
struct AutomationFolderLocation {
  std::filesystem::path path_;
  /// The folder id of the parent at path_; kLibraryRootFolderId for the root itself.
  sl_element_id_t       parent_id_ = kLibraryRootFolderId;
};

/// The location of the folder @p folder_id from one AlbumBrowseService::ListFolderTree read:
/// the root path for kLibraryRootFolderId, else the first path at which the tree lists the
/// folder. Nothing when no live folder has the id (a photo id, a deleted folder, an unused id).
/// @throws the error of the tree read.
[[nodiscard]] auto FindAutomationFolder(const AlbumBrowseService& browse, sl_element_id_t folder_id)
    -> std::optional<AutomationFolderLocation>;

/// The location of the folder id @p params[@p key] (FindAutomationFolder). Answers @p reply and
/// returns nothing when the id is not an element id (-32602), when no live folder has it
/// (-32002 with data.error kAutomationUnknownFolderError), or when the tree cannot be read
/// (-32007).
[[nodiscard]] auto AutomationFolderOfParam(ProjectService& project, const QJsonObject& params,
                                           const QString& key, AutomationReply& reply)
    -> std::optional<AutomationFolderLocation>;

/// The `data.error` of a folder id that names no live folder (-32002).
inline constexpr const char* kAutomationUnknownFolderError = "unknown_folder";

}  // namespace alcedo::automation
