//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_task_tracker.hpp"

namespace alcedo::automation {

/// Registers `export.options`, `export.run`, and `export.status`. They use the owners of the
/// export panel: ExportRecipeBuilder for the defaults, limits, checks, and the export start,
/// ImportExportHandler for the export state, and ExportQueue for the queue of the panel.
/// `export.run` answers with the `export-N` task id of @p tracker as soon as the export runs.
/// @p host and @p tracker must outlive every dispatch. Returns false and writes @p error when a
/// registration fails.
[[nodiscard]] auto RegisterAutomationExportCommands(AutomationCommandRegistry& registry,
                                                    ui::ApplicationModuleHost* host,
                                                    AutomationTaskTracker*     tracker,
                                                    QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
