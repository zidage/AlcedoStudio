//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// Registers `labels.status`, `labels.install_model`, `labels.activate_model`, `labels.generate`,
/// and `labels.cancel`. The commands go through the owners that the AI label dialog and the AI
/// Models settings page use: the SemanticGenerationController and the ModelDownloadController of
/// @p host. @p host must outlive every dispatch.
/// Returns false and writes @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationLabelCommands(AutomationCommandRegistry& registry,
                                                   ui::ApplicationModuleHost* host,
                                                   QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
