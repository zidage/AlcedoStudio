//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// Registers `adjustments.items`, `adjustments.copy`, and `adjustments.paste`, the Copy
/// Adjustments and Paste Adjustments of the GUI.
///
/// - `adjustments.items` lists the Versions of a photo and the transferable items of one
///   Version (AdjustmentTransferCatalogService), like the Copy Adjustments dialog.
/// - `adjustments.copy` builds an AdjustmentTransferPackage from a selection of those items
///   (AdjustmentTransferPackageBuilder) and keeps it for the session under a package id.
/// - `adjustments.paste` pastes a package onto photos as one new Version per photo with the
///   author agent. The photo open in the editor pastes through the editor session; every other
///   photo pastes in the background task of AdjustmentTransferApplyCoordinator, whose finished
///   task carries the result of each photo (AutomationTaskTracker).
///
/// The library selection exists only in the GUI host: with @p host_mode `headless`, a paste
/// without `element_ids` answers -32001. @p registry and @p host must outlive every dispatch.
/// Returns false and writes @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationAdjustmentCommands(AutomationCommandRegistry& registry,
                                                        ui::ApplicationModuleHost* host,
                                                        const QString&             host_mode,
                                                        QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
