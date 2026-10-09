//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_task_tracker.hpp"

namespace alcedo::automation {

/// Registers `library.import`, `library.folders`, `library.list`, and `library.thumbnail`.
/// They call the owners that the GUI uses: ImportExportHandler and its folder scan for the
/// import, AlbumBrowseService (the library query) for the reads, and ThumbnailService for the
/// thumbnail. @p host and @p tracker must outlive every dispatch. Returns false and writes
/// @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationLibraryCommands(AutomationCommandRegistry& registry,
                                                     ui::ApplicationModuleHost* host,
                                                     AutomationTaskTracker*     tracker,
                                                     QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
