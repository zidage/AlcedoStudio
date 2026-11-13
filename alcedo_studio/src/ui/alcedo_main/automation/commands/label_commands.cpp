//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <utility>

#include "ui/alcedo_main/album_backend/model_download_controller.hpp"
#include "ui/alcedo_main/album_backend/semantic_generation_controller.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_label_commands.hpp"

namespace alcedo::automation {
namespace {

using ui::SemanticGenerationController;

auto StatusObject(const SemanticGenerationController& labels) -> QJsonObject {
  QJsonObject status{{"active_model_profile_id", labels.ActiveModelProfileId()},
                     {"active_model_key", labels.ActiveModelKeyQString()},
                     {"model_activation_running", labels.ModelActivationRunning()},
                     {"running", labels.Running()},
                     {"total", labels.Total()},
                     {"embedded", labels.Embedded()},
                     {"skipped", labels.Skipped()},
                     {"failed", labels.Failed()},
                     {"canceled", labels.Canceled()},
                     {"album_total", labels.AlbumTotalCount()},
                     {"album_labeled", labels.AlbumLabeledCount()},
                     {"album_unlabeled", labels.AlbumUnlabeledCount()},
                     {"status_text", labels.StatusText()}};
  if (labels.RemainingSeconds() >= 0) {
    status.insert("remaining_seconds", labels.RemainingSeconds());
  }
  return status;
}

auto StatusSchema() -> QJsonObject {
  return AutomationObjectSchema(
      QJsonObject{{"active_model_profile_id", QJsonObject{{"type", "string"}}},
                  {"active_model_key", QJsonObject{{"type", "string"}}},
                  {"model_activation_running", QJsonObject{{"type", "boolean"}}},
                  {"running", QJsonObject{{"type", "boolean"}}},
                  {"total", QJsonObject{{"type", "integer"}}},
                  {"embedded", QJsonObject{{"type", "integer"}}},
                  {"skipped", QJsonObject{{"type", "integer"}}},
                  {"failed", QJsonObject{{"type", "integer"}}},
                  {"canceled", QJsonObject{{"type", "integer"}}},
                  {"album_total", QJsonObject{{"type", "integer"}}},
                  {"album_labeled", QJsonObject{{"type", "integer"}}},
                  {"album_unlabeled", QJsonObject{{"type", "integer"}}},
                  {"status_text", QJsonObject{{"type", "string"}}},
                  {"remaining_seconds", QJsonObject{{"type", "integer"}}}},
      QJsonArray{"active_model_profile_id", "running", "status_text"});
}

}  // namespace

auto RegisterAutomationLabelCommands(AutomationCommandRegistry& registry,
                                     ui::ApplicationModuleHost* host, QString* error) -> bool {
  if (host == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("the label commands need a host");
    }
    return false;
  }

  AutomationCommandSpec status;
  status.method      = QStringLiteral("labels.status");
  status.description = QStringLiteral(
      "Returns the active AI label model of the project, the photo counts with and without "
      "labels, and the progress of a running label generation. 'remaining_seconds' is present "
      "when the running generation has a time estimate.");
  status.params_schema = AutomationClosedParamsSchema();
  status.result_schema = StatusSchema();
  status.handler       = [host](const QJsonObject&, AutomationReply reply) {
    if (!AutomationEnteredProject(host, reply)) {
      return;
    }
    SemanticGenerationController* labels = host->semantic_generation();
    labels->RefreshAlbumSummary();
    reply.SendResult(StatusObject(*labels));
  };
  if (!registry.Register(std::move(status), error)) {
    return false;
  }

  AutomationCommandSpec install;
  install.method      = QStringLiteral("labels.install_model");
  install.description = QStringLiteral(
      "Downloads the AI label model 'profile_id' into the model folder of the AI Models settings "
      "page, as its Install button does, and answers with the download's task id (use "
      "tasks.wait). The download statement of the AI Models page must be accepted. A model that "
      "is installed answers with 'task_id' null.");
  install.changes_state = true;
  install.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"profile_id", QJsonObject{{"type", "string"}, {"description", "The model profile."}}}},
      QJsonArray{"profile_id"});
  install.result_schema = AutomationObjectSchema(
      QJsonObject{{"task_id", QJsonObject{{"type", QJsonArray{"string", "null"}}}}},
      QJsonArray{"task_id"});
  install.handler = [host](const QJsonObject& params, AutomationReply reply) {
    ui::ModelDownloadController* models     = host->model_download();
    const QString                profile_id = params.value("profile_id").toString();
    if (models->FindProfile(profile_id) == nullptr) {
      SendAutomationParamError(reply, QStringLiteral("/profile_id"),
                               QStringLiteral("unknown model profile: %1").arg(profile_id));
      return;
    }
    if (models->IsModelInstalled(profile_id)) {
      reply.SendResult(QJsonObject{{"task_id", QJsonValue::Null}});
      return;
    }
    if (!models->InstallModel(profile_id)) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Rejected,
                               QStringLiteral("The download of %1 did not start; see the AI "
                                              "Models settings page for the reason.")
                                   .arg(profile_id));
      return;
    }
    reply.SendResult(QJsonObject{{"task_id", models->DownloadTaskId()}});
  };
  if (!registry.Register(std::move(install), error)) {
    return false;
  }

  AutomationCommandSpec activate;
  activate.method      = QStringLiteral("labels.activate_model");
  activate.description = QStringLiteral(
      "Activates the installed AI label model 'profile_id' for the project, as the AI Models "
      "settings page does, and answers when the activation finished. The answer is the result "
      "of labels.status.");
  activate.changes_state = true;
  activate.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"profile_id", QJsonObject{{"type", "string"},
                                             {"description",
                                              "The model profile, for example "
                                              "siglip2-b32-256-multilingual."}}}},
      QJsonArray{"profile_id"});
  activate.result_schema = StatusSchema();
  activate.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    if (!AutomationEnteredProject(host, reply)) {
      return;
    }
    SemanticGenerationController* labels     = host->semantic_generation();
    const QString                 profile_id = params.value("profile_id").toString();
    if (!host->model_download()->IsModelInstalled(profile_id)) {
      SendAutomationParamError(reply, QStringLiteral("/profile_id"),
                                     QStringLiteral("the model %1 is not installed").arg(profile_id));
      return;
    }
    if (labels->ActiveModelProfileId() == profile_id) {
      reply.SendResult(StatusObject(*labels));
      return;
    }
    if (!labels->ActivateModel(profile_id)) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Rejected,
                                     QStringLiteral("The model %1 was not activated; see the AI "
                                                          "Models settings page for the reason.")
                                         .arg(profile_id));
      return;
    }
    QPointer<AutomationCommandWait> wait = new AutomationCommandWait(std::move(reply), host);
    auto                            answer_when_finished = [wait, labels, profile_id]() {
      if (wait == nullptr || labels->ModelActivationRunning()) {
        return;
      }
      if (labels->ActiveModelProfileId() != profile_id) {
        wait->SendOwnerError(AutomationErrorCode::Failed,
                                                              QStringLiteral("The activation of %1 failed.").arg(profile_id));
        return;
      }
      wait->SendResult(StatusObject(*labels));
    };
    QObject::connect(labels, &SemanticGenerationController::StateChanged, wait.data(),
                           answer_when_finished);
    answer_when_finished();
  };
  if (!registry.Register(std::move(activate), error)) {
    return false;
  }

  AutomationCommandSpec generate;
  generate.method      = QStringLiteral("labels.generate");
  generate.description = QStringLiteral(
      "Generates AI labels for the photos of the project that have none, as the Generate "
      "button of the AI label settings does. With 'force' true every photo gets new labels. "
      "Answers with the background task id when the generation runs (use tasks.wait), or "
      "with 'task_id' null and the status text when nothing runs.");
  generate.changes_state = true;
  generate.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"force", QJsonObject{{"type", "boolean"},
                            {"description", "Generate labels for every photo. Default false."}}}});
  generate.result_schema = AutomationObjectSchema(
      QJsonObject{{"task_id", QJsonObject{{"type", QJsonArray{"string", "null"}}}},
                  {"status_text", QJsonObject{{"type", "string"}}}},
      QJsonArray{"task_id", "status_text"});
  generate.handler = [host](const QJsonObject& params, AutomationReply reply) {
    if (!AutomationEnteredProject(host, reply)) {
      return;
    }
    SemanticGenerationController* labels = host->semantic_generation();
    if (labels->Running() || labels->ModelActivationRunning()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Busy,
                               QStringLiteral("A label generation or model activation runs."));
      return;
    }
    QPointer<AutomationCommandWait> wait = new AutomationCommandWait(std::move(reply), host);
    auto                            answer_when_known = [wait, labels]() {
      if (wait == nullptr) {
        return;
      }
      if (!labels->BackgroundTaskId().isEmpty()) {
        wait->SendResult(QJsonObject{{"task_id", labels->BackgroundTaskId()},
                                                                {"status_text", labels->StatusText()}});
        return;
      }
      if (!labels->Running()) {
        wait->SendResult(
            QJsonObject{{"task_id", QJsonValue::Null}, {"status_text", labels->StatusText()}});
      }
    };
    QObject::connect(labels, &SemanticGenerationController::StateChanged, wait.data(),
                     answer_when_known);
    labels->StartAlbumGeneration(params.value("force").toBool(false));
    answer_when_known();
  };
  if (!registry.Register(std::move(generate), error)) {
    return false;
  }

  AutomationCommandSpec cancel;
  cancel.method      = QStringLiteral("labels.cancel");
  cancel.description = QStringLiteral(
      "Cancels the running label generation. Labels of the photos that finished stay. The "
      "answer is the result of labels.status.");
  cancel.changes_state = true;
  cancel.params_schema = AutomationClosedParamsSchema();
  cancel.result_schema = StatusSchema();
  cancel.handler       = [host](const QJsonObject&, AutomationReply reply) {
    if (!AutomationEnteredProject(host, reply)) {
      return;
    }
    SemanticGenerationController* labels = host->semantic_generation();
    labels->CancelGeneration();
    reply.SendResult(StatusObject(*labels));
  };
  return registry.Register(std::move(cancel), error);
}

}  // namespace alcedo::automation
