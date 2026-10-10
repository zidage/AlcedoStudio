//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// The project summary that `project.open` and `project.create` return: `path`, `name`,
/// `entered`, and `photo_count` (the photo count read when the project loaded).
auto               ReadAutomationProjectSummary(ui::ProjectModule& project) -> QJsonObject;

/// Registers `project.create`, `project.open`, `project.save`, and `project.close`. They call
/// ProjectLaunchCoordinator, ApplicationCloseCoordinator, and ProjectModule, the owners that the
/// GUI uses. @p host must outlive every dispatch of these commands. Returns false and writes
/// @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationProjectCommands(AutomationCommandRegistry& registry,
                                                     ui::ApplicationModuleHost* host,
                                                     QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
