//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QString>
#include <memory>
#include <optional>
#include <vector>

#include "app/project_service.hpp"
#include "automation/automation_protocol.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_command_registry.hpp"

namespace alcedo::automation {

/// `{"type": "object", "properties": ..., "required": ...}`. Omits `required` when empty.
auto AutomationObjectSchema(QJsonObject properties = {}, QJsonArray required = {}) -> QJsonObject;

/// A parameter schema: an object schema with `"additionalProperties": false`.
auto AutomationClosedParamsSchema(QJsonObject properties = {}, QJsonArray required = {})
    -> QJsonObject;

/// The project service of the entered project, or null. Sends -32001 when no project is open,
/// a project is loading, or a project is loaded but not entered (the window shows the start
/// screen). The message names the next command; `data.path` is the loaded project file, if any.
auto AutomationEnteredProject(ui::ApplicationModuleHost* host, AutomationReply& reply)
    -> std::shared_ptr<ProjectService>;

/// The largest element id and folder id: project ids are 32-bit (sl_element_id_t).
inline constexpr double kMaxAutomationElementId = 4294967295.0;

/// Schema of one element id: an integer from @p minimum (1 for photos, 0 for folders) to
/// kMaxAutomationElementId. With @p description it has a description.
auto AutomationElementIdSchema(int minimum = 1, const QString& description = {}) -> QJsonObject;

/// Schema of a list of element ids: at least one AutomationElementIdSchema item. With
/// @p description it has a description.
auto AutomationElementIdListSchema(const QString& description = {}) -> QJsonObject;

/// The element id of @p value, or nothing when @p value is not an integer from 0 to
/// kMaxAutomationElementId. A schema check comes first, so a command answers nothing for an id it
/// cannot read and never reaches a photo through a wrapped id.
auto AutomationElementIdOf(const QJsonValue& value) -> std::optional<sl_element_id_t>;

/// The element id of @p params[@p key] (AutomationElementIdOf). Sends -32602 with the pointer
/// `/key` and returns nothing when it is not an element id.
auto AutomationElementIdParam(const QJsonObject& params, const QString& key, AutomationReply& reply)
    -> std::optional<sl_element_id_t>;

/// The library rows of @p params["element_ids"], in that order, without repeated ids. Sends
/// -32602 with `data.unknown_ids` and returns nothing when an id is not a photo of the project,
/// and -32602 with the pointer of the id when an id is not an element id.
auto ResolveAutomationElementRows(ProjectService& project, const QJsonObject& params,
                                  AutomationReply& reply)
    -> std::optional<std::vector<SearchResultRow>>;

/// Sends @p code with @p message and `data.reason` set to @p message, the form of `rejected`,
/// `failed`, and `not_ready` errors that report an owner message.
void SendAutomationOwnerError(AutomationReply& reply, AutomationErrorCode code,
                              const QString& message);

/// Sends -32602 with `data.pointer` set to @p pointer (a JSON pointer into the parameters) and
/// `data.reason` set to @p message.
void SendAutomationParamError(AutomationReply& reply, const QString& pointer,
                              const QString& message);

/// The reason why a command cannot create a new output file at @p path because a file, folder, or
/// link already has the path; empty when the path is free. Only for an early answer before a
/// render: AutomationNewOutputFile::Create is what keeps an existing file unchanged.
auto AutomationExistingOutputReason(const QString& path) -> QString;

/// A new output file of a command. Create makes the file with an exclusive open
/// (QIODevice::NewOnly), which fails when a file, folder, or link already has the path, also one
/// that appeared after an earlier check. A command therefore never changes an existing file. The
/// destructor calls Discard, so a failed command leaves no empty or partial file.
class AutomationNewOutputFile final {
 public:
  explicit AutomationNewOutputFile(QString path);
  ~AutomationNewOutputFile();
  AutomationNewOutputFile(const AutomationNewOutputFile&)                                  = delete;
  auto               operator=(const AutomationNewOutputFile&) -> AutomationNewOutputFile& = delete;

  /// Creates the file. Returns the reason of a failure.
  auto               Create() -> QString;
  /// Encodes @p image as PNG into the created file and closes it. Returns the reason of a failure.
  auto               WritePng(const QImage& image) -> QString;
  /// Keeps the file when this object ends. Call it when the command succeeded.
  void               Keep();
  /// Removes the created file unless Keep marked it. Call it before a failure answer, so the
  /// file is gone when the client reads the answer.
  void               Discard();
  [[nodiscard]] auto path() const -> const QString& { return path_; }

 private:
  QString path_;
  QFile   file_;
  bool    created_ = false;
  bool    kept_    = false;
};

/// The reply of one command that waits for owner signals. Connect the owner signals with this
/// object as the context: the first Send call answers the request and deletes this object with
/// deleteLater, which ends those connections. GUI thread only.
class AutomationCommandWait final : public QObject {
 public:
  explicit AutomationCommandWait(AutomationReply reply, QObject* parent = nullptr);

  void SendResult(QJsonValue result);
  void SendError(AutomationErrorCode code, const QString& message,
                 QJsonValue data = QJsonValue(QJsonValue::Undefined));
  void SendOwnerError(AutomationErrorCode code, const QString& message);
  /// True after a Send call or after the registry sent the timeout response of the request.
  [[nodiscard]] auto IsAnswered() const -> bool { return finished_ || reply_.IsSent(); }

 private:
  void            Finish();

  AutomationReply reply_;
  bool            finished_ = false;
};

}  // namespace alcedo::automation
