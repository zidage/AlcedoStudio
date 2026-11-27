//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_task_tracker.hpp"

namespace alcedo::automation {

/// Registers `tasks.list`, `tasks.wait`, and `tasks.cancel`. `tasks.wait` waits on the owner
/// signals (ImportStateChanged, ExportStateChanged, TasksChanged), never on a sleep loop.
/// `tasks.cancel` calls the cancel of the task list (BackgroundTaskController::CancelTask), or
/// ImportExportHandler::CancelImport for the running import (#1189). @p host and
/// @p tracker must outlive every dispatch. Returns false and writes @p error when a registration
/// fails.
[[nodiscard]] auto RegisterAutomationTaskCommands(AutomationCommandRegistry& registry,
                                                  ui::ApplicationModuleHost* host,
                                                  AutomationTaskTracker*     tracker,
                                                  QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
