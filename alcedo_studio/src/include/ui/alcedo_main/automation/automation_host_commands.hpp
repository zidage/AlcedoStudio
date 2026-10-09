//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QString>
#include <functional>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// Owner references of the session commands that read or end the application session.
struct AutomationHostCommandContext {
  /// Must outlive every dispatch of the registered commands.
  ui::ApplicationModuleHost* host = nullptr;
  /// `headless` or `gui`.
  QString                    host_mode;
  /// Runs after `ApplicationModuleHost::Shutdown()` returned and the response was queued. The
  /// headless host removes its session file and quits the event loop.
  std::function<void()>      after_shutdown;
};

/// Reads the session state that `state.get` returns: project, workspace, editor session,
/// control, running tasks, and the idle flag.
auto ReadAutomationSessionState(ui::ApplicationModuleHost& host, const QString& host_mode)
    -> QJsonObject;

/// Registers `session.shutdown` and `state.get`. Returns false and writes @p error when a
/// registration fails.
[[nodiscard]] auto RegisterAutomationHostCommands(AutomationCommandRegistry&   registry,
                                                  AutomationHostCommandContext context,
                                                  QString* error = nullptr) -> bool;

}  // namespace alcedo::automation
