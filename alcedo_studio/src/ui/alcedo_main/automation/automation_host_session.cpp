//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_host_session.hpp"

#include <QCoreApplication>
#include <QDebug>
#include <QJsonObject>
#include <QSettings>
#include <utility>

#include "automation/automation_protocol.hpp"
#include "automation/automation_session_file.hpp"
#include "ui/alcedo_main/automation/automation_adjustment_commands.hpp"
#include "ui/alcedo_main/automation/automation_control_commands.hpp"
#include "ui/alcedo_main/automation/automation_curve_commands.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"
#include "ui/alcedo_main/automation/automation_export_commands.hpp"
#include "ui/alcedo_main/automation/automation_film_commands.hpp"
#include "ui/alcedo_main/automation/automation_host_commands.hpp"
#include "ui/alcedo_main/automation/automation_label_commands.hpp"
#include "ui/alcedo_main/automation/automation_library_commands.hpp"
#include "ui/alcedo_main/automation/automation_lut_commands.hpp"
#include "ui/alcedo_main/automation/automation_project_commands.hpp"
#include "ui/alcedo_main/automation/automation_search_commands.hpp"
#include "ui/alcedo_main/automation/automation_session_commands.hpp"
#include "ui/alcedo_main/automation/automation_task_commands.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo::automation {
namespace {

constexpr auto kGuiHostMode    = "gui";
constexpr auto kGuiSessionName = "gui";

void           SetError(QString* error, QString message) {
  if (error != nullptr) {
    *error = std::move(message);
  }
}

}  // namespace

auto IsGuiAutomationServerEnabled() -> bool {
  const QSettings settings;
  return settings.value(QLatin1String(kAutomationServerEnabledSettingsKey), true).toBool();
}

auto GuiAutomationSessionName(const QString& session_dir) -> QString {
  const QString name     = QLatin1String(kGuiSessionName);
  const auto    existing = ReadAutomationSessionFile(AutomationSessionFilePath(session_dir, name));
  if (existing.has_value() && existing->pid != QCoreApplication::applicationPid() &&
      IsAutomationProcessRunning(existing->pid)) {
    return QStringLiteral("%1-%2").arg(name).arg(QCoreApplication::applicationPid());
  }
  return name;
}

AutomationHostSession::AutomationHostSession(ui::ApplicationModuleHost*   host,
                                             AutomationHostSessionOptions options, QObject* parent)
    : QObject(parent),
      host_(host),
      options_(std::move(options)),
      server_(registry_),
      tracker_(host),
      editor_notifier_(host->editor_session()) {}

AutomationHostSession::~AutomationHostSession() { Stop(); }

auto AutomationHostSession::IsGuiHost() const -> bool {
  return options_.host_mode == QLatin1String(kGuiHostMode);
}

auto AutomationHostSession::session_file_path() const -> QString {
  return AutomationSessionFilePath(options_.session_dir, options_.session_name);
}

auto AutomationHostSession::Start(QString* error) -> bool {
  if (!IsValidAutomationSessionName(options_.session_name)) {
    SetError(error, QStringLiteral("invalid session name '%1'").arg(options_.session_name));
    return false;
  }
  if (const auto existing = ReadAutomationSessionFile(session_file_path());
      existing.has_value() && existing->pid != QCoreApplication::applicationPid() &&
      IsAutomationProcessRunning(existing->pid)) {
    SetError(error, QStringLiteral("session '%1' is in use by process %2")
                        .arg(options_.session_name)
                        .arg(existing->pid));
    return false;
  }
  if (!RegisterCommands(error)) {
    return false;
  }

  connect(&tracker_, &AutomationTaskTracker::TaskUpdated, this, [this](const QJsonObject& task) {
    server_.SendNotification(QStringLiteral("task.updated"), task);
  });
  connect(&tracker_, &AutomationTaskTracker::TaskFinished, this, [this](const QJsonObject& task) {
    server_.SendNotification(QStringLiteral("task.finished"), task);
  });
  connect(&editor_notifier_, &AutomationEditorNotifier::SessionStateChanged, this,
          [this](const QJsonObject& state) {
            server_.SendNotification(QStringLiteral("session.state_changed"), state);
          });
  connect(&editor_notifier_, &AutomationEditorNotifier::HistoryChanged, this,
          [this](const QJsonObject& history) {
            server_.SendNotification(QStringLiteral("history.changed"), history);
          });
  if (IsGuiHost()) {
    ui::AgentControlOwner* control = host_->agent_control();
    connect(control, &ui::AgentControlOwner::ControlChanged, this, [this, control]() {
      server_.SendNotification(QStringLiteral("control.changed"), AutomationControlState(control));
    });
  }
  // The session file names the open project; a client reads it before it connects.
  connect(host_->project(), &ui::ProjectModule::ProjectLoadStateChanged, this, [this]() {
    if (session_file_written_ && !host_->project()->ProjectLoading()) {
      QString rewrite_error;
      if (!WriteSessionFile(&rewrite_error)) {
        qCWarning(alcedo::diag::automationLog).noquote()
            << "automation.session.file.rewrite.failed" << rewrite_error;
      }
    }
  });

  socket_name_ = QStringLiteral("alcedo-automation-%1-%2")
                     .arg(QCoreApplication::applicationPid())
                     .arg(options_.session_name);
  if (!server_.Listen(socket_name_, error)) {
    return false;
  }
  started_at_ = QDateTime::currentDateTimeUtc();
  if (!WriteSessionFile(error)) {
    server_.Close();
    return false;
  }
  session_file_written_ = true;
  return true;
}

auto AutomationHostSession::RegisterCommands(QString* error) -> bool {
  ui::AgentControlOwner* control = IsGuiHost() ? host_->agent_control() : nullptr;
  if (control != nullptr) {
    registry_.SetStateChangeControl(MakeAgentStateChangeControl(control));
  }

  AutomationHostCommandContext context;
  context.host           = host_;
  context.host_mode      = options_.host_mode;
  context.after_shutdown = [this]() {
    RemoveSessionFile();
    if (options_.after_shutdown) {
      options_.after_shutdown();
    }
  };
  return RegisterAutomationSessionCommands(
             registry_,
             AutomationSessionDescription{options_.host_mode,
                                          QCoreApplication::applicationVersion()},
             error) &&
         RegisterAutomationHostCommands(registry_, std::move(context), error) &&
         RegisterAutomationControlCommands(registry_, control, error) &&
         RegisterAutomationProjectCommands(registry_, host_, error) &&
         RegisterAutomationLibraryCommands(registry_, host_, &tracker_, options_.host_mode,
                                           error) &&
         RegisterAutomationTaskCommands(registry_, host_, &tracker_, error) &&
         RegisterAutomationExportCommands(registry_, host_, &tracker_, error) &&
         RegisterAutomationSearchCommands(registry_, host_, error) &&
         RegisterAutomationLabelCommands(registry_, host_, error) &&
         RegisterAutomationLutCommands(registry_, host_, error) &&
         RegisterAutomationFilmCommands(registry_, error) &&
         RegisterAutomationCurveCommands(registry_, error) &&
         RegisterAutomationEditorCommands(registry_, host_, error) &&
         RegisterAutomationVersionCommands(registry_, host_, error) &&
         RegisterAutomationRenderCommands(registry_, host_, error) &&
         RegisterAutomationAdjustmentCommands(registry_, host_, options_.host_mode, error);
}

auto AutomationHostSession::WriteSessionFile(QString* error) -> bool {
  AutomationSessionFile session_file;
  session_file.protocol_version = kAutomationProtocolVersion;
  session_file.session_name     = options_.session_name;
  session_file.socket_name      = socket_name_;
  session_file.pid              = QCoreApplication::applicationPid();
  session_file.host_mode        = options_.host_mode;
  session_file.project_path =
      QString::fromStdWString(host_->project()->handler().package_path().generic_wstring());
  session_file.started_at = started_at_;
  return WriteAutomationSessionFile(options_.session_dir, session_file, error);
}

void AutomationHostSession::RemoveSessionFile() {
  if (session_file_written_) {
    (void)RemoveAutomationSessionFile(options_.session_dir, options_.session_name);
    session_file_written_ = false;
  }
}

void AutomationHostSession::Stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  server_.Close();
  RemoveSessionFile();
}

}  // namespace alcedo::automation
