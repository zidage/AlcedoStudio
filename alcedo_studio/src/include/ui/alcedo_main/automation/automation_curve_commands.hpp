//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// Registers `curve.presets`. It lists the Tone Curve presets (ToneCurvePresets) in menu order
/// with their id, name, group, and points. `editor.set` on `curve` applies one by id, for example
/// `{"points": "film_like"}` for the RGB curve. Returns false and writes @p error when the
/// registration fails.
[[nodiscard]] auto RegisterAutomationCurveCommands(AutomationCommandRegistry& registry,
                                                   QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
