//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// Registers `presets.list`, `presets.save`, and `presets.apply`, the presets of the GUI (edit
/// presets plan, PS4B).
///
/// - `presets.list` lists the preset folder (PresetLibraryService inventory): each preset with
///   its availability, and with `details` the items of each available preset.
/// - `presets.save` saves a Version of a photo as a preset with the items of a save by drag
///   (SelectPresetItems: all but Crop and Rotate and Lens Correction) and answers when the file
///   is written.
/// - `presets.apply` applies a listed preset or a preset file one time onto photos as one new
///   Version per photo with the author agent, through the Paste of `adjustments.paste`
///   (StartAgentPaste).
///
/// The library selection exists only in the GUI host: with @p host_mode `headless`, an apply
/// without `element_ids` answers -32001. @p registry and @p host must outlive every dispatch.
/// Returns false and writes @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationPresetCommands(AutomationCommandRegistry& registry,
                                                    ui::ApplicationModuleHost* host,
                                                    const QString&             host_mode,
                                                    QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
