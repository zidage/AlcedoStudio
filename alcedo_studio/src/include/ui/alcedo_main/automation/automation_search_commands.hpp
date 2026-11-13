//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// Registers `search.query`. It reads the search through the SearchSession of the
/// SearchController, the owner that the search palette uses: the same routing and request
/// match. @p host must outlive every dispatch. Returns false and writes
/// @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationSearchCommands(AutomationCommandRegistry& registry,
                                                    ui::ApplicationModuleHost* host,
                                                    QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
