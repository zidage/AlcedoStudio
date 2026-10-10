//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonArray>
#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// The `editor.catalog` field list: one EditorParameterCatalog entry per field in UI units.
auto AutomationEditorCatalogFields() -> QJsonArray;

/// Registers `editor.catalog`. Returns false and writes @p error when the registration fails.
[[nodiscard]] auto RegisterAutomationEditorCatalogCommand(AutomationCommandRegistry& registry,
                                                          QString* error = nullptr) -> bool;

/// Registers `editor.catalog`, `editor.open`, `editor.close`, `editor.get`, `editor.set`,
/// `editor.batch_set`, `editor.undo`, `editor.redo`, `editor.history`, and `editor.actions`.
/// They call the owners that the GUI uses: WorkspaceRouter to open, EditorSessionController for
/// writes, Undo, Redo, and close, and the session snapshots for the reads. A write answers after
/// its commit and the frame of that commit. @p host must outlive every dispatch. Returns false
/// and writes @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationEditorCommands(AutomationCommandRegistry& registry,
                                                    ui::ApplicationModuleHost* host,
                                                    QString* error = nullptr) -> bool;

/// Registers `render.preview`: the imported root and the working values of the open image as
/// PNG files, rendered through the editor image render port. @p host must outlive every
/// dispatch. Returns false and writes @p error when the registration fails.
[[nodiscard]] auto RegisterAutomationRenderCommands(AutomationCommandRegistry& registry,
                                                    ui::ApplicationModuleHost* host,
                                                    QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
