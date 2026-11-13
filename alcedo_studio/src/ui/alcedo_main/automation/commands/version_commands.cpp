//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "app/editor_history_types.hpp"
#include "app/editor_session_types.hpp"
#include "app/version_name_rules.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"
#include "ui/alcedo_main/automation/automation_editor_operation.hpp"

namespace alcedo::automation {
namespace {

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

auto HashText(const head_commit_hash_t& hash) -> QJsonValue {
  if (!hash.has_value()) {
    return QJsonValue(QJsonValue::Null);
  }
  return QString::fromStdString(hash->ToString());
}

auto VersionJson(const EditorHistoryVersion& version) -> QJsonObject {
  return QJsonObject{{"version_id", QString::fromStdString(version.version_id.ToString())},
                     {"name", QString::fromStdString(version.display_name)},
                     {"head_commit", HashText(version.head_commit_hash)},
                     {"active", version.active},
                     {"created_at", static_cast<double>(version.created_at)},
                     {"updated_at", static_cast<double>(version.updated_at)}};
}

/// The Version with the id text @p version_id in @p history, or null.
auto FindVersion(const EditorHistorySnapshot& history, const QString& version_id)
    -> const EditorHistoryVersion* {
  const std::string id = version_id.trimmed().toLower().toStdString();
  for (const auto& version : history.versions) {
    if (version.version_id.ToString() == id) {
      return &version;
    }
  }
  return nullptr;
}

/// True when @p commit_hash (hex text) is a commit of the active Version in @p history.
auto HasCommit(const EditorHistorySnapshot& history, const QString& commit_hash) -> bool {
  const std::string hash = commit_hash.trimmed().toLower().toStdString();
  for (const auto& commit : history.commits) {
    if (commit.commit_hash.ToString() == hash) {
      return true;
    }
  }
  return false;
}

/// The open editor, or null. Sends -32001 when no image is open.
auto OpenEditor(ui::ApplicationModuleHost* host, AutomationReply& reply)
    -> ui::EditorSessionController* {
  ui::EditorSessionController* editor = host->editor_session();
  if (editor == nullptr || !editor->has_image()) {
    SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                             QStringLiteral("No editor image is open."));
    return nullptr;
  }
  return editor;
}

/// The Version name of @p params.name. A missing name gives the default name of the next
/// Version. Sends -32602 and returns nothing for a name that is empty after trimming.
auto RequestedName(const ui::EditorSessionController& editor, const QJsonObject& params,
                   AutomationReply& reply) -> std::optional<std::string> {
  if (!params.contains("name")) {
    return DefaultVersionName(editor.history_snapshot().versions.size());
  }
  std::string reason;
  auto        name = NormalizeVersionName(params.value("name").toString().toStdString(), &reason);
  if (!name.has_value()) {
    SendAutomationParamError(reply, QStringLiteral("/name"), QString::fromStdString(reason));
  }
  return name;
}

/// Submits one Version command with @p submit and answers with @p answer after the result of
/// the Version change and, when the change renders, its frame.
void RunVersionCommand(ui::EditorSessionController* editor, ui::ApplicationModuleHost* host,
                       AutomationReply reply, const std::function<EditorSessionResult()>& submit,
                       std::function<QJsonObject(const EditorHistorySnapshot&)> answer) {
  QPointer<AutomationCommandWait> wait      = new AutomationCommandWait(std::move(reply), host);
  auto                            operation = OperationWait::Begin(
      editor, wait,
      [wait, editor, answer = std::move(answer)](std::optional<OperationFailure> failure) {
        if (wait == nullptr) {
          return;
        }
        if (failure.has_value()) {
          wait->SendOwnerError(failure->code_, failure->message_);
          return;
        }
        AfterQueuedChanges(wait, [wait, editor, answer] {
          if (wait != nullptr) {
            wait->SendResult(answer(editor->history_snapshot()));
          }
        });
      },
      OperationEnd::AfterSaveCheckpoint);
  const auto result = submit();
  if (EditorSessionResultIsFailure(result.kind)) {
    operation->Fail(OperationFailure{AutomationErrorCode::Rejected,
                                     result.message.empty()
                                         ? QStringLiteral("The editor cannot change Versions now.")
                                         : QString::fromStdString(result.message)});
    return;
  }
  operation->Start(result.operation_id);
}

/// The answer of a command that checks out a Version: the active Version and its head.
auto ActiveVersionAnswer(const EditorHistorySnapshot& history) -> QJsonObject {
  QJsonObject result{{"version_id", QString::fromStdString(history.active_version_id.ToString())},
                     {"head_commit", HashText(history.active_head)}};
  for (const auto& version : history.versions) {
    if (version.version_id == history.active_version_id) {
      result.insert("name", QString::fromStdString(version.display_name));
    }
  }
  return result;
}

}  // namespace

auto RegisterAutomationVersionCommands(AutomationCommandRegistry& registry,
                                       ui::ApplicationModuleHost* host, QString* error) -> bool {
  bool              ok = true;

  const QJsonObject text_schema{{"type", "string"}};
  const QJsonObject version_id_schema{{"type", "string"},
                                      {"description", "Hex id of a Version (versions.list)."}};
  const QJsonObject name_schema{
      {"type", "string"},
      {"description",
       "The Version name. Leading and trailing white space is removed; an empty "
       "name is rejected."}};
  const QJsonObject head_schema{{"type", QJsonArray{"string", "null"}},
                                {"description", "Hex hash of the head commit; null at the root."}};
  const QJsonObject version_schema =
      AutomationObjectSchema(QJsonObject{{"version_id", text_schema},
                                         {"name", text_schema},
                                         {"head_commit", head_schema},
                                         {"active", QJsonObject{{"type", "boolean"}}},
                                         {"created_at", QJsonObject{{"type", "integer"}}},
                                         {"updated_at", QJsonObject{{"type", "integer"}}}},
                             QJsonArray{"version_id", "name", "head_commit", "active"});
  const QJsonObject active_result_schema = AutomationObjectSchema(
      QJsonObject{{"version_id", text_schema}, {"name", text_schema}, {"head_commit", head_schema}},
      QJsonArray{"version_id", "head_commit"});

  AutomationCommandSpec list;
  list.method      = QStringLiteral("versions.list");
  list.description = QStringLiteral(
      "Returns the Versions of the open image and the active one, like the Versions panel. "
      "created_at and updated_at are Unix seconds.");
  list.params_schema = AutomationClosedParamsSchema();
  list.result_schema = AutomationObjectSchema(
      QJsonObject{{"active_version_id", text_schema},
                  {"versions", QJsonObject{{"type", "array"}, {"items", version_schema}}}},
      QJsonArray{"active_version_id", "versions"});
  list.handler = [host](const QJsonObject&, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const auto history = editor->history_snapshot();
    QJsonArray versions;
    for (const auto& version : history.versions) {
      versions.push_back(VersionJson(version));
    }
    reply.SendResult(QJsonObject{
        {"active_version_id", QString::fromStdString(history.active_version_id.ToString())},
        {"versions", versions}});
  };
  RegisterOrFail(registry, std::move(list), &ok, error);

  AutomationCommandSpec create;
  create.method      = QStringLiteral("versions.create");
  create.description = QStringLiteral(
      "Creates a Version at the image root and checks it out, like New Version in the Versions "
      "panel. The open edits are saved first. Without 'name' the Version gets the next default "
      "name. Answers after the frame of the new Version.");
  create.params_schema = AutomationClosedParamsSchema(QJsonObject{{"name", name_schema}});
  create.result_schema = active_result_schema;
  create.changes_state = true;
  create.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const auto name = RequestedName(*editor, params, reply);
    if (!name.has_value()) {
      return;
    }
    const QString text = QString::fromStdString(*name);
    RunVersionCommand(
        editor, host, std::move(reply),
        [editor, text] { return editor->RequestCreateRootVersion(text); }, ActiveVersionAnswer);
  };
  RegisterOrFail(registry, std::move(create), &ok, error);

  AutomationCommandSpec branch;
  branch.method      = QStringLiteral("versions.branch");
  branch.description = QStringLiteral(
      "Creates a Version that starts at a commit and checks it out, like Branch in the "
      "Versions panel. 'from_commit' is a commit hash (editor.history); without it the head of "
      "the active Version. Without 'name' the Version gets the next default name. Answers after "
      "the frame of the new Version.");
  branch.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"name", name_schema},
      {"from_commit",
       QJsonObject{{"type", "string"}, {"description", "Hex hash of the start commit."}}}});
  branch.result_schema = active_result_schema;
  branch.changes_state = true;
  branch.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    QString from_commit = params.value("from_commit").toString().trimmed();
    if (!params.contains("from_commit")) {
      const auto head = editor->history_snapshot().active_head;
      if (!head.has_value()) {
        SendAutomationParamError(
            reply, QStringLiteral("/from_commit"),
            QStringLiteral("The active Version has no commit; name a commit to branch from."));
        return;
      }
      from_commit = QString::fromStdString(head->ToString());
    } else if (from_commit.isEmpty()) {
      SendAutomationParamError(reply, QStringLiteral("/from_commit"),
                                     QStringLiteral("The commit hash is empty."));
      return;
    } else if (!HasCommit(editor->history_snapshot(), from_commit)) {
      // The editor saves before it branches, and a branch that then fails leaves the session
      // in its save recovery state. Refuse an unknown commit before that.
      SendAutomationParamError(
          reply, QStringLiteral("/from_commit"),
          QStringLiteral("unknown commit; editor.history lists the commits of the active "
                         "Version"));
      return;
    }
    const auto name = RequestedName(*editor, params, reply);
    if (!name.has_value()) {
      return;
    }
    const QString text = QString::fromStdString(*name);
    RunVersionCommand(
        editor, host, std::move(reply),
        [editor, from_commit, text] { return editor->RequestBranchFromCommit(from_commit, text); },
        ActiveVersionAnswer);
  };
  RegisterOrFail(registry, std::move(branch), &ok, error);

  AutomationCommandSpec checkout;
  checkout.method      = QStringLiteral("versions.checkout");
  checkout.description = QStringLiteral(
      "Checks out a Version, like a click on its card in the Versions panel. The open edits "
      "are saved first. Answers after the frame of the Version head.");
  checkout.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"version_id", version_id_schema}}, QJsonArray{"version_id"});
  checkout.result_schema = active_result_schema;
  checkout.changes_state = true;
  checkout.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const auto  history = editor->history_snapshot();
    const auto* version = FindVersion(history, params.value("version_id").toString());
    if (version == nullptr) {
      SendAutomationParamError(reply, QStringLiteral("/version_id"),
                                     QStringLiteral("unknown Version id; versions.list lists them"));
      return;
    }
    const QString id = QString::fromStdString(version->version_id.ToString());
    RunVersionCommand(
        editor, host, std::move(reply), [editor, id] { return editor->RequestCheckoutVersion(id); },
        ActiveVersionAnswer);
  };
  RegisterOrFail(registry, std::move(checkout), &ok, error);

  AutomationCommandSpec rename;
  rename.method      = QStringLiteral("versions.rename");
  rename.description = QStringLiteral(
      "Renames a Version, like Rename in the Versions panel. A rename to the current name "
      "changes nothing and succeeds.");
  rename.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"version_id", version_id_schema}, {"name", name_schema}},
      QJsonArray{"version_id", "name"});
  rename.result_schema =
      AutomationObjectSchema(QJsonObject{{"version_id", text_schema}, {"name", text_schema}},
                             QJsonArray{"version_id", "name"});
  rename.changes_state = true;
  rename.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const auto  history = editor->history_snapshot();
    const auto* version = FindVersion(history, params.value("version_id").toString());
    if (version == nullptr) {
      SendAutomationParamError(reply, QStringLiteral("/version_id"),
                                     QStringLiteral("unknown Version id; versions.list lists them"));
      return;
    }
    const auto name = RequestedName(*editor, params, reply);
    if (!name.has_value()) {
      return;
    }
    const QString id = QString::fromStdString(version->version_id.ToString());
    if (IsUnchangedVersionName(version->display_name, *name)) {
      reply.SendResult(QJsonObject{{"version_id", id}, {"name", QString::fromStdString(*name)}});
      return;
    }
    const QString text = QString::fromStdString(*name);
    RunVersionCommand(
        editor, host, std::move(reply),
        [editor, id, text] { return editor->RequestRenameVersion(id, text); },
        [id](const EditorHistorySnapshot& history) {
          const auto* renamed = FindVersion(history, id);
          return QJsonObject{
                    {"version_id", id},
                    {"name",
               renamed != nullptr ? QString::fromStdString(renamed->display_name) : QString()}};
        });
  };
  RegisterOrFail(registry, std::move(rename), &ok, error);

  AutomationCommandSpec remove;
  remove.method        = QStringLiteral("versions.remove");
  remove.description   = QStringLiteral("Removes a Version, like Remove in the Versions panel.");
  remove.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"version_id", version_id_schema}}, QJsonArray{"version_id"});
  remove.result_schema = AutomationObjectSchema(
      QJsonObject{{"version_id", text_schema}, {"active_version_id", text_schema}},
      QJsonArray{"version_id", "active_version_id"});
  remove.changes_state = true;
  remove.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const auto  history = editor->history_snapshot();
    const auto* version = FindVersion(history, params.value("version_id").toString());
    if (version == nullptr) {
      SendAutomationParamError(reply, QStringLiteral("/version_id"),
                                     QStringLiteral("unknown Version id; versions.list lists them"));
      return;
    }
    const QString id = QString::fromStdString(version->version_id.ToString());
    RunVersionCommand(
        editor, host, std::move(reply), [editor, id] { return editor->RequestRemoveVersion(id); },
        [id](const EditorHistorySnapshot& history) {
          return QJsonObject{
                    {"version_id", id},
                    {"active_version_id", QString::fromStdString(history.active_version_id.ToString())}};
        });
  };
  RegisterOrFail(registry, std::move(remove), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
