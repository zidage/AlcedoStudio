// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include <QSignalSpy>
#include <chrono>
#include <fstream>
#include <future>

#include "../automation/automation_in_process_session.hpp"
#include "../support/project_maintenance_fixture.hpp"
#include "app/project_package_service.hpp"
#include "sleeve/storage.hpp"
#include "storage/mapper/sleeve/element/file_mapper.hpp"
#include "support/storage_test_access.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/automation/automation_project_commands.hpp"

namespace alcedo::ui {
namespace {
class ProjectMaintenanceControllerTest : public ::testing::Test {
 protected:
  void Open(ApplicationModuleHost& host) {
    QSignalSpy loaded(host.project(), &ProjectModule::ProjectChanged);
    ASSERT_TRUE(host.project()->CreateProjectInFolderNamed(directory_.path(),
                                                           QStringLiteral("Maintenance")));
    ASSERT_TRUE(loaded.wait(15000));
    ASSERT_TRUE(host.project()->ServiceReady());
    EXPECT_EQ(host.maintenance()->ProjectPath(),
              QString::fromStdWString(host.project()->handler().package_path().wstring()));
  }
  void ImportDuplicates(ApplicationModuleHost& host) {
    const auto source = std::filesystem::path(directory_.path().toStdWString()) / "photo.jpg";
    std::filesystem::copy_file(std::filesystem::path(__FILE__).parent_path().parent_path() /
                                   "resources/raster/adobe_rgb_exif_r03_no_icc.jpg",
                               source);
    auto completion   = std::make_shared<std::promise<ImportResult>>();
    auto future       = completion->get_future();
    auto job          = std::make_shared<ImportJob>();
    job->on_finished_ = [completion](const auto& result) { completion->set_value(result); };
    ImportOptions options;
    options.skip_duplicates = false;
    host.project()->handler().import_service()->ImportToFolder({source, source}, {}, options, job);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(90)), std::future_status::ready);
    ASSERT_EQ(future.get().imported_, 2);
    host.project()->handler().import_service()->SyncImports(job->import_log_->Snapshot(), L"");
    ASSERT_TRUE(job->IsCancelationAcked());
  }
  auto ScanAndSelect(ApplicationModuleHost& host) -> uint {
    return ScanAndSelect(*host.maintenance());
  }
  auto ScanAndSelect(ProjectMaintenanceController& controller) -> uint {
    QSignalSpy scanned(&controller, &ProjectMaintenanceController::ScanFinished);
    if (!controller.Scan() || !scanned.wait(15000)) return 0;
    for (const auto& value : controller.Model()->ReadPage()) {
      const auto row = value.toMap();
      if (row.value("removable").toBool()) return row.value("entryId").toUInt();
    }
    return 0;
  }
  QTemporaryDir directory_{alcedo::test::MaintenanceTestDirectory()};
};
TEST_F(ProjectMaintenanceControllerTest, NoLoadedProjectDisablesScanAndApply) {
  ApplicationModuleHost host;
  EXPECT_FALSE(host.maintenance()->Available());
  EXPECT_TRUE(host.maintenance()->ProjectPath().isEmpty());
  EXPECT_FALSE(host.maintenance()->Scan());
  EXPECT_FALSE(host.maintenance()->ApplyIds({1}));
}
TEST_F(ProjectMaintenanceControllerTest, EnabledScanPackagesMarksAndReviewOnlyKeepsThem) {
  ApplicationModuleHost host;
  Open(host);
  ImportDuplicates(host);
  const auto directory = std::filesystem::path(directory_.path().toStdWString());
  const auto source    = directory / "photo.jpg";
  const auto original  = directory / "original.jpg";
  std::filesystem::copy_file(source, original);
  std::ofstream(source, std::ios::binary) << "replacement source";
  auto*      maintenance = host.maintenance();
  QSignalSpy scanned(maintenance, &ProjectMaintenanceController::ScanFinished);
  ASSERT_TRUE(maintenance->Scan(true));
  ASSERT_TRUE(scanned.wait(15000));
  ASSERT_TRUE(scanned.takeFirst().front().toBool()) << maintenance->Error().toStdString();
  EXPECT_EQ(maintenance->SourceChangedEntries().size(), 2);

  ProjectPackageService packages;
  std::filesystem::path db, meta;
  QString               error;
  ASSERT_TRUE(packages.UnpackProjectToWorkspace(host.project()->handler().package_path(), directory,
                                                QStringLiteral("marked"), &db, &meta, &error))
      << error.toStdString();
  Storage saved(db);
  EXPECT_EQ(saved.GetSourceFileStore().ReadChangedEntryIds().size(), 2);

  std::filesystem::copy_file(original, source, std::filesystem::copy_options::overwrite_existing);
  ASSERT_TRUE(maintenance->Scan(false));
  ASSERT_TRUE(scanned.wait(15000));
  ASSERT_TRUE(scanned.takeFirst().front().toBool()) << maintenance->Error().toStdString();
  EXPECT_EQ(maintenance->SourceChangedEntries().size(), 2);
  ASSERT_TRUE(maintenance->Scan(true));
  ASSERT_TRUE(scanned.wait(15000));
  ASSERT_TRUE(scanned.takeFirst().front().toBool()) << maintenance->Error().toStdString();
  EXPECT_TRUE(maintenance->SourceChangedEntries().isEmpty());
}
TEST_F(ProjectMaintenanceControllerTest, DialogReopenShowsCurrentScan) {
  ApplicationModuleHost host;
  Open(host);
  auto*      maintenance = host.maintenance();
  QSignalSpy scanned(maintenance, &ProjectMaintenanceController::ScanFinished);
  ASSERT_TRUE(maintenance->Scan());
  EXPECT_TRUE(maintenance->Scanning());
  EXPECT_EQ(host.background_tasks()->RunningCount(), 1);
  ASSERT_TRUE(scanned.wait(15000));
  EXPECT_TRUE(scanned.front().front().toBool());
  EXPECT_EQ(maintenance->State(), QStringLiteral("review"));
  EXPECT_EQ(maintenance->Model()->rowCount(), 0);
  // Opening a view reads this same host-owned controller; no view owns the scan lifetime.
  EXPECT_EQ(host.maintenance(), maintenance);
  EXPECT_FALSE(maintenance->Busy());
}
TEST_F(ProjectMaintenanceControllerTest, ScanCancelLeavesApplyDisabled) {
  ApplicationModuleHost host;
  Open(host);
  auto*      maintenance = host.maintenance();
  QSignalSpy scanned(maintenance, &ProjectMaintenanceController::ScanFinished);
  {
    auto guard = StorageTestAccess::GetDatabase(*host.project()->handler().project()->GetStorage())
                     .GetConnectionGuard();
    auto lock = guard.Lock();
    ASSERT_TRUE(maintenance->Scan());
    maintenance->CancelScan();
  }
  ASSERT_TRUE(scanned.wait(15000));
  EXPECT_FALSE(scanned.front().front().toBool());
  EXPECT_FALSE(maintenance->ApplyIds({1}));
  EXPECT_EQ(maintenance->Model()->SelectedCount(), 0);
}
TEST_F(ProjectMaintenanceControllerTest, ProjectChangeReleasesOldResultsAfterScanCancellation) {
  ApplicationModuleHost host;
  Open(host);
  ASSERT_TRUE(host.maintenance()->Scan());
  QSignalSpy closed(host.application_close(), &ApplicationCloseCoordinator::ProjectCloseFinished);
  ASSERT_TRUE(host.application_close()->BeginProjectClose(false).isEmpty());
  ASSERT_TRUE(closed.wait(15000));
  EXPECT_TRUE(closed.front().front().toBool());
  EXPECT_FALSE(host.maintenance()->Available());
  EXPECT_EQ(host.maintenance()->Model()->rowCount(), 0);
  EXPECT_FALSE(host.maintenance()->Busy());
}
TEST_F(ProjectMaintenanceControllerTest, RemoveSelectedCommandRequiresExplicitIds) {
  ApplicationModuleHost                 host;
  automation::AutomationCommandRegistry registry;
  QString                               error;
  ASSERT_TRUE(automation::RegisterAutomationProjectCommands(registry, &host, &error))
      << error.toStdString();
  EXPECT_TRUE(registry.Contains(QStringLiteral("project.maintenance.scan")));
  EXPECT_TRUE(registry.Contains(QStringLiteral("project.maintenance.remove_selected")));
  const auto commands = registry.Describe();
  for (const auto& command : commands)
    if (command.toObject().value("method") ==
        QStringLiteral("project.maintenance.remove_selected")) {
      const auto schema = command.toObject().value("params_schema").toObject();
      // Dispatch validation is also covered by the shared automation schema suite.
      EXPECT_TRUE(schema.value("required").toArray().contains(QStringLiteral("element_ids")));
    }
}
TEST_F(ProjectMaintenanceControllerTest, SuccessfulApplyReopensAndReportsActualCounts) {
  ApplicationModuleHost host;
  Open(host);
  ImportDuplicates(host);
  const auto id = ScanAndSelect(host);
  ASSERT_NE(id, 0);
  const auto path          = host.project()->handler().package_path();
  bool       progress_seen = false;
  QObject::connect(host.maintenance(), &ProjectMaintenanceController::Changed, &host, [&] {
    if (host.maintenance()->ProgressDeterminate()) {
      progress_seen = true;
      EXPECT_EQ(host.maintenance()->RemovedCount(), 0);
      EXPECT_EQ(host.maintenance()->TotalEntries(), 1);
      EXPECT_LE(host.maintenance()->ProcessedEntries(), 1);
    }
  });
  QSignalSpy finished(host.maintenance(), &ProjectMaintenanceController::Finished);
  ASSERT_TRUE(host.maintenance()->ApplyIds({id}));
  automation::AutomationCommandRegistry registry;
  QString                               error;
  ASSERT_TRUE(automation::RegisterAutomationProjectCommands(registry, &host, &error));
  EXPECT_TRUE(registry.AdmitStateChange().has_value());
  EXPECT_FALSE(host.images()
                   ->SetImageDescription(id, QStringLiteral("Must be rejected"))
                   .value("success")
                   .toBool());
  ASSERT_TRUE(finished.wait(30000));
  ASSERT_TRUE(finished.front().front().toBool()) << host.maintenance()->Error().toStdString();
  EXPECT_TRUE(progress_seen);
  EXPECT_EQ(host.maintenance()->ProcessedEntries(), 1);
  EXPECT_FALSE(host.maintenance()->ProgressDeterminate());
  EXPECT_EQ(host.maintenance()->RemovedCount(), 1);
  EXPECT_EQ(host.maintenance()->RetainedCount(), 1);
  EXPECT_EQ(host.project()->handler().package_path(), path);
  EXPECT_EQ(
      host.project()->handler().project()->GetStorage()->GetElementStore().CountFilesInFolder(0),
      1);
  EXPECT_TRUE(host.maintenance()->BackupVerified());
  EXPECT_FALSE(registry.AdmitStateChange().has_value());
  QSignalSpy scanned(host.maintenance(), &ProjectMaintenanceController::ScanFinished);
  ASSERT_TRUE(host.maintenance()->Scan());
  ASSERT_TRUE(scanned.wait(15000));
  EXPECT_EQ(host.maintenance()->Model()->MatchingCount(), 0);
}
TEST_F(ProjectMaintenanceControllerTest, InvalidNumericIdsCannotStartBackupOrRemoval) {
  ApplicationModuleHost host;
  Open(host);
  ImportDuplicates(host);
  const auto id = ScanAndSelect(host);
  ASSERT_NE(id, 0);
  const std::vector<QVariantList> invalid{
      {static_cast<double>(id) + 0.25}, {QString::number(id)}, {true}, {0}, {-1}, {4294967296.0}};
  for (const auto& values : invalid) {
    EXPECT_FALSE(host.maintenance()->ApplyIds(values));
    EXPECT_FALSE(host.maintenance()->Busy());
    EXPECT_TRUE(host.maintenance()->BackupPath().isEmpty());
  }
  EXPECT_EQ(
      host.project()->handler().project()->GetStorage()->GetElementStore().CountFilesInFolder(0),
      2);
}
TEST_F(ProjectMaintenanceControllerTest, AlreadyAdmittedRatingFinishesBeforeCapture) {
  ApplicationModuleHost host;
  Open(host);
  ImportDuplicates(host);
  const auto id = ScanAndSelect(host);
  ASSERT_NE(id, 0);
  QSignalSpy finished(host.maintenance(), &ProjectMaintenanceController::Finished);
  {
    auto guard = StorageTestAccess::GetDatabase(*host.project()->handler().project()->GetStorage())
                     .GetConnectionGuard();
    auto               lock     = guard.Lock();
    const auto         image_id = FileMapper(guard.conn_).GetBoundImageById(id);
    const QVariantList targets{QVariantMap{{"elementId", id}, {"imageId", image_id}}};
    ASSERT_TRUE(host.images()->StartSetImageRatings(targets, 4).value("started").toBool());
    ASSERT_TRUE(host.maintenance()->ApplyIds({id}));
    EXPECT_TRUE(host.maintenance()->BackupPath().isEmpty());
  }
  ASSERT_TRUE(finished.wait(30000));
  EXPECT_FALSE(finished.front().front().toBool());
  EXPECT_EQ(host.maintenance()->RemovedCount(), 0);
  EXPECT_EQ(
      host.project()->handler().project()->GetStorage()->GetElementStore().CountFilesInFolder(0),
      2);
  EXPECT_EQ(host.images()->GetImageRating(id, 0).value("rating").toInt(), 4);
}
TEST_F(ProjectMaintenanceControllerTest, ActivationFailureRestoresVerifiedBackupAndReopens) {
  ApplicationModuleHost host;
  Open(host);
  ImportDuplicates(host);
  ProjectMaintenanceService service;
  int                       attempts = 0;
  MaintenanceOperationHooks hooks;
  hooks.reopen_project = [&](const QString& path) {
    return ++attempts == 1 ? false : host.project_launch()->BeginOpen(path);
  };
  ProjectMaintenanceController controller(service, *host.project(), *host.project_launch(),
                                          *host.editor_session(), *host.background_tasks(),
                                          *host.interaction_policy(), nullptr, std::move(hooks));
  const auto                   id = ScanAndSelect(controller);
  ASSERT_NE(id, 0);
  QSignalSpy finished(&controller, &ProjectMaintenanceController::Finished);
  ASSERT_TRUE(controller.ApplyIds({id}));
  ASSERT_TRUE(finished.wait(30000));
  EXPECT_FALSE(finished.front().front().toBool());
  EXPECT_EQ(attempts, 2);
  EXPECT_FALSE(controller.Busy());
  EXPECT_EQ(controller.RemovedCount(), 0);
  EXPECT_EQ(
      host.project()->handler().project()->GetStorage()->GetElementStore().CountFilesInFolder(0),
      2);
  EXPECT_TRUE(controller.Error().contains(QStringLiteral("restored and reopened")));
}
TEST_F(ProjectMaintenanceControllerTest, RestoreFailureReportsBothErrorsAndKeepsWritesDisabled) {
  ApplicationModuleHost host;
  Open(host);
  ImportDuplicates(host);
  ProjectMaintenanceService service;
  MaintenanceOperationHooks hooks;
  hooks.reopen_project = [](const QString&) { return false; };
  hooks.create_package = [] {
    return std::make_shared<ProjectMaintenancePackage>([](auto stage) {
      if (stage == MaintenancePackageStage::Restoration)
        throw std::runtime_error("Injected restoration failure");
    });
  };
  ProjectMaintenanceController controller(service, *host.project(), *host.project_launch(),
                                          *host.editor_session(), *host.background_tasks(),
                                          *host.interaction_policy(), nullptr, std::move(hooks));
  const auto                   id = ScanAndSelect(controller);
  ASSERT_NE(id, 0);
  QSignalSpy finished(&controller, &ProjectMaintenanceController::Finished);
  ASSERT_TRUE(controller.ApplyIds({id}));
  ASSERT_TRUE(finished.wait(30000));
  EXPECT_FALSE(finished.front().front().toBool());
  EXPECT_TRUE(controller.Busy()) << controller.Error().toStdString();
  EXPECT_EQ(controller.State(), QStringLiteral("recovery"));
  EXPECT_TRUE(controller.Error().contains(QStringLiteral("reopening could not start")));
  EXPECT_TRUE(controller.Error().contains(QStringLiteral("Injected restoration failure")));
  EXPECT_FALSE(host.interaction_policy()->CanChangeStateAsUser());
  EXPECT_TRUE(
      std::filesystem::exists(std::filesystem::path(controller.BackupPath().toStdWString())));
  EXPECT_EQ(controller.RemovedCount(), 0);
  // The process exits after this assertion; release its test task without resuming any writer.
  for (const auto& task : host.background_tasks()->Tasks()) {
    const auto row = task.toMap();
    if (row.value("kind") == QStringLiteral("maintenanceApply"))
      host.background_tasks()->FinishTask(row.value("id").toString(), BackgroundTaskState::Failed);
  }
}
TEST_F(ProjectMaintenanceControllerTest,
       CommandsPreserveProtectedRowsAndReportActualRemovalAfterReopen) {
  automation::test::InProcessAutomationSession session;
  session.CallResult(QStringLiteral("project.create"),
                     {{"folder", directory_.path()}, {"name", "commands"}});
  auto& host = session.host();
  ImportDuplicates(host);
  QJsonObject scan;
  {
    ProjectMaintenanceStore store(
        StorageTestAccess::GetDatabase(*host.project()->handler().project()->GetStorage()));
    const auto              before = store.RetainedRowDigests({}, {}, false);
    scan                           = session.CallResult(QStringLiteral("project.maintenance.scan"));
    EXPECT_EQ(store.RetainedRowDigests({}, {}, false), before);
  }
  uint removable = 0, retained = 0;
  for (const auto& value : scan.value("findings").toArray()) {
    const auto row = value.toObject();
    if (row.value("removable").toBool())
      removable = row.value("entryId").toInt();
    else
      retained = row.value("entryId").toInt();
  }
  ASSERT_NE(removable, 0);
  ASSERT_NE(retained, 0);
  auto refused = session.Call(QStringLiteral("project.maintenance.remove_selected"), {});
  ASSERT_TRUE(refused);
  EXPECT_TRUE(refused->error.has_value());
  refused = session.Call(QStringLiteral("project.maintenance.remove_selected"),
                         {{"element_ids", QJsonArray{static_cast<int>(retained)}}});
  ASSERT_TRUE(refused);
  EXPECT_TRUE(refused->error.has_value());
  const auto result =
      session.CallResult(QStringLiteral("project.maintenance.remove_selected"),
                         {{"element_ids", QJsonArray{static_cast<int>(removable)}}});
  EXPECT_EQ(result.value("removed_count").toInt(), 1);
  EXPECT_EQ(result.value("retained_count").toInt(), 1);
  EXPECT_FALSE(result.value("backup_path").toString().isEmpty());
  session.CallResult(QStringLiteral("project.save"));
  const auto second = session.CallResult(QStringLiteral("project.maintenance.scan"));
  EXPECT_EQ(second.value("matching_count").toInt(), 0);
}
TEST_F(ProjectMaintenanceControllerTest, ExternalPackageChangeDuringActivationKeepsCurrentFile) {
  ApplicationModuleHost host;
  Open(host);
  ImportDuplicates(host);
  ProjectMaintenanceService                  service;
  std::shared_ptr<ProjectMaintenancePackage> operation;
  std::filesystem::path                      original;
  std::string                                external_digest;
  MaintenanceOperationHooks                  hooks;
  hooks.create_package = [&] {
    operation = std::make_shared<ProjectMaintenancePackage>();
    return operation;
  };
  hooks.reopen_project = [&](const QString& path) {
    original = std::filesystem::path(path.toStdWString());
    std::filesystem::copy_file(operation->BackupPath(), original,
                               std::filesystem::copy_options::overwrite_existing);
    external_digest = ProjectMaintenancePackage::Digest(original);
    return host.project_launch()->BeginOpen(path);
  };
  ProjectMaintenanceController controller(service, *host.project(), *host.project_launch(),
                                          *host.editor_session(), *host.background_tasks(),
                                          *host.interaction_policy(), nullptr, std::move(hooks));
  const auto                   id = ScanAndSelect(controller);
  ASSERT_NE(id, 0);
  QSignalSpy finished(&controller, &ProjectMaintenanceController::Finished);
  ASSERT_TRUE(controller.ApplyIds({id}));
  ASSERT_TRUE(finished.wait(30000));
  EXPECT_FALSE(finished.front().front().toBool());
  EXPECT_TRUE(controller.Busy());
  EXPECT_EQ(controller.RemovedCount(), 0);
  ASSERT_FALSE(original.empty()) << controller.Error().toStdString();
  EXPECT_EQ(ProjectMaintenancePackage::Digest(original), external_digest);
  EXPECT_FALSE(host.interaction_policy()->CanChangeStateAsUser());
  for (const auto& task : host.background_tasks()->Tasks()) {
    const auto row = task.toMap();
    if (row.value("kind") == QStringLiteral("maintenanceApply"))
      host.background_tasks()->FinishTask(row.value("id").toString(), BackgroundTaskState::Failed);
  }
}
}  // namespace
}  // namespace alcedo::ui
