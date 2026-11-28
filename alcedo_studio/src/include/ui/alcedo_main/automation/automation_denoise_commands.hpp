//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/**
 * @brief Registers `denoise.status` and `denoise.set` (AI Denoise integration plan, DN13).
 *
 * Both work on the photo that the editor has open, through the owner of the editor switch
 * (EditorAiDenoiseControl): `denoise.set` writes the `raw_decode` change of the switch as one
 * history commit with the author agent and answers after its frame. Several photos get the
 * switch through `adjustments.copy` and `adjustments.paste` of the RAW Decode item (plan 2.2).
 * @p host must outlive every dispatch. Returns false and writes @p error when a registration
 * fails.
 */
[[nodiscard]] auto RegisterAutomationDenoiseCommands(AutomationCommandRegistry& registry,
                                                     ui::ApplicationModuleHost* host,
                                                     QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
