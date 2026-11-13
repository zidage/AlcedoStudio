//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QVariantMap>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>

#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_project_commands.hpp"

namespace alcedo::automation {
namespace {

auto PathText(const std::filesystem::path& path) -> QString {
  return QString::fromStdWString(path.generic_wstring());
}

auto ProjectSummarySchema() -> QJsonObject {
  return AutomationObjectSchema(QJsonObject{{"path", QJsonObject{{"type", "string"}}},
                                            {"name", QJsonObject{{"type", "string"}}},
                                            {"entered", QJsonObject{{"type", "boolean"}}},
                                            {"photo_count", QJsonObject{{"type", "integer"}}}},
                                QJsonArray{"path", "name", "entered", "photo_count"});
}

/// Runs @p next after the loaded project closed through ApplicationCloseCoordinator, or at once
/// when no project is loaded. Answers @p wait when the close cannot start or fails.
void CloseLoadedProjectThen(ui::ApplicationModuleHost* host, AutomationCommandWait* wait,
                            bool persist, std::function<void()> next) {
  ui::ProjectModule* project = host->project();
  if (!project->handler().project()) {
    next();
    return;
  }
  ui::ApplicationCloseCoordinator* close = host->application_close();
  QObject::connect(close, &ui::ApplicationCloseCoordinator::ProjectCloseFinished, wait,
                   [close, wait, next = std::move(next)](bool closed, const QString& message) {
                     QObject::disconnect(close, nullptr, wait, nullptr);
                     if (!closed) {
                       wait->SendOwnerError(AutomationErrorCode::Failed, message);
                       return;
                     }
                     next();
                   });
  if (const QString reason = close->BeginProjectClose(persist); !reason.isEmpty()) {
    wait->SendOwnerError(AutomationErrorCode::Rejected, reason);
  }
}

/// Starts one launch through ProjectLaunchCoordinator and answers @p wait with the project
/// summary when the load ends with the project entered.
void LaunchAndAnswer(ui::ApplicationModuleHost* host, AutomationCommandWait* wait,
                     const std::function<bool(ui::ProjectLaunchCoordinator&)>& begin) {
  ui::ProjectLaunchCoordinator* launch          = host->project_launch();
  ui::ProjectModule*            project         = host->project();
  // ProjectChanged is emitted only when a load installs the new project.
  auto                          project_changed = std::make_shared<bool>(false);
  QObject::connect(project, &ui::ProjectModule::ProjectChanged, wait,
                   [project_changed]() { *project_changed = true; });
  QObject::connect(
      launch, &ui::ProjectLaunchCoordinator::LaunchRequestFinished, wait,
      [launch, project, wait, project_changed](bool started) {
        QObject::disconnect(launch, nullptr, wait, nullptr);
        if (!started) {
          wait->SendOwnerError(AutomationErrorCode::Rejected, project->ServiceMessage());
          return;
        }
        QObject::connect(
            project, &ui::ProjectModule::ProjectLoadStateChanged, wait,
            [project, wait, project_changed]() {
              if (project->ProjectLoading()) {
                return;
              }
              if (*project_changed && project->ServiceReady() && project->ProjectEntered()) {
                wait->SendResult(ReadAutomationProjectSummary(*project));
                return;
              }
              wait->SendOwnerError(AutomationErrorCode::Failed, project->ServiceMessage());
            });
      });
  if (!begin(*launch)) {
    wait->SendOwnerError(
        AutomationErrorCode::Busy,
        QStringLiteral("Another project launch is queued, or the accelerator preparation runs."));
  }
}

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

}  // namespace

auto ReadAutomationProjectSummary(ui::ProjectModule& project) -> QJsonObject {
  const auto&       handler      = project.handler();
  const QString     package_path = PathText(handler.package_path());
  const QVariantMap overview     = project.ProjectOverview();
  return QJsonObject{{"path", package_path},
                     {"name", QFileInfo(package_path).completeBaseName()},
                     {"entered", project.ProjectEntered()},
                     {"photo_count", overview.value(QStringLiteral("photoCount")).toDouble()}};
}

auto RegisterAutomationProjectCommands(AutomationCommandRegistry& registry,
                                       ui::ApplicationModuleHost* host, QString* error) -> bool {
  if (host == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("the project commands need an application module host");
    }
    return false;
  }
  bool                  ok = true;

  AutomationCommandSpec create;
  create.method      = QStringLiteral("project.create");
  create.description = QStringLiteral(
      "Creates a packed project named 'name' in 'folder' and enters it. A loaded project closes "
      "first and is saved. The response arrives when the new project is loaded.");
  create.changes_state = true;
  create.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"folder", QJsonObject{{"type", "string"}, {"description", "An existing folder."}}},
          {"name", QJsonObject{{"type", "string"}, {"description", "The project name."}}}},
      QJsonArray{"folder", "name"});
  create.result_schema = ProjectSummarySchema();
  create.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    const QString folder = params.value("folder").toString();
    const QString name   = params.value("name").toString().trimmed();
    if (!QFileInfo(folder).isDir()) {
      SendAutomationParamError(reply, QStringLiteral("/folder"),
                                     QStringLiteral("the folder does not exist: %1").arg(folder));
      return;
    }
    if (name.isEmpty()) {
      SendAutomationParamError(reply, QStringLiteral("/name"),
                                     QStringLiteral("the project name is empty"));
      return;
    }
    auto* wait = new AutomationCommandWait(std::move(reply), host);
    CloseLoadedProjectThen(host, wait, /*persist=*/true, [host, wait, folder, name]() {
      LaunchAndAnswer(host, wait, [folder, name](ui::ProjectLaunchCoordinator& launch) {
        return launch.BeginCreate(folder, name);
      });
    });
  };
  RegisterOrFail(registry, std::move(create), &ok, error);

  AutomationCommandSpec open;
  open.method      = QStringLiteral("project.open");
  open.description = QStringLiteral(
      "Opens the packed project at 'path' and enters it. A loaded project closes first and is "
      "saved. The response arrives when the project is loaded.");
  open.changes_state = true;
  open.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"path", QJsonObject{{"type", "string"}, {"description", "The .alcd project file."}}}},
      QJsonArray{"path"});
  open.result_schema = ProjectSummarySchema();
  open.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    const QString path = params.value("path").toString();
    // Checked before the loaded project closes, so a wrong path keeps that project open.
    if (!QFileInfo(path).isFile()) {
      SendAutomationParamError(reply, QStringLiteral("/path"),
                                     QStringLiteral("the project file does not exist: %1").arg(path));
      return;
    }
    auto* wait = new AutomationCommandWait(std::move(reply), host);
    CloseLoadedProjectThen(host, wait, /*persist=*/true, [host, wait, path]() {
      LaunchAndAnswer(host, wait, [path](ui::ProjectLaunchCoordinator& launch) {
        return launch.BeginOpen(path);
      });
    });
  };
  RegisterOrFail(registry, std::move(open), &ok, error);

  AutomationCommandSpec save;
  save.method      = QStringLiteral("project.save");
  save.description = QStringLiteral(
      "Saves and packs the loaded project. Like File > Save Project, it first closes the editor "
      "image with its changes.");
  save.changes_state = true;
  save.params_schema = AutomationClosedParamsSchema();
  save.result_schema = AutomationObjectSchema(
      QJsonObject{{"path", QJsonObject{{"type", "string"}}}}, QJsonArray{"path"});
  save.handler = [host](const QJsonObject&, AutomationReply reply) {
    ui::ProjectModule* project = host->project();
    if (!project->handler().project() || project->ProjectLoading()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                               QStringLiteral("No project is loaded."));
      return;
    }
    if (!project->SaveProject()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Failed, project->ServiceMessage());
      return;
    }
    reply.SendResult(QJsonObject{{"path", PathText(project->handler().package_path())}});
  };
  RegisterOrFail(registry, std::move(save), &ok, error);

  AutomationCommandSpec close;
  close.method      = QStringLiteral("project.close");
  close.description = QStringLiteral(
      "Closes the editor image and the loaded project. With persist (the default), both are "
      "saved first; without it, the changes since the last save are discarded. The response "
      "arrives when no project is loaded.");
  close.changes_state = true;
  close.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"persist", QJsonObject{{"type", "boolean"},
                              {"default", true},
                              {"description", "Save the editor image and the project first."}}}});
  close.result_schema = AutomationObjectSchema(
      QJsonObject{{"closed", QJsonObject{{"type", "boolean"}}}}, QJsonArray{"closed"});
  close.handler = [host](const QJsonObject& params, AutomationReply reply) {
    ui::ProjectModule* project = host->project();
    if (!project->handler().project()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                               QStringLiteral("No project is loaded."));
      return;
    }
    const bool persist = params.value("persist").toBool(true);
    auto*      wait    = new AutomationCommandWait(std::move(reply), host);
    CloseLoadedProjectThen(host, wait, persist,
                           [wait]() { wait->SendResult(QJsonObject{{"closed", true}}); });
  };
  RegisterOrFail(registry, std::move(close), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
