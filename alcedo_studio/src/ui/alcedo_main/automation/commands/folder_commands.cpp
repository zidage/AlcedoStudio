//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// library.folder.create, library.folder.rename, library.folder.move, library.folder.delete, and
// library.link (#1190). The folder commands call the FolderController folder edits that the
// Collections panel calls; library.link calls the link of the Add to menu
// (ImageController::LinkImagesToFolder).

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <exception>
#include <filesystem>
#include <map>
#include <optional>
#include <utility>

#include "app/album_browse_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_error.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/image_controller.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_library_commands.hpp"

namespace alcedo::automation {

auto FindAutomationFolder(const AlbumBrowseService& browse, sl_element_id_t folder_id)
    -> std::optional<AutomationFolderLocation> {
  if (folder_id == kLibraryRootFolderId) {
    return AutomationFolderLocation{.path_      = ui::album_util::RootFsPath(),
                                    .parent_id_ = kLibraryRootFolderId};
  }
  // The id of each listed path; the tree lists a parent before its children.
  std::map<std::filesystem::path, sl_element_id_t> listed{
      {ui::album_util::RootFsPath(), kLibraryRootFolderId}};
  for (const auto& folder : browse.ListFolderTree()) {
    const auto parent = listed.find(folder.folder_path_.parent_path());
    if (parent == listed.end()) {
      continue;
    }
    if (folder.folder_id_ == folder_id) {
      return AutomationFolderLocation{.path_ = folder.folder_path_, .parent_id_ = parent->second};
    }
    listed.emplace(folder.folder_path_, folder.folder_id_);
  }
  return std::nullopt;
}

namespace {

/// The `data.error` of a rename, move, delete, or link of the library root.
constexpr const char* kRootFolderError = "root_folder_cannot_change";

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

/// Sends -32002 (rejected) with @p message and `data` {reason, error: @p error}.
void SendNamedRefusal(AutomationReply& reply, const QString& error, const QString& message) {
  reply.SendError(AutomationErrorCode::Rejected, message,
                  QJsonObject{{"reason", message}, {"error", error}});
}

void SendUnknownFolder(AutomationReply& reply, const QString& pointer, sl_element_id_t id) {
  SendNamedRefusal(reply, QString::fromLatin1(kAutomationUnknownFolderError),
                   QStringLiteral("No folder of the project has the id %1 (%2).")
                       .arg(static_cast<qulonglong>(id))
                       .arg(pointer));
}

/// The `data.error` name of @p refusal; empty for a refusal that is not -32002.
auto FolderRefusalName(ui::FolderEditRefusal refusal) -> QString {
  switch (refusal) {
    case ui::FolderEditRefusal::kInvalidName:
      return QStringLiteral("invalid_folder_name");
    case ui::FolderEditRefusal::kNameTaken:
      return QStringLiteral("folder_name_taken");
    case ui::FolderEditRefusal::kUnknownFolder:
      return QString::fromLatin1(kAutomationUnknownFolderError);
    case ui::FolderEditRefusal::kRootFolder:
      return QString::fromLatin1(kRootFolderError);
    case ui::FolderEditRefusal::kMoveIntoOwnSubtree:
      return QStringLiteral("move_into_own_subtree");
    case ui::FolderEditRefusal::kMountedFolder:
      return QString::fromLatin1(SleeveErrorName(SleeveError::kMountedFolderStructureIsReadOnly));
    case ui::FolderEditRefusal::kNone:
    case ui::FolderEditRefusal::kProjectUnavailable:
    case ui::FolderEditRefusal::kTaskRunning:
    case ui::FolderEditRefusal::kFailed:
      break;
  }
  return {};
}

/// Answers @p reply with the refusal of @p outcome: -32001 (not ready) without a ready project,
/// -32005 (busy) while an import or export runs, -32007 (failed) for a write failure, and
/// -32002 (rejected) with `data.error` for the other reasons.
void SendFolderRefusal(AutomationReply& reply, const ui::FolderEditOutcome& outcome) {
  switch (outcome.refusal_) {
    case ui::FolderEditRefusal::kProjectUnavailable:
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady, outcome.message_);
      return;
    case ui::FolderEditRefusal::kTaskRunning:
      SendAutomationOwnerError(reply, AutomationErrorCode::Busy, outcome.message_);
      return;
    case ui::FolderEditRefusal::kFailed:
    case ui::FolderEditRefusal::kNone:
      SendAutomationOwnerError(reply, AutomationErrorCode::Failed, outcome.message_);
      return;
    case ui::FolderEditRefusal::kInvalidName:
    case ui::FolderEditRefusal::kNameTaken:
    case ui::FolderEditRefusal::kUnknownFolder:
    case ui::FolderEditRefusal::kRootFolder:
    case ui::FolderEditRefusal::kMoveIntoOwnSubtree:
    case ui::FolderEditRefusal::kMountedFolder:
      SendNamedRefusal(reply, FolderRefusalName(outcome.refusal_), outcome.message_);
      return;
  }
}

/// The library.folders item of the folder @p folder after an edit, below @p parent_id.
auto FolderJson(const AlbumFolderView& folder, sl_element_id_t parent_id) -> QJsonObject {
  return QJsonObject{{"folder_id", static_cast<double>(folder.folder_id_)},
                     {"parent_id", static_cast<double>(parent_id)},
                     {"name", QString::fromStdWString(folder.folder_name_)},
                     {"path", QString::fromStdWString(folder.folder_path_.generic_wstring())}};
}

auto FolderResultSchema() -> QJsonObject {
  return AutomationObjectSchema(QJsonObject{{"folder_id", QJsonObject{{"type", "integer"}}},
                                            {"parent_id", QJsonObject{{"type", "integer"}}},
                                            {"name", QJsonObject{{"type", "string"}}},
                                            {"path", QJsonObject{{"type", "string"}}}},
                                QJsonArray{"folder_id", "parent_id", "name", "path"});
}

auto FolderIdSchema(const char* description) -> QJsonObject {
  return AutomationElementIdSchema(0, QString::fromLatin1(description));
}

}  // namespace

auto AutomationFolderOfParam(ProjectService& project, const QJsonObject& params, const QString& key,
                             AutomationReply& reply) -> std::optional<AutomationFolderLocation> {
  const auto id = AutomationElementIdParam(params, key, reply);
  if (!id.has_value()) {
    return std::nullopt;
  }
  const auto browse = project.GetAlbumBrowseService();
  if (!browse) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed,
                             QStringLiteral("The folder tree cannot be read."));
    return std::nullopt;
  }
  std::optional<AutomationFolderLocation> location;
  try {
    location = FindAutomationFolder(*browse, *id);
  } catch (const std::exception& e) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed, QString::fromUtf8(e.what()));
    return std::nullopt;
  }
  if (!location.has_value()) {
    SendUnknownFolder(reply, QStringLiteral("/") + key, *id);
  }
  return location;
}

namespace {

/// The refusals that the folder edit commands answer, for the descriptions.
constexpr const char* kFolderRefusalText =
    " A refusal answers -32002 (rejected) with data.error: 'invalid_folder_name' (empty after "
    "the trim, '.', '..', or a name with '/', '\\', or a control character), "
    "'folder_name_taken' (a sibling folder has the name), 'unknown_folder' (no folder has the "
    "id), 'root_folder_cannot_change', 'move_into_own_subtree', or "
    "'mounted_folder_structure_is_read_only'. An import or export that runs answers -32005 "
    "(busy).";

}  // namespace

auto RegisterAutomationFolderCommands(AutomationCommandRegistry& registry,
                                      ui::ApplicationModuleHost* host, QString* error) -> bool {
  bool                  ok = true;

  AutomationCommandSpec create;
  create.method      = QStringLiteral("library.folder.create");
  create.description = QStringLiteral(
                           "Creates a folder named 'name' in the folder 'parent_id' (0 is the "
                           "root) and returns it as library.folders lists it. The name is "
                           "trimmed; the result holds the stored name.") +
                       QString::fromLatin1(kFolderRefusalText);
  create.changes_state = true;
  create.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"parent_id", FolderIdSchema("A folder id from library.folders.")},
                  {"name", QJsonObject{{"type", "string"}}}},
      QJsonArray{"parent_id", "name"});
  create.result_schema = FolderResultSchema();
  create.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto parent =
        AutomationFolderOfParam(*project, params, QStringLiteral("parent_id"), reply);
    if (!parent.has_value()) {
      return;
    }
    const auto outcome =
        host->folders()->CreateFolderAt(parent->path_, params.value("name").toString());
    if (!outcome.Done()) {
      SendFolderRefusal(reply, outcome);
      return;
    }
    reply.SendResult(
        FolderJson(outcome.folder_, *AutomationElementIdOf(params.value("parent_id"))));
  };
  RegisterOrFail(registry, std::move(create), &ok, error);

  AutomationCommandSpec rename;
  rename.method      = QStringLiteral("library.folder.rename");
  rename.description = QStringLiteral(
                           "Renames the folder 'folder_id' to 'name' and returns it. The folder "
                           "keeps its id, its child folders, and its photos. The name is "
                           "trimmed; a rename to the current name changes nothing.") +
                       QString::fromLatin1(kFolderRefusalText);
  rename.changes_state = true;
  rename.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"folder_id", FolderIdSchema("A folder id from library.folders.")},
                  {"name", QJsonObject{{"type", "string"}}}},
      QJsonArray{"folder_id", "name"});
  rename.result_schema = FolderResultSchema();
  rename.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto folder =
        AutomationFolderOfParam(*project, params, QStringLiteral("folder_id"), reply);
    if (!folder.has_value()) {
      return;
    }
    const auto outcome =
        host->folders()->RenameFolderAt(folder->path_, params.value("name").toString());
    if (!outcome.Done()) {
      SendFolderRefusal(reply, outcome);
      return;
    }
    reply.SendResult(FolderJson(outcome.folder_, folder->parent_id_));
  };
  RegisterOrFail(registry, std::move(rename), &ok, error);

  AutomationCommandSpec move;
  move.method      = QStringLiteral("library.folder.move");
  move.description = QStringLiteral(
                         "Moves the folder 'folder_id' into the folder 'parent_id' (0 is the "
                         "root) and returns it. The folder keeps its id, its name, its child "
                         "folders, and its photos. A move to the current parent changes "
                         "nothing.") +
                     QString::fromLatin1(kFolderRefusalText);
  move.changes_state = true;
  move.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"folder_id", FolderIdSchema("A folder id from library.folders.")},
                  {"parent_id", FolderIdSchema("The new parent folder.")}},
      QJsonArray{"folder_id", "parent_id"});
  move.result_schema = FolderResultSchema();
  move.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto folder =
        AutomationFolderOfParam(*project, params, QStringLiteral("folder_id"), reply);
    if (!folder.has_value()) {
      return;
    }
    const auto parent =
        AutomationFolderOfParam(*project, params, QStringLiteral("parent_id"), reply);
    if (!parent.has_value()) {
      return;
    }
    const auto outcome = host->folders()->MoveFolderAt(folder->path_, parent->path_);
    if (!outcome.Done()) {
      SendFolderRefusal(reply, outcome);
      return;
    }
    reply.SendResult(
        FolderJson(outcome.folder_, *AutomationElementIdOf(params.value("parent_id"))));
  };
  RegisterOrFail(registry, std::move(move), &ok, error);

  AutomationCommandSpec remove;
  remove.method      = QStringLiteral("library.folder.delete");
  remove.description = QStringLiteral(
                           "Deletes the folder 'folder_id' and its child folders. The photos "
                           "stay in the project.") +
                       QString::fromLatin1(kFolderRefusalText);
  remove.changes_state = true;
  remove.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"folder_id", FolderIdSchema("A folder id from library.folders.")}},
      QJsonArray{"folder_id"});
  remove.result_schema =
      AutomationObjectSchema(QJsonObject{{"folder_id", QJsonObject{{"type", "integer"}}},
                                         {"deleted", QJsonObject{{"type", "boolean"}}}},
                             QJsonArray{"folder_id", "deleted"});
  remove.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto folder =
        AutomationFolderOfParam(*project, params, QStringLiteral("folder_id"), reply);
    if (!folder.has_value()) {
      return;
    }
    const auto outcome = host->folders()->DeleteFolderAt(folder->path_);
    if (!outcome.Done()) {
      SendFolderRefusal(reply, outcome);
      return;
    }
    reply.SendResult(QJsonObject{{"folder_id", params.value("folder_id")}, {"deleted", true}});
  };
  RegisterOrFail(registry, std::move(remove), &ok, error);

  AutomationCommandSpec link;
  link.method      = QStringLiteral("library.link");
  link.description = QStringLiteral(
      "Adds the photos 'element_ids' to the folder 'folder_id', as the Add to menu does. A "
      "photo can be in several folders; it stays in the others. 'linked_ids' lists the added "
      "photos, 'already_linked_ids' the photos that the folder already held, and 'failed_ids' "
      "the photos that the link could not add. A refusal answers -32002 (rejected) with "
      "data.error 'unknown_folder', 'root_folder_cannot_change' (every photo is in the root), or "
      "'mounted_folder_structure_is_read_only'. An unknown photo id answers -32602 with "
      "data.unknown_ids.");
  link.changes_state = true;
  link.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"element_ids", AutomationElementIdListSchema()},
                  {"folder_id", FolderIdSchema("A folder id from library.folders.")}},
      QJsonArray{"element_ids", "folder_id"});
  link.result_schema =
      AutomationObjectSchema(QJsonObject{{"linked_ids", QJsonObject{{"type", "array"}}},
                                         {"already_linked_ids", QJsonObject{{"type", "array"}}},
                                         {"failed_ids", QJsonObject{{"type", "array"}}}},
                             QJsonArray{"linked_ids", "already_linked_ids", "failed_ids"});
  link.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto rows = ResolveAutomationElementRows(*project, params, reply);
    if (!rows.has_value()) {
      return;
    }
    const auto folder =
        AutomationFolderOfParam(*project, params, QStringLiteral("folder_id"), reply);
    if (!folder.has_value()) {
      return;
    }
    const sl_element_id_t folder_id = *AutomationElementIdOf(params.value("folder_id"));
    if (folder_id == kLibraryRootFolderId) {
      SendNamedRefusal(reply, QString::fromLatin1(kRootFolderError),
                       QStringLiteral("Every photo of the project is in the root folder."));
      return;
    }
    QVariantList targets;
    for (const auto& row : *rows) {
      targets.push_back(
          QVariantMap{{QStringLiteral("elementId"), static_cast<uint>(row.file_id_)},
                      {QStringLiteral("imageId"), static_cast<uint>(row.image_id_)},
                      {QStringLiteral("fileName"), QString::fromStdString(row.file_name_)}});
    }
    const QVariantMap result  = host->images()->LinkImagesToFolder(targets, folder_id);
    const QString     message = result.value(QStringLiteral("message")).toString();
    const QString     refusal = result.value(QStringLiteral("refusal")).toString();
    const QJsonArray  linked =
        QJsonArray::fromVariantList(result.value(QStringLiteral("addedElementIds")).toList());
    const QJsonArray already = QJsonArray::fromVariantList(
        result.value(QStringLiteral("alreadyLinkedElementIds")).toList());
    const QJsonArray failed =
        QJsonArray::fromVariantList(result.value(QStringLiteral("failedElementIds")).toList());
    if (linked.isEmpty() && !refusal.isEmpty()) {
      SendNamedRefusal(reply, refusal, message);
      return;
    }
    if (linked.isEmpty() && already.isEmpty()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Failed, message);
      return;
    }
    reply.SendResult(QJsonObject{
        {"linked_ids", linked}, {"already_linked_ids", already}, {"failed_ids", failed}});
  };
  RegisterOrFail(registry, std::move(link), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
