//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// library.mount, library.mounts, library.sync, and library.unmount (Mounted Folder Plan, MF8).
// They call the FolderController mount commands that the Collections panel calls, which run
// MountService on the scan worker of MountScanCoordinator.

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimeZone>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "app/mount_service.hpp"
#include "app/project_service.hpp"
#include "ui/alcedo_main/album_backend/folder_controller.hpp"
#include "ui/alcedo_main/album_backend/path_utils.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_library_commands.hpp"

namespace alcedo::automation {
namespace {

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

auto MountJson(const MountSummary& summary) -> QJsonObject {
  QJsonObject mount{
      {"mount_id", static_cast<double>(summary.record.id)},
      {"folder_id", static_cast<double>(summary.record.root_folder_id)},
      {"path", ui::album_util::PathToQString(summary.record.root_path)},
      {"volume_identity", QString::fromStdString(summary.record.volume_identity)},
      {"volume_name", QString::fromStdString(summary.record.volume_name)},
      {"volume_mount_point", ui::album_util::PathToQString(summary.volume_mount_point)},
      {"state", ReachabilityName(summary.reachability)},
      {"present_count", static_cast<double>(summary.present_count)},
      {"missing_count", static_cast<double>(summary.missing_count)}};
  mount.insert("last_scan_at", summary.record.last_scan_at.has_value()
                                   ? QJsonValue(QDateTime::fromSecsSinceEpoch(
                                                    *summary.record.last_scan_at, QTimeZone::UTC)
                                                    .toString(Qt::ISODate))
                                   : QJsonValue(QJsonValue::Null));
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
      "('not_scanned', 'online', 'offline'), the counts of present and missing photos, and the "
      "end of the last complete scan (UTC).");
  mounts.params_schema = AutomationClosedParamsSchema();
  mounts.result_schema = AutomationObjectSchema(
      QJsonObject{{"mounts", QJsonObject{{"type", "array"}}}}, QJsonArray{"mounts"});
  mounts.handler = [host](const QJsonObject&, AutomationReply reply) {
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
    QJsonArray list;
    try {
      for (const auto& summary : service->ListMounts()) {
        list.push_back(MountJson(summary));
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
      "mount id, the folder id of the new collection, and the scan counts. A large directory "
      "needs a larger 'timeout_ms'; after a timeout the scan continues and library.mounts shows "
      "its result. A refused mount answers -32002 (rejected) with data.error 'nested_mount' or "
      "'directory_unreachable'.");
  mount.changes_state = true;
  mount.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"path", QJsonObject{{"type", "string"}}}}, QJsonArray{"path"});
  mount.result_schema =
      AutomationObjectSchema(QJsonObject{{"mount_id", QJsonObject{{"type", "integer"}}},
                                         {"folder_id", QJsonObject{{"type", "integer"}}},
                                         {"scan", ScanResultSchema()}},
                             QJsonArray{"mount_id", "folder_id", "scan"});
  mount.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const auto directory = ui::album_util::InputToPath(params.value("path").toString());
    if (!directory.has_value()) {
      SendAutomationParamError(reply, QStringLiteral("/path"),
                               QStringLiteral("the path is not a local path"));
      return;
    }
    QPointer<AutomationCommandWait> wait     = new AutomationCommandWait(std::move(reply), host);
    auto                            mount_id = std::make_shared<mount_id_t>(0);
    const auto                      refusal  = host->folders()->StartCreateMount(
        *directory,
        [wait, mount_id](std::exception_ptr error, mount_id_t created) {
          *mount_id = created;
          if (error && wait) {
            SendMountError(wait, error);
          }
        },
        [wait, mount_id, project](const MountScanResult& result) {
          if (!wait) {
            return;
          }
          sl_element_id_t folder_id = 0;
          for (const auto& summary : project->GetMountService()->ListMounts()) {
            if (summary.record.id == *mount_id) {
              folder_id = summary.record.root_folder_id;
            }
          }
          wait->SendResult(QJsonObject{{"mount_id", static_cast<double>(*mount_id)},
                                                             {"folder_id", static_cast<double>(folder_id)},
                                                             {"scan", ScanResultJson(*mount_id, result)}});
        });
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
      "'offline' and changes nothing.");
  sync.changes_state = true;
  sync.params_schema =
      AutomationClosedParamsSchema(QJsonObject{{"mount_id", MountIdSchema()}}, QJsonArray{});
  sync.result_schema = AutomationObjectSchema(
      QJsonObject{{"scans", QJsonObject{{"type", "array"}, {"items", ScanResultSchema()}}}},
      QJsonArray{"scans"});
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
      ids.push_back(*id);
    } else {
      for (const auto& summary : project->GetMountService()->ListMounts()) {
        ids.push_back(summary.record.id);
      }
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

  return ok;
}

}  // namespace alcedo::automation
