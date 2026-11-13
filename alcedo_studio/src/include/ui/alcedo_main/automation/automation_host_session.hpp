//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QDateTime>
#include <QObject>
#include <QString>
#include <functional>

#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"
#include "ui/alcedo_main/automation/automation_editor_notifier.hpp"
#include "ui/alcedo_main/automation/automation_server.hpp"
#include "ui/alcedo_main/automation/automation_task_tracker.hpp"

namespace alcedo::automation {

/// The settings key that turns the automation server of the GUI host on or off (Settings >
/// Agent control). The default is on. The value applies at the next start.
inline constexpr auto kAutomationServerEnabledSettingsKey = "automation/serverEnabled";

/// True when the GUI host starts its automation server: the saved setting, on by default.
auto IsGuiAutomationServerEnabled() -> bool;

/// The session name of a GUI host: `gui`, or `gui-<pid>` when a live process of another host
/// uses `gui` in @p session_dir.
auto GuiAutomationSessionName(const QString& session_dir) -> QString;

/// Options of one automation session.
struct AutomationHostSessionOptions {
  /// `headless` or `gui`.
  QString               host_mode;
  QString               session_name = QStringLiteral("default");
  /// The directory of the session file.
  QString               session_dir;
  /// Runs after `session.shutdown` shut the host down, removed the session file, and queued
  /// its response. The host process ends its event loop here.
  std::function<void()> after_shutdown;
};

/// One automation session of a running host: the command registry with every command group,
/// the socket server, the task and editor notifications, and the session file.
///
/// In a GUI session the state-changing commands need agent control: the session installs the
/// rules of the host's AgentControlOwner in the registry, and `control.changed` reports its
/// changes. In a headless session the agent always holds control. The session file is written
/// after the server listens and is written again when the project changes. GUI thread only.
class AutomationHostSession final : public QObject {
  Q_OBJECT

 public:
  /// @p host must outlive the session.
  AutomationHostSession(ui::ApplicationModuleHost* host, AutomationHostSessionOptions options,
                        QObject* parent = nullptr);
  ~AutomationHostSession() override;

  AutomationHostSession(const AutomationHostSession&)                    = delete;
  auto operator=(const AutomationHostSession&) -> AutomationHostSession& = delete;

  /// Registers the commands, listens, and writes the session file. Fails when the session name
  /// is invalid, a live process of another host uses the session file, a registration fails, or
  /// the server or the session file cannot start. A failed start leaves no session file.
  [[nodiscard]] auto Start(QString* error) -> bool;
  /// Closes the server and removes the session file. Runs once; the destructor calls it.
  void               Stop();

  [[nodiscard]] auto registry() -> AutomationCommandRegistry& { return registry_; }
  [[nodiscard]] auto server() -> AutomationServer& { return server_; }
  [[nodiscard]] auto session_name() const -> QString { return options_.session_name; }
  [[nodiscard]] auto socket_name() const -> QString { return socket_name_; }
  [[nodiscard]] auto session_file_path() const -> QString;

 private:
  [[nodiscard]] auto RegisterCommands(QString* error) -> bool;
  [[nodiscard]] auto WriteSessionFile(QString* error) -> bool;
  void               RemoveSessionFile();
  [[nodiscard]] auto IsGuiHost() const -> bool;

  ui::ApplicationModuleHost*   host_ = nullptr;
  AutomationHostSessionOptions options_;
  AutomationCommandRegistry    registry_;
  AutomationServer             server_;
  AutomationTaskTracker        tracker_;
  AutomationEditorNotifier     editor_notifier_;
  QString                      socket_name_;
  QDateTime                    started_at_;
  bool                         session_file_written_ = false;
  bool                         stopped_              = false;
};

}  // namespace alcedo::automation
