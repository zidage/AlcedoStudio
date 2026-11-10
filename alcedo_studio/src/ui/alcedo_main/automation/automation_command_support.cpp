//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_command_support.hpp"

#include <QFileInfo>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>

namespace alcedo::automation {

auto AutomationObjectSchema(QJsonObject properties, QJsonArray required) -> QJsonObject {
  QJsonObject schema{{"type", "object"}, {"properties", properties}};
  if (!required.isEmpty()) {
    schema.insert("required", required);
  }
  return schema;
}

auto AutomationClosedParamsSchema(QJsonObject properties, QJsonArray required) -> QJsonObject {
  QJsonObject schema = AutomationObjectSchema(std::move(properties), std::move(required));
  schema.insert("additionalProperties", false);
  return schema;
}

auto AutomationEnteredProject(ui::ApplicationModuleHost* host, AutomationReply& reply)
    -> std::shared_ptr<ProjectService> {
  ui::ProjectModule* project = host->project();
  auto               service = project->handler().project();
  if (service && project->ProjectEntered() && !project->ProjectLoading()) {
    return service;
  }
  const QString path = QString::fromStdWString(project->handler().package_path().generic_wstring());
  QString       message;
  if (project->ProjectLoading()) {
    message = QStringLiteral(
        "A project is loading; wait until state.get reports project.loading false.");
  } else if (service) {
    // The GUI can start with the last project loaded while the window stays on the start screen.
    message = QStringLiteral(
        "The project is loaded but not open in the window; call project.open with its path.");
  } else {
    message = QStringLiteral("No project is open; call project.open or project.create.");
  }
  QJsonObject data{{"reason", message}};
  if (service) {
    data.insert("path", path);
  }
  reply.SendError(AutomationErrorCode::NotReady, message, data);
  return nullptr;
}

auto AutomationElementIdSchema(int minimum, const QString& description) -> QJsonObject {
  QJsonObject schema{
      {"type", "integer"}, {"minimum", minimum}, {"maximum", kMaxAutomationElementId}};
  if (!description.isEmpty()) {
    schema.insert("description", description);
  }
  return schema;
}

auto AutomationElementIdListSchema(const QString& description) -> QJsonObject {
  QJsonObject schema{{"type", "array"}, {"items", AutomationElementIdSchema()}, {"minItems", 1}};
  if (!description.isEmpty()) {
    schema.insert("description", description);
  }
  return schema;
}

auto AutomationElementIdOf(const QJsonValue& value) -> std::optional<sl_element_id_t> {
  if (!value.isDouble()) {
    return std::nullopt;
  }
  const double number = value.toDouble();
  // The range test comes before the cast: a double outside the target range is undefined
  // behavior in a conversion to an unsigned integer. NaN fails both comparisons.
  if (!(number >= 0.0 && number <= kMaxAutomationElementId) || std::floor(number) != number) {
    return std::nullopt;
  }
  return static_cast<sl_element_id_t>(number);
}

auto AutomationElementIdParam(const QJsonObject& params, const QString& key, AutomationReply& reply)
    -> std::optional<sl_element_id_t> {
  auto id = AutomationElementIdOf(params.value(key));
  if (!id.has_value()) {
    SendAutomationParamError(reply, QLatin1Char('/') + key,
                             QStringLiteral("value is not an element id from 0 to %1")
                                 .arg(static_cast<qulonglong>(kMaxAutomationElementId)));
  }
  return id;
}

auto ResolveAutomationElementRows(ProjectService& project, const QJsonObject& params,
                                  AutomationReply& reply)
    -> std::optional<std::vector<SearchResultRow>> {
  std::vector<sl_element_id_t> ids;
  const QJsonArray             values = params.value("element_ids").toArray();
  for (qsizetype index = 0; index < values.size(); ++index) {
    const auto id = AutomationElementIdOf(values.at(index));
    if (!id.has_value()) {
      SendAutomationParamError(reply, QStringLiteral("/element_ids/%1").arg(index),
                               QStringLiteral("value is not an element id from 1 to %1")
                                   .arg(static_cast<qulonglong>(kMaxAutomationElementId)));
      return std::nullopt;
    }
    if (std::find(ids.begin(), ids.end(), *id) == ids.end()) {
      ids.push_back(*id);
    }
  }
  const auto browse = project.GetAlbumBrowseService();
  if (!browse) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed,
                             QStringLiteral("The library cannot be read."));
    return std::nullopt;
  }
  std::vector<SearchResultRow> rows;
  try {
    rows = browse->ReadAlbumFileRows(ids);
  } catch (const std::exception& e) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed, QString::fromUtf8(e.what()));
    return std::nullopt;
  }
  QJsonArray  unknown;
  QStringList unknown_text;
  for (const auto id : ids) {
    const bool found = std::any_of(rows.begin(), rows.end(), [id](const SearchResultRow& row) {
      return row.file_id_ == id && row.image_id_ != 0;
    });
    if (!found) {
      unknown.push_back(static_cast<double>(id));
      unknown_text.push_back(QString::number(id));
    }
  }
  if (!unknown.isEmpty()) {
    reply.SendError(
        AutomationErrorCode::InvalidParams,
        QStringLiteral("unknown element ids: %1").arg(unknown_text.join(QStringLiteral(", "))),
        QJsonObject{{"pointer", "/element_ids"}, {"unknown_ids", unknown}});
    return std::nullopt;
  }
  return rows;
}

void SendAutomationOwnerError(AutomationReply& reply, AutomationErrorCode code,
                              const QString& message) {
  reply.SendError(code, message, QJsonObject{{"reason", message}});
}

void SendAutomationParamError(AutomationReply& reply, const QString& pointer,
                              const QString& message) {
  reply.SendError(AutomationErrorCode::InvalidParams, message,
                  QJsonObject{{"pointer", pointer}, {"reason", message}});
}

auto AutomationExistingOutputReason(const QString& path) -> QString {
  const QFileInfo info(path);
  // exists() follows links, so a link to a missing file needs isSymLink().
  if (!info.exists() && !info.isSymLink()) {
    return {};
  }
  return QStringLiteral(
             "%1 exists. Commands write only new files and do not replace an existing "
             "file; choose another path.")
      .arg(path);
}

AutomationNewOutputFile::AutomationNewOutputFile(QString path)
    : path_(std::move(path)), file_(path_) {}

AutomationNewOutputFile::~AutomationNewOutputFile() { Discard(); }

auto AutomationNewOutputFile::Create() -> QString {
  if (file_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
    created_ = true;
    return {};
  }
  const QString existing = AutomationExistingOutputReason(path_);
  if (!existing.isEmpty()) {
    return existing;
  }
  return QStringLiteral("The file %1 cannot be created: %2").arg(path_, file_.errorString());
}

auto AutomationNewOutputFile::WritePng(const QImage& image) -> QString {
  if (!created_ || !file_.isOpen()) {
    return QStringLiteral("The file %1 was not created.").arg(path_);
  }
  if (!image.save(&file_, "PNG") || !file_.flush()) {
    return QStringLiteral("The PNG file cannot be written: %1").arg(path_);
  }
  file_.close();
  return {};
}

void AutomationNewOutputFile::Keep() { kept_ = true; }

void AutomationNewOutputFile::Discard() {
  if (created_ && !kept_) {
    file_.remove();
    created_ = false;
  }
}

AutomationCommandWait::AutomationCommandWait(AutomationReply reply, QObject* parent)
    : QObject(parent), reply_(std::move(reply)) {}

void AutomationCommandWait::SendResult(QJsonValue result) {
  if (finished_) {
    return;
  }
  reply_.SendResult(std::move(result));
  Finish();
}

void AutomationCommandWait::SendError(AutomationErrorCode code, const QString& message,
                                      QJsonValue data) {
  if (finished_) {
    return;
  }
  reply_.SendError(code, message, std::move(data));
  Finish();
}

void AutomationCommandWait::SendOwnerError(AutomationErrorCode code, const QString& message) {
  if (finished_) {
    return;
  }
  SendAutomationOwnerError(reply_, code, message);
  Finish();
}

void AutomationCommandWait::Finish() {
  finished_ = true;
  deleteLater();
}

}  // namespace alcedo::automation
