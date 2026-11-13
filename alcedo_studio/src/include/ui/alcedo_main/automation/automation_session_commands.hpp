//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QString>

#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// Host values that `session.describe` reports.
struct AutomationSessionDescription {
  /// `headless` or `gui`.
  QString host_mode;
  QString application_version;
};

/// Registers `session.ping` and `session.describe`. @p registry must outlive every dispatch of
/// these commands. Returns false and writes @p error when a registration fails.
[[nodiscard]] auto RegisterAutomationSessionCommands(AutomationCommandRegistry&   registry,
                                                     AutomationSessionDescription description,
                                                     QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
