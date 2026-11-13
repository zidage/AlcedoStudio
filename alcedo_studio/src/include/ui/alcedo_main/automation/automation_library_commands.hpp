//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

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

}  // namespace alcedo::automation
