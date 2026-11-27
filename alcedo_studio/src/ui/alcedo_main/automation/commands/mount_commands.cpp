//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// library.mount, library.mounts, library.sync, and library.unmount (Mounted Folder Plan, MF8);
// library.mount.locate, library.mount.locate_photo, library.mount.remove_missing,
// library.mounts.wait_idle, and library.volumes.arrived (#1189). They call the FolderController
// mount commands that the Collections panel and the image menu call, which run MountService on
// the scan worker of MountScanCoordinator.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QTimeZone>
#include <QTimer>
#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "app/mount_service.hpp"
#include "app/project_service.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/mount_scan_coordinator.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_library_commands.hpp"

namespace alcedo::automation {
namespace {

/// The poll interval of library.mounts.wait_idle. The coordinator has no signal for its timers.
constexpr std::chrono::milliseconds kMountIdlePollInterval{20};

auto MountErrorName(MountError code) -> QString {
  switch (code) {
    case MountError::kNestedMount:
      return QStringLiteral("nested_mount");
    case MountError::kDirectoryUnreachable:
      return QStringLiteral("directory_unreachable");
    case MountError::kUnknownMount:
      return QStringLiteral("unknown_mount");
    case MountError::kStorageFailure:
      return QStringLiteral("storage_failure");
    case MountError::kPhotoIsNotMissing:
      return QStringLiteral("photo_is_not_missing");
    case MountError::kFileOutsideMount:
      return QStringLiteral("file_outside_mount");
    case MountError::kFileIsNotListed:
      return QStringLiteral("file_is_not_listed");
    case MountError::kFileHasPhoto:
      return QStringLiteral("file_has_photo");
    case MountError::kFileUnreadable:
      return QStringLiteral("file_unreadable");
  }
  return QStringLiteral("unknown");
}

/// Answers @p wait with the error of a mount operation: -32002 (rejected) with `data.error` for a
/// refusal of MountService, -32007 (failed) for a storage failure or another error.
void SendMountError(AutomationCommandWait* wait, std::exception_ptr error) {
  const QString message = ui::MountCommandErrorText(error);
  try {
    std::rethrow_exception(error);
  } catch (const MountOperationError& e) {
    if (e.Code() == MountError::kStorageFailure) {
      wait->SendOwnerError(AutomationErrorCode::Failed, QString::fromUtf8(e.what()));
      return;
    }
    wait->SendError(AutomationErrorCode::Rejected, message,
                    QJsonObject{{"reason", message}, {"error", MountErrorName(e.Code())}});
  } catch (...) {
    wait->SendOwnerError(AutomationErrorCode::Failed, message);
  }
}

/// Answers @p reply with -32002 (rejected) and `data.error` 'unknown_mount', as MountService
/// refuses an operation on a mount id that it does not have.
void SendUnknownMount(AutomationReply& reply) {
  const QString message = ui::MountCommandErrorText(std::make_exception_ptr(
      MountOperationError(MountError::kUnknownMount, "The mount does not exist.")));
  reply.SendError(
      AutomationErrorCode::Rejected, message,
      QJsonObject{{"reason", message}, {"error", MountErrorName(MountError::kUnknownMount)}});
}

auto ScanStatusName(MountScanStatus status) -> QString {
  switch (status) {
    case MountScanStatus::kComplete:
      return QStringLiteral("complete");
    case MountScanStatus::kOffline:
      return QStringLiteral("offline");
    case MountScanStatus::kCanceled:
      return QStringLiteral("canceled");
    case MountScanStatus::kStorageFailure:
      return QStringLiteral("storage_failure");
  }
  return QStringLiteral("unknown");
}

auto ScanResultJson(mount_id_t mount_id, const MountScanResult& result) -> QJsonObject {
  QJsonObject scan{{"mount_id", static_cast<double>(mount_id)},
                   {"status", ScanStatusName(result.status)},
                   {"new", static_cast<double>(result.imported_count + result.adopted_count)},
                   {"moved", static_cast<double>(result.moved_count)},
                   {"changed", static_cast<double>(result.changed_in_place_count)},
                   {"missing", static_cast<double>(result.missing_count)},
                   {"returned", static_cast<double>(result.returned_count)},
                   {"not_imported", static_cast<double>(result.import_failed_count)}};
  if (result.relocated_root.has_value()) {
    scan.insert("relocated_root", ui::album_util::PathToQString(*result.relocated_root));
  }
  if (!result.error.empty()) {
    scan.insert("error", QString::fromStdString(result.error));
  }
  return scan;
}

auto ScanResultSchema() -> QJsonObject {
  const QJsonObject count{{"type", "integer"}};
  return AutomationObjectSchema(
      QJsonObject{{"mount_id", count},
                  {"status", QJsonObject{{"type", "string"},
                                         {"enum", QJsonArray{"complete", "offline", "canceled",
                                                             "storage_failure"}}}},
                  {"new", count},
                  {"moved", count},
                  {"changed", count},
                  {"missing", count},
                  {"returned", count},
                  {"not_imported", count},
                  {"relocated_root", QJsonObject{{"type", "string"}}},
                  {"error", QJsonObject{{"type", "string"}}}},
      QJsonArray{"mount_id", "status", "new", "moved", "changed", "missing", "returned",
                 "not_imported"});
}

auto ReachabilityName(MountReachability reachability) -> QString {
  switch (reachability) {
    case MountReachability::kNotScanned:
      return QStringLiteral("not_scanned");
    case MountReachability::kOnline:
      return QStringLiteral("online");
    case MountReachability::kOffline:
      return QStringLiteral("offline");
  }
  return QStringLiteral("unknown");
}

auto ScanActivityName(ui::MountScanActivity activity) -> QString {
  switch (activity) {
    case ui::MountScanActivity::kIdle:
      return QStringLiteral("idle");
    case ui::MountScanActivity::kQueued:
      return QStringLiteral("queued");
    case ui::MountScanActivity::kRunning:
      return QStringLiteral("running");
    case ui::MountScanActivity::kPaused:
      return QStringLiteral("paused");
  }
  return QStringLiteral("unknown");
}

auto ElementIdArray(const std::vector<sl_element_id_t>& ids) -> QJsonArray {
  QJsonArray array;
  for (const auto id : ids) {
    array.push_back(static_cast<double>(id));
  }
  return array;
}

/// The mount object of library.mounts. @p scans is null when the host has no coordinator.
auto MountJson(const MountSummary& summary, const ui::MountScanCoordinator* scans, bool with_ids)
    -> QJsonObject {
  const mount_id_t id  = summary.record.id;
  const auto  activity = scans != nullptr ? scans->ScanActivity(id) : ui::MountScanActivity::kIdle;
  QJsonObject mount{
      {"mount_id", static_cast<double>(id)},
      {"folder_id", static_cast<double>(summary.record.root_folder_id)},
      {"path", ui::album_util::PathToQString(summary.record.root_path)},
      {"volume_identity", QString::fromStdString(summary.record.volume_identity)},
      {"volume_name", QString::fromStdString(summary.record.volume_name)},
      {"volume_mount_point", ui::album_util::PathToQString(summary.volume_mount_point)},
      {"state", ReachabilityName(summary.reachability)},
      {"present_count", static_cast<double>(summary.present_count)},
      {"missing_count", static_cast<double>(summary.missing_count)},
      {"scan", ScanActivityName(activity)},
      {"watched", scans != nullptr && scans->IsWatched(id)}};
  mount.insert("last_scan_at", summary.record.last_scan_at.has_value()
                                   ? QJsonValue(QDateTime::fromSecsSinceEpoch(
                                                    *summary.record.last_scan_at, QTimeZone::UTC)
                                                    .toString(Qt::ISODate))
                                   : QJsonValue(QJsonValue::Null));
  if (with_ids) {
    mount.insert("present_element_ids", ElementIdArray(summary.present_element_ids));
    mount.insert("missing_element_ids", ElementIdArray(summary.missing_element_ids));
  }
  return mount;
}

/// The mount id of the `mount_id` parameter. Answers @p reply and returns empty when it is not a
/// positive integer.
auto MountIdParam(const QJsonObject& params, AutomationReply& reply) -> std::optional<mount_id_t> {
  const auto id = AutomationElementIdParam(params, QStringLiteral("mount_id"), reply);
  if (!id.has_value()) {
    return std::nullopt;
  }
  return static_cast<mount_id_t>(*id);
}

auto MountIdSchema() -> QJsonObject {
  return QJsonObject{{"type", "integer"}, {"minimum", 1}, {"maximum", 4294967295.0}};
}

auto WaitSchema() -> QJsonObject {
  return QJsonObject{
      {"type", "boolean"},
      {"default", true},
      {"description",
       "true waits for the scan; false answers at once with the task id of the scan."}};
}

/// The local path of the `path` parameter. Answers @p reply and returns empty when it is not a
/// local path.
auto PathParam(const QJsonObject& params, AutomationReply& reply)
    -> std::optional<std::filesystem::path> {
  auto path = ui::album_util::InputToPath(params.value("path").toString());
  if (!path.has_value()) {
    SendAutomationParamError(reply, QStringLiteral("/path"),
                             QStringLiteral("the path is not a local path"));
  }
  return path;
}

/// True when the project has the mount @p mount_id.
auto HasMount(ProjectService& project, mount_id_t mount_id) -> bool {
  const auto mounts = project.GetMountService()->ListMounts();
  return std::any_of(mounts.begin(), mounts.end(), [mount_id](const MountSummary& summary) {
    return summary.record.id == mount_id;
  });
}

/// The folder id of the root collection of @p mount_id; 0 when the project has no such mount.
auto MountFolderId(ProjectService& project, mount_id_t mount_id) -> sl_element_id_t {
  for (const auto& summary : project.GetMountService()->ListMounts()) {
    if (summary.record.id == mount_id) {
      return summary.record.root_folder_id;
    }
  }
  return 0;
}

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

/// Collects the scan results of library.sync and answers @p wait after the last one.
struct SyncAnswer {
  QPointer<AutomationCommandWait> wait;
  std::size_t                     pending = 0;
  QJsonArray                      scans;

  void                            Take(mount_id_t mount_id, const MountScanResult& result) {
    scans.push_back(ScanResultJson(mount_id, result));
    if (--pending == 0 && wait) {
      wait->SendResult(QJsonObject{{"scans", scans}});
    }
  }
};

/// Answers @p wait when no scan or mount operation runs or waits and no watcher delay or volume
/// arrival delay runs. The first check runs queued, so the end of a worker that is posted now
/// counts.
void AnswerWhenMountsIdle(ui::ApplicationModuleHost* host, AutomationCommandWait* wait) {
  const auto check = [host, wait] {
    const ui::MountScanCoordinator* scans = host->mount_scans();
    const bool idle = scans == nullptr || (!scans->HasPendingScans() && !scans->HasPendingTimers());
    // After a timeout answer of the registry the wait only ends itself.
    if (idle || wait->IsAnswered()) {
      wait->SendResult(QJsonObject{{"idle", true}});
    }
  };
  auto* poll = new QTimer(wait);
  poll->setInterval(kMountIdlePollInterval);
  QObject::connect(poll, &QTimer::timeout, wait, check);
  poll->start();
  QMetaObject::invokeMethod(wait, check, Qt::QueuedConnection);
}

}  // namespace

auto RegisterAutomationMountCommands(AutomationCommandRegistry& registry,
                                     ui::ApplicationModuleHost* host, QString* error) -> bool {
  bool                  ok = true;

  AutomationCommandSpec mounts;
  mounts.method      = QStringLiteral("library.mounts");
  mounts.description = QStringLiteral(
      "Returns the mounted folders of the open project: the mount id, the folder id of the mount "
      "root collection, the disk directory, the volume identity, the last read display name of "
      "the volume (empty when it has none), the mount point of the volume when a scan of this "
      "session reached it (empty otherwise), the state of the last scan of this session "
      "('not_scanned', 'online', 'offline'), the counts of present and missing photos, the end "
      "of the last complete scan (UTC), what the scans of the mount do now ('scan': 'idle', "
      "'queued', 'running', or 'paused' when an import or a delete stopped the scan until it "
      "ends), and whether the directory watcher holds the mount ('watched'). With 'with_ids' "
      "true each mount also has the element ids of its present and missing photos.");
  mounts.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"with_ids",
       QJsonObject{{"type", "boolean"},
                   {"default", false},
                   {"description", "Add present_element_ids and missing_element_ids."}}}});
  mounts.result_schema = AutomationObjectSchema(
      QJsonObject{{"mounts", QJsonObject{{"type", "array"}}}}, QJsonArray{"mounts"});
  mounts.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto service = project->GetMountService();
    if (!service) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Failed,
                               QStringLiteral("The mounted folders cannot be read."));
      return;
    }
    const bool with_ids = params.value("with_ids").toBool(false);
    QJsonArray list;
    try {
      for (const auto& summary : service->ListMounts()) {
        list.push_back(MountJson(summary, host->mount_scans(), with_ids));
      }
    } catch (const std::exception& e) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Failed, QString::fromUtf8(e.what()));
      return;
    }
    reply.SendResult(QJsonObject{{"mounts", list}});
  };
  RegisterOrFail(registry, std::move(mounts), &ok, error);

  AutomationCommandSpec mount;
  mount.method      = QStringLiteral("library.mount");
  mount.description = QStringLiteral(
      "Mounts the disk directory 'path' as a new collection in the library root, then waits "
      "for the first scan, which imports the supported files of the directory and its "
      "subdirectories by reference. Alcedo Studio never writes to the directory. Returns the "
      "mount id, the folder id of the new collection, and the scan counts. With 'wait' false it "
      "answers after the mount is stored, with the task id of the first scan ('task_id') for "
      "tasks.wait and tasks.cancel instead of the counts. A large directory needs a larger "
      "'timeout_ms'; after a timeout the scan continues and library.mounts shows its result. A "
      "refused mount answers -32002 (rejected) with data.error 'nested_mount' or "
      "'directory_unreachable'.");
  mount.changes_state = true;
  mount.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"path", QJsonObject{{"type", "string"}}}, {"wait", WaitSchema()}},
      QJsonArray{"path"});
  mount.result_schema =
      AutomationObjectSchema(QJsonObject{{"mount_id", QJsonObject{{"type", "integer"}}},
                                         {"folder_id", QJsonObject{{"type", "integer"}}},
                                         {"scan", ScanResultSchema()},
                                         {"task_id", QJsonObject{{"type", "string"}}}},
                             QJsonArray{"mount_id", "folder_id"});
  mount.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto directory = PathParam(params, reply);
    if (!directory.has_value()) {
      return;
    }
    const bool                      wait_for_scan = params.value("wait").toBool(true);
    QPointer<AutomationCommandWait> wait     = new AutomationCommandWait(std::move(reply), host);
    auto                            mount_id = std::make_shared<mount_id_t>(0);
    ui::MountScanEndedCallback      first_scan_ended;
    if (wait_for_scan) {
      first_scan_ended = [wait, mount_id, project](const MountScanResult& result) {
        if (!wait) {
          return;
        }
        wait->SendResult(
            QJsonObject{{"mount_id", static_cast<double>(*mount_id)},
                        {"folder_id", static_cast<double>(MountFolderId(*project, *mount_id))},
                        {"scan", ScanResultJson(*mount_id, result)}});
      };
    }
    const auto refusal = host->folders()->StartCreateMount(
        *directory,
        [wait, mount_id, project, host, wait_for_scan](std::exception_ptr error,
                                                       mount_id_t         created) {
          *mount_id = created;
          if (!wait) {
            return;
          }
          if (error) {
            SendMountError(wait, error);
            return;
          }
          if (!wait_for_scan) {
            // StartCreateMount requested the first scan before this call.
            wait->SendResult(
                QJsonObject{{"mount_id", static_cast<double>(created)},
                            {"folder_id", static_cast<double>(MountFolderId(*project, created))},
                            {"task_id", host->mount_scans()->SynchronizeTaskId(created)}});
          }
        },
        std::move(first_scan_ended));
    if (!refusal.isEmpty()) {
      wait->SendOwnerError(AutomationErrorCode::Rejected, refusal);
    }
  };
  RegisterOrFail(registry, std::move(mount), &ok, error);

  AutomationCommandSpec sync;
  sync.method      = QStringLiteral("library.sync");
  sync.description = QStringLiteral(
      "Scans the mounted folder 'mount_id', or each mounted folder without it, and waits for the "
      "scans. A scan makes the collection equal to its directory: new files are imported, moved "
      "and renamed files keep their photo, and a file that is gone makes its photo missing. "
      "Returns the counts of each scan. A scan of an unreachable directory has the status "
      "'offline' and changes nothing. A request during a scan of the same mount waits for the "
      "scan after it. With 'wait' false it answers at once with the task id of each requested "
      "scan ('tasks': mount_id and task_id) for tasks.wait and tasks.cancel. An unknown mount id "
      "answers -32002 (rejected) with data.error 'unknown_mount'.");
  sync.changes_state = true;
  sync.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"mount_id", MountIdSchema()}, {"wait", WaitSchema()}}, QJsonArray{});
  sync.result_schema = AutomationObjectSchema(
      QJsonObject{{"scans", QJsonObject{{"type", "array"}, {"items", ScanResultSchema()}}},
                  {"tasks", QJsonObject{{"type", "array"}}}});
  sync.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    std::vector<mount_id_t> ids;
    if (params.contains("mount_id")) {
      const auto id = MountIdParam(params, reply);
      if (!id.has_value()) {
        return;
      }
      if (!HasMount(*project, *id)) {
        SendUnknownMount(reply);
        return;
      }
      ids.push_back(*id);
    } else {
      for (const auto& summary : project->GetMountService()->ListMounts()) {
        ids.push_back(summary.record.id);
      }
    }
    if (!params.value("wait").toBool(true)) {
      QJsonArray tasks;
      for (const auto id : ids) {
        const auto refusal = host->folders()->StartSynchronize(id);
        if (!refusal.isEmpty()) {
          // The scans that started keep running.
          SendAutomationOwnerError(reply, AutomationErrorCode::Rejected, refusal);
          return;
        }
        tasks.push_back(QJsonObject{{"mount_id", static_cast<double>(id)},
                                    {"task_id", host->mount_scans()->SynchronizeTaskId(id)}});
      }
      reply.SendResult(QJsonObject{{"tasks", tasks}});
      return;
    }
    QPointer<AutomationCommandWait> wait = new AutomationCommandWait(std::move(reply), host);
    if (ids.empty()) {
      wait->SendResult(QJsonObject{{"scans", QJsonArray{}}});
      return;
    }
    auto answer     = std::make_shared<SyncAnswer>();
    answer->wait    = wait;
    answer->pending = ids.size();
    for (const auto id : ids) {
      const auto refusal = host->folders()->StartSynchronize(
          id, [answer, id](const MountScanResult& result) { answer->Take(id, result); });
      if (!refusal.isEmpty()) {
        // The scans that started keep running; their results are dropped with the answer.
        answer->wait = nullptr;
        wait->SendOwnerError(AutomationErrorCode::Rejected, refusal);
        return;
      }
    }
  };
  RegisterOrFail(registry, std::move(sync), &ok, error);

  AutomationCommandSpec unmount;
  unmount.method      = QStringLiteral("library.unmount");
  unmount.description = QStringLiteral(
      "Removes the mounted folder 'mount_id'. Its photos stay in the project as ordinary photos "
      "with their edits, marks, and collection links; the mounted collections are removed. "
      "Nothing changes on the disk.");
  unmount.changes_state = true;
  unmount.params_schema = AutomationClosedParamsSchema(QJsonObject{{"mount_id", MountIdSchema()}},
                                                       QJsonArray{"mount_id"});
  unmount.result_schema =
      AutomationObjectSchema(QJsonObject{{"mount_id", QJsonObject{{"type", "integer"}}},
                                         {"unmounted", QJsonObject{{"type", "boolean"}}}},
                             QJsonArray{"mount_id", "unmounted"});
  unmount.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto id = MountIdParam(params, reply);
    if (!id.has_value()) {
      return;
    }
    QPointer<AutomationCommandWait> wait = new AutomationCommandWait(std::move(reply), host);
    const auto                      refusal =
        host->folders()->StartUnmount(*id, [wait, id = *id](std::exception_ptr error) {
          if (!wait) {
            return;
          }
          if (error) {
            SendMountError(wait, error);
            return;
          }
          wait->SendResult(QJsonObject{{"mount_id", static_cast<double>(id)}, {"unmounted", true}});
        });
    if (!refusal.isEmpty()) {
      wait->SendOwnerError(AutomationErrorCode::Rejected, refusal);
    }
  };
  RegisterOrFail(registry, std::move(unmount), &ok, error);

  AutomationCommandSpec locate;
  locate.method      = QStringLiteral("library.mount.locate");
  locate.description = QStringLiteral(
      "Binds the mounted folder 'mount_id' to the new location 'path' of its root directory "
      "(Locate Folder of the Collections panel), then waits for the scan of the new location. "
      "Each photo keeps its element; a file that is not at the new location becomes missing, "
      "and a new file is imported. Returns the mount id and the scan counts. A refusal answers "
      "-32002 (rejected) with data.error 'unknown_mount', 'directory_unreachable', or "
      "'nested_mount' (the directory is, is inside, or holds the directory of another mount).");
  locate.changes_state = true;
  locate.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"mount_id", MountIdSchema()}, {"path", QJsonObject{{"type", "string"}}}},
      QJsonArray{"mount_id", "path"});
  locate.result_schema = AutomationObjectSchema(
      QJsonObject{{"mount_id", QJsonObject{{"type", "integer"}}}, {"scan", ScanResultSchema()}},
      QJsonArray{"mount_id", "scan"});
  locate.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto id = MountIdParam(params, reply);
    if (!id.has_value()) {
      return;
    }
    const auto directory = PathParam(params, reply);
    if (!directory.has_value()) {
      return;
    }
    QPointer<AutomationCommandWait> wait    = new AutomationCommandWait(std::move(reply), host);
    const auto                      refusal = host->folders()->StartRelocateMountRoot(
        *id, *directory,
        [wait](std::exception_ptr error) {
          if (error && wait) {
            SendMountError(wait, error);
          }
        },
        [wait, id = *id](const MountScanResult& result) {
          if (!wait) {
            return;
          }
          QJsonObject answer;
          answer.insert("mount_id", static_cast<double>(id));
          answer.insert("scan", ScanResultJson(id, result));
          wait->SendResult(answer);
        });
    if (!refusal.isEmpty()) {
      wait->SendOwnerError(AutomationErrorCode::Rejected, refusal);
    }
  };
  RegisterOrFail(registry, std::move(locate), &ok, error);

  AutomationCommandSpec locate_photo;
  locate_photo.method      = QStringLiteral("library.mount.locate_photo");
  locate_photo.description = QStringLiteral(
      "Takes the file 'path' as the source of the missing mounted photo 'element_id' (Locate of "
      "the image menu). The photo keeps its element, edit history, marks, and collection links, "
      "and moves to the collection of the directory of the file. Returns the element id. A "
      "refusal answers -32002 (rejected) with data.error 'photo_is_not_missing', "
      "'file_outside_mount' (the file is not below the root directory of the mount of the "
      "photo), 'file_is_not_listed' (a scan does not list the file: an unsupported type, a name "
      "or a directory that starts with a period, or a link), 'file_has_photo' (another photo of "
      "the mount has the file), or 'file_unreadable'.");
  locate_photo.changes_state = true;
  locate_photo.params_schema =
      AutomationClosedParamsSchema(QJsonObject{{"element_id", AutomationElementIdSchema()},
                                               {"path", QJsonObject{{"type", "string"}}}},
                                   QJsonArray{"element_id", "path"});
  locate_photo.result_schema =
      AutomationObjectSchema(QJsonObject{{"element_id", QJsonObject{{"type", "integer"}}},
                                         {"located", QJsonObject{{"type", "boolean"}}}},
                             QJsonArray{"element_id", "located"});
  locate_photo.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto element_id = AutomationElementIdParam(params, QStringLiteral("element_id"), reply);
    if (!element_id.has_value()) {
      return;
    }
    const auto file = PathParam(params, reply);
    if (!file.has_value()) {
      return;
    }
    QPointer<AutomationCommandWait> wait    = new AutomationCommandWait(std::move(reply), host);
    const auto                      refusal = host->folders()->StartLocateMissingPhoto(
        *element_id, *file, [wait, element_id = *element_id](std::exception_ptr error) {
          if (!wait) {
            return;
          }
          if (error) {
            SendMountError(wait, error);
            return;
          }
          wait->SendResult(
              QJsonObject{{"element_id", static_cast<double>(element_id)}, {"located", true}});
        });
    if (!refusal.isEmpty()) {
      wait->SendOwnerError(AutomationErrorCode::Rejected, refusal);
    }
  };
  RegisterOrFail(registry, std::move(locate_photo), &ok, error);

  AutomationCommandSpec remove_missing;
  remove_missing.method      = QStringLiteral("library.mount.remove_missing");
  remove_missing.description = QStringLiteral(
      "Removes each missing photo of the mounted folder 'mount_id' from the project (Remove "
      "Missing Photos of the Collections panel): the photo, its collection links, and its edit "
      "history. No file is removed from the disk, and a present photo stays. Returns the "
      "removed element ids and their image ids (at the same index; 0 for a photo without an "
      "image). An unknown mount id answers -32002 (rejected) with data.error 'unknown_mount'.");
  remove_missing.changes_state = true;
  remove_missing.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"mount_id", MountIdSchema()}}, QJsonArray{"mount_id"});
  remove_missing.result_schema =
      AutomationObjectSchema(QJsonObject{{"mount_id", QJsonObject{{"type", "integer"}}},
                                         {"removed_element_ids", QJsonObject{{"type", "array"}}},
                                         {"removed_image_ids", QJsonObject{{"type", "array"}}}},
                             QJsonArray{"mount_id", "removed_element_ids", "removed_image_ids"});
  remove_missing.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto id = MountIdParam(params, reply);
    if (!id.has_value()) {
      return;
    }
    QPointer<AutomationCommandWait> wait    = new AutomationCommandWait(std::move(reply), host);
    const auto                      refusal = host->folders()->StartRemoveMissingPhotos(
        *id, [wait, id = *id](std::exception_ptr error, const MountMissingPhotoRemoval& removal) {
          if (!wait) {
            return;
          }
          if (error) {
            SendMountError(wait, error);
            return;
          }
          QJsonArray image_ids;
          for (const auto image_id : removal.image_ids) {
            image_ids.push_back(static_cast<double>(image_id));
          }
          QJsonObject answer;
          answer.insert("mount_id", static_cast<double>(id));
          answer.insert("removed_element_ids", ElementIdArray(removal.element_ids));
          answer.insert("removed_image_ids", image_ids);
          wait->SendResult(answer);
        });
    if (!refusal.isEmpty()) {
      wait->SendOwnerError(AutomationErrorCode::Rejected, refusal);
    }
  };
  RegisterOrFail(registry, std::move(remove_missing), &ok, error);

  AutomationCommandSpec wait_idle;
  wait_idle.method      = QStringLiteral("library.mounts.wait_idle");
  wait_idle.description = QStringLiteral(
      "Waits until no scan and no mount operation runs or waits, and no watcher delay (5 s "
      "after the last change notification of a mount) or volume arrival delay (2 s) runs, then "
      "returns idle true. After a disk change it answers when the scans that the directory "
      "watcher and the volume events started have ended. A change that the watcher has not "
      "reported yet does not count. A scan that an import or a delete stopped counts until it "
      "has run again. Answers also without a project.");
  wait_idle.params_schema = AutomationClosedParamsSchema();
  wait_idle.result_schema = AutomationObjectSchema(
      QJsonObject{{"idle", QJsonObject{{"type", "boolean"}}}}, QJsonArray{"idle"});
  wait_idle.handler = [host](const QJsonObject&, AutomationReply reply) {
    AnswerWhenMountsIdle(host, new AutomationCommandWait(std::move(reply), host));
  };
  RegisterOrFail(registry, std::move(wait_idle), &ok, error);

  AutomationCommandSpec arrived;
  arrived.method      = QStringLiteral("library.volumes.arrived");
  arrived.description = QStringLiteral(
      "Returns the volume of the card of the Disk section: the last volume that arrived while "
      "the project was open and that holds no mount of the project. 'volume' has the display "
      "name (empty when it has none), the mount point, whether its root holds a DCIM directory, "
      "and the volume identity (empty when it has none); it is null when there is no card.");
  arrived.params_schema = AutomationClosedParamsSchema();
  arrived.result_schema = AutomationObjectSchema(
      QJsonObject{{"volume", QJsonObject{{"type", QJsonArray{"object", "null"}}}}},
      QJsonArray{"volume"});
  arrived.handler = [host](const QJsonObject&, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const ui::FolderController& folders = *host->folders();
    const QVariantMap           card    = folders.ArrivedVolume();
    if (card.isEmpty()) {
      reply.SendResult(QJsonObject{{"volume", QJsonValue(QJsonValue::Null)}});
      return;
    }
    reply.SendResult(QJsonObject{
        {"volume",
         QJsonObject{{"name", card.value(QStringLiteral("name")).toString()},
                     {"mount_point", ui::album_util::PathToQString(folders.arrived_mount_point())},
                     {"dcim", card.value(QStringLiteral("hasDcim")).toBool()},
                     {"identity", QString::fromStdString(folders.arrived_volume_identity())}}}});
  };
  RegisterOrFail(registry, std::move(arrived), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
