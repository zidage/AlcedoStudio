//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonArray>
#include <QString>

#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// The `editor.catalog` field list: one EditorParameterCatalog entry per field in UI units.
auto AutomationEditorCatalogFields() -> QJsonArray;

/// Registers `editor.catalog`. Returns false and writes @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationEditorCommands(AutomationCommandRegistry& registry,
                                                    QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
