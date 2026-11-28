// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <chrono>
#include <format>
#include <fstream>
#include <future>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_maintenance_package.hpp"
#include "app/project_maintenance_service.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "support/storage_test_access.hpp"

namespace alcedo {
namespace {
auto PackageTestDirectory() -> QString {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
      "build/tmp/project_maintenance/packages";
  std::filesystem::create_directories(root);
  return QString::fromStdWString((root / "case-XXXXXX").wstring());
}
class ProjectMaintenanceApplyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.isValid());
    root_    = std::filesystem::path(directory_.path().toStdWString());
    project_ = std::make_shared<ProjectService>(root_ / "project.db", root_ / "project.json");
    source_  = root_ / "photo.jpg";
    std::filesystem::copy_file(
        std::filesystem::path(ALCEDO_RASTER_FIXTURE_DIR) / "adobe_rgb_exif_r03_no_icc.jpg",
        source_);
    ImportServiceImpl importer(project_->GetSleeveService(), project_->GetImagePoolService(),
                               std::make_shared<PipelineMgmtService>(project_->GetStorage()));
    auto              job = std::make_shared<ImportJob>();
    std::promise<ImportResult> completion;
    auto                       future = completion.get_future();
    job->on_finished_ = [&completion](const auto& result) { completion.set_value(result); };
    ImportOptions options;
    options.skip_duplicates = false;
    importer.ImportToFolder({source_, source_}, {}, options, job);
    ASSERT_EQ(future.wait_for(std::chrono::seconds(90)), std::future_status::ready);
    ASSERT_EQ(future.get().imported_, 2);
    importer.SyncImports(job->import_log_->Snapshot(), L"");
    ASSERT_TRUE(service_.Scan(project_->GetStorage()->GetProjectMaintenanceStore()));
    for (const auto& finding : service_.Findings())
      if (finding.removable) ids_.push_back(finding.evidence.entry_id);
    ASSERT_EQ(ids_.size(), 1);
    removals_ = service_.Selection(ids_);
    original_ = root_ / "Test.alcd";
    project_->SaveProject(root_ / "project.json");
    QString error;
    ASSERT_TRUE(packages_.CreateLiveDbSnapshot(project_, root_ / "baseline.db", &error))
        << error.toStdString();
    ASSERT_TRUE(packages_.WritePackedProject(original_, root_ / "project.json",
                                             root_ / "baseline.db", &error))
        << error.toStdString();
  }
  auto Count(Database& database, const std::string& table) -> int64_t {
    auto guard = database.GetConnectionGuard();
    auto lock  = guard.Lock();
    return duckorm::select_int64(guard.conn_, duckorm::expr::raw("SELECT count(*) FROM " + table))
        .value();
  }
  auto LiveCount() -> int64_t {
    return Count(StorageTestAccess::GetDatabase(*project_->GetStorage()), "Image");
  }
  QTemporaryDir                   directory_{PackageTestDirectory()};
  image_path_t                    root_, source_, original_;
  std::shared_ptr<ProjectService> project_;
  ProjectPackageService           packages_;
  ProjectMaintenanceService       service_;
  std::vector<sl_element_id_t>    ids_;
  std::vector<MaintenanceRemoval> removals_;
};
TEST_F(ProjectMaintenanceApplyTest, SuccessfulMaintenancePreservesCompleteHistoryAndReopens) {
  {
    auto guard = StorageTestAccess::GetDatabase(*project_->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    duckorm::execute(
        guard.conn_,
        duckorm::expr::raw(std::format(
            "UPDATE Image SET color_labels=5,delete_mark=true WHERE id=(SELECT image_id FROM "
            "FileImage WHERE file_id={})",
            *removals_.front().retained_id)));
  }
  std::vector<size_t>       processed;
  ProjectMaintenancePackage operation({}, [&](size_t completed, size_t total) {
    EXPECT_TRUE(operation.BackupVerified());
    EXPECT_FALSE(operation.Installed());
    EXPECT_EQ(total, 1);
    EXPECT_EQ(LiveCount(), 2);
    processed.push_back(completed);
  });
  operation.Apply(project_, original_, removals_);
  EXPECT_EQ(processed, (std::vector<size_t>{0, 1}));
  EXPECT_TRUE(operation.Installed());
  EXPECT_EQ(operation.RemovedIds(), ids_);
  EXPECT_TRUE(std::filesystem::exists(operation.BackupPath()));
  EXPECT_TRUE(std::filesystem::exists(source_));
  EXPECT_EQ(LiveCount(), 2);  // Live owners are replaced only by the UI lifecycle controller.
  std::filesystem::create_directory(root_ / "reopen");
  image_path_t db, meta;
  QString      error;
  ASSERT_TRUE(packages_.UnpackProjectToWorkspace(original_, root_ / "reopen",
                                                 QStringLiteral("result"), &db, &meta, &error))
      << error.toStdString();
  Storage reopened(db);
  EXPECT_EQ(Count(StorageTestAccess::GetDatabase(reopened), "Image"), 1);
  EXPECT_EQ(Count(StorageTestAccess::GetDatabase(reopened), "PipelineRoot"), 1);
  EXPECT_EQ(
      Count(StorageTestAccess::GetDatabase(reopened), "Image WHERE color_labels=5 AND delete_mark"),
      1);
  ProjectMaintenanceService repeat;
  ASSERT_TRUE(repeat.Scan(reopened.GetProjectMaintenanceStore()));
  EXPECT_TRUE(repeat.Findings().empty());
  operation.Finish();
}
TEST_F(ProjectMaintenanceApplyTest, BackupFailurePerformsNoRemoval) {
  std::string               before;
  ProjectMaintenancePackage operation([&](auto stage) {
    if (stage == MaintenancePackageStage::Backup) {
      before = ProjectMaintenancePackage::Digest(original_);
      throw std::runtime_error("Injected backup failure");
    }
  });
  EXPECT_THROW(operation.Apply(project_, original_, removals_), std::runtime_error);
  EXPECT_FALSE(operation.Installed());
  EXPECT_EQ(LiveCount(), 2);
  EXPECT_EQ(ProjectMaintenancePackage::Digest(original_), before);
}
TEST_F(ProjectMaintenanceApplyTest, StagedWriteFailureLeavesOriginalPackageIntact) {
  std::string               before;
  ProjectMaintenancePackage operation([&](auto stage) {
    if (stage == MaintenancePackageStage::Backup)
      before = ProjectMaintenancePackage::Digest(original_);
    if (stage == MaintenancePackageStage::ResultWrite)
      throw std::runtime_error("Injected package write failure");
  });
  EXPECT_THROW(operation.Apply(project_, original_, removals_), std::runtime_error);
  EXPECT_FALSE(operation.Installed());
  EXPECT_EQ(LiveCount(), 2);
  EXPECT_EQ(ProjectMaintenancePackage::Digest(original_), before);
  EXPECT_EQ(ProjectMaintenancePackage::Digest(operation.BackupPath()), before);
}
TEST_F(ProjectMaintenanceApplyTest, ExistingBackupIsNeverOverwritten) {
  std::filesystem::path first;
  {
    ProjectMaintenancePackage operation([](auto stage) {
      if (stage == MaintenancePackageStage::Removal)
        throw std::runtime_error("Stop after verified backup");
    });
    EXPECT_THROW(operation.Apply(project_, original_, removals_), std::runtime_error);
    first = operation.BackupPath();
  }
  const auto                digest = ProjectMaintenancePackage::Digest(first);
  ProjectMaintenancePackage second([](auto stage) {
    if (stage == MaintenancePackageStage::Removal)
      throw std::runtime_error("Stop after verified backup");
  });
  EXPECT_THROW(second.Apply(project_, original_, removals_), std::runtime_error);
  EXPECT_NE(first, second.BackupPath());
  EXPECT_EQ(ProjectMaintenancePackage::Digest(first), digest);
}
TEST_F(ProjectMaintenanceApplyTest, ChangedBaselineNeverTriggersBackupRestore) {
  std::string               changed;
  ProjectMaintenancePackage operation([&](auto stage) {
    if (stage != MaintenancePackageStage::Publication) return;
    std::ifstream input(root_ / "project.json");
    auto          metadata = nlohmann::json::parse(input);
    input.close();
    metadata["project_uuid"] = "external-writer";
    std::ofstream(root_ / "external.json") << metadata.dump();
    QString error;
    if (!packages_.WritePackedProject(original_, root_ / "external.json", root_ / "baseline.db",
                                      &error))
      throw std::runtime_error(error.toStdString());
    changed = ProjectMaintenancePackage::Digest(original_);
  });
  EXPECT_THROW(operation.Apply(project_, original_, removals_), std::runtime_error);
  ASSERT_FALSE(changed.empty());
  EXPECT_FALSE(operation.Installed());
  EXPECT_THROW(operation.Restore(), std::runtime_error);
  EXPECT_EQ(ProjectMaintenancePackage::Digest(original_), changed);
}
TEST_F(ProjectMaintenanceApplyTest, ActivationFailureRestoresVerifiedBackup) {
  ProjectMaintenancePackage operation;
  operation.Apply(project_, original_, removals_);
  const auto before = ProjectMaintenancePackage::Digest(operation.BackupPath());
  ASSERT_NE(ProjectMaintenancePackage::Digest(original_), before);
  operation.Restore();
  EXPECT_FALSE(operation.Installed());
  EXPECT_EQ(ProjectMaintenancePackage::Digest(original_), before);
  operation.Finish();
}
TEST_F(ProjectMaintenanceApplyTest,
       ExternallyChangedPublishedPackageIsNotOverwrittenDuringRecovery) {
  ProjectMaintenancePackage operation;
  operation.Apply(project_, original_, removals_);
  QString error;
  ASSERT_TRUE(packages_.WritePackedProject(original_, root_ / "project.json", root_ / "baseline.db",
                                           &error));
  const auto external = ProjectMaintenancePackage::Digest(original_);
  EXPECT_THROW(operation.Restore(), std::runtime_error);
  EXPECT_EQ(ProjectMaintenancePackage::Digest(original_), external);
  EXPECT_TRUE(std::filesystem::exists(operation.BackupPath()));
}
TEST_F(ProjectMaintenanceApplyTest, ReviewThenRatingPreventsSelectedRemoval) {
  auto guard = StorageTestAccess::GetDatabase(*project_->GetStorage()).GetConnectionGuard();
  {
    auto lock = guard.Lock();
    duckorm::execute(
        guard.conn_,
        duckorm::expr::raw(std::format(
            "UPDATE Image SET rating=3,metadata=json_merge_patch(metadata,'{{\"Rating\":3}}') "
            "WHERE id=(SELECT image_id FROM FileImage WHERE file_id={})",
            ids_.front())));
  }
  ProjectMaintenancePackage operation;
  EXPECT_THROW(operation.Apply(project_, original_, removals_), std::exception);
  EXPECT_FALSE(operation.Installed());
  EXPECT_EQ(LiveCount(), 2);
}
TEST_F(ProjectMaintenanceApplyTest, ReviewThenColorLabelPreventsSelectedRemoval) {
  auto guard = StorageTestAccess::GetDatabase(*project_->GetStorage()).GetConnectionGuard();
  {
    auto lock = guard.Lock();
    duckorm::execute(guard.conn_, duckorm::expr::raw(std::format(
                                      "UPDATE Image SET color_labels=2 WHERE id=(SELECT image_id "
                                      "FROM FileImage WHERE file_id={})",
                                      ids_.front())));
  }
  ProjectMaintenancePackage operation;
  EXPECT_THROW(operation.Apply(project_, original_, removals_), std::exception);
  EXPECT_FALSE(operation.Installed());
  EXPECT_EQ(LiveCount(), 2);
}
TEST_F(ProjectMaintenanceApplyTest, ReviewThenDeletionMarkPreventsSelectedRemoval) {
  auto guard = StorageTestAccess::GetDatabase(*project_->GetStorage()).GetConnectionGuard();
  {
    auto lock = guard.Lock();
    duckorm::execute(guard.conn_, duckorm::expr::raw(std::format(
                                      "UPDATE Image SET delete_mark=true WHERE id=(SELECT image_id "
                                      "FROM FileImage WHERE file_id={})",
                                      ids_.front())));
  }
  ProjectMaintenancePackage operation;
  EXPECT_THROW(operation.Apply(project_, original_, removals_), std::exception);
  EXPECT_FALSE(operation.Installed());
  EXPECT_EQ(LiveCount(), 2);
}
#ifdef _WIN32
TEST_F(ProjectMaintenanceApplyTest, AtomicReplaceFailureRetainsOriginal) {
  HANDLE                    file = INVALID_HANDLE_VALUE;
  std::string               baseline;
  ProjectMaintenancePackage operation([&](auto stage) {
    if (stage == MaintenancePackageStage::Backup)
      baseline = ProjectMaintenancePackage::Digest(original_);
    if (stage == MaintenancePackageStage::Publication) {
      file = CreateFileW(original_.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (file == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot hold the package against replacement");
    }
  });
  EXPECT_THROW(operation.Apply(project_, original_, removals_), std::exception);
  ASSERT_NE(file, INVALID_HANDLE_VALUE);
  CloseHandle(file);
  EXPECT_FALSE(operation.Installed());
  EXPECT_FALSE(operation.RecoveryRequired());
  EXPECT_EQ(ProjectMaintenancePackage::Digest(original_), baseline);
  EXPECT_EQ(LiveCount(), 2);
}
#endif
}  // namespace
}  // namespace alcedo
