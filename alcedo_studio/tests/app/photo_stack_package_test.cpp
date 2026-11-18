//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QString>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>

#include "app/import_service.hpp"
#include "app/project_package_backend.hpp"
#include "image/color_label.hpp"
#include "image/image.hpp"
#include "json.hpp"
#include "support/non_raw_import_files.hpp"
#include "support/photo_stack_test_library.hpp"
#include "support/storage_invariants.hpp"

namespace alcedo {
namespace {
class PhotoStackPackageTest : public test::PhotoStackProjectTest {};

TEST_F(PhotoStackPackageTest, Version011ProjectGainsEmptyStackTablesAndKeepsRootIds) {
  const auto roots = TextRows("SELECT root_id FROM PipelineRoot ORDER BY element_id");
  const auto image_id =
      project->GetAlbumBrowseService()->ReadAlbumFileRows(Files({0})).at(0).image_id_;
  project->GetImagePoolService()->Write_NoSync<void>(image_id, [](std::shared_ptr<Image> image) {
    image->SetColorLabel(ColorLabel::kGreen, true);
    image->SetDeleteMark(true);
  });
  ASSERT_TRUE(project->GetImagePoolService()->SyncWithStorage().failed_images_.empty());
  project->SaveProject(meta_path);
  Execute("DROP INDEX idx_photo_stack_member_stack");
  Execute("DROP TABLE PhotoStackMember");
  Execute("DROP TABLE PhotoStack");
  project.reset();
  auto metadata = nlohmann::json::parse(std::ifstream(meta_path, std::ios::binary));
  metadata["project_file_version"] = "0.11.0";
  metadata["data_summary"]["tables"].erase("PhotoStack");
  metadata["data_summary"]["tables"].erase("PhotoStackMember");
  {
    std::ofstream out(meta_path, std::ios::binary | std::ios::trunc);
    out << metadata.dump(2);
  }
  project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);
  EXPECT_TRUE(StackRows().empty());
  EXPECT_EQ(TextRows("SELECT root_id FROM PipelineRoot ORDER BY element_id"), roots);
  const auto rows = project->GetAlbumBrowseService()->ReadAlbumFileRows(files);
  ASSERT_EQ(rows.size(), files.size());
  EXPECT_EQ(rows[0].color_labels_, ColorLabelBit(ColorLabel::kGreen));
  EXPECT_TRUE(rows[0].delete_mark_);
  project->SaveProject(meta_path);
  EXPECT_EQ(
      nlohmann::json::parse(std::ifstream(meta_path, std::ios::binary)).at("project_file_version"),
      "0.12.0");
  Reopen();
  EXPECT_TRUE(StackRows().empty());
  test::ExpectStorageInvariants(*project->GetStorage());
}

TEST_F(PhotoStackPackageTest, PackagedProjectReopensWithExactStackMembershipAndCover) {
  const auto first  = Create({2, 0, 1, 3}, 0);
  const auto second = Create({4, 5}, 5);
  ASSERT_TRUE(Stacks().RemoveMembers(Files({1})).Changed());
  ASSERT_TRUE(Stacks().MoveMember(files[3], PhotoStackMove::kEarlier).Changed());
  ASSERT_TRUE(Stacks().SetCover(first, files[3]).Changed());
  const auto before = StackRows();
  const auto roots  = TextRows("SELECT root_id FROM PipelineRoot ORDER BY element_id");
  project->SaveProject(meta_path);
  const auto snapshot = temp / "package-copy.db";
  const auto packed   = temp / "stacked.alcd";
  QString    error;
  ASSERT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshot, &error)) << error.toStdString();
  ASSERT_TRUE(project_pack::WritePackedProject(packed, meta_path, snapshot, &error))
      << error.toStdString();
  std::filesystem::path reopened_db;
  std::filesystem::path reopened_meta;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(
      packed, temp / "unpacked", QStringLiteral("Stacks"), &reopened_db, &reopened_meta, &error))
      << error.toStdString();
  project =
      std::make_shared<ProjectService>(reopened_db, reopened_meta, ProjectOpenMode::kLoadExisting);
  EXPECT_EQ(StackRows(), before);
  EXPECT_EQ(Stacks().ReadMemberIds(first), Files({2, 3, 0}));
  EXPECT_EQ(Cover(first), files[3]);
  EXPECT_EQ(Stacks().ReadMemberIds(second), Files({4, 5}));
  EXPECT_EQ(Cover(second), files[5]);
  EXPECT_EQ(TextRows("SELECT root_id FROM PipelineRoot ORDER BY element_id"), roots);
  test::ExpectStorageInvariants(*project->GetStorage());
}

TEST_F(PhotoStackPackageTest, PackageDatabaseCopyPreservesStackRowsAndFailureKeepsSource) {
  const auto stack       = Create({2, 0, 1}, 0);
  const auto before      = StackRows();
  auto&      maintenance = project->GetStorage()->Maintenance();
  const auto copied      = temp / "copy.db";
  maintenance.WriteSnapshot(copied);
  {
    auto              storage = std::make_shared<Storage>(copied);
    PhotoStackService stacks(storage);
    EXPECT_EQ(stacks.ReadMemberIds(stack), Files({2, 0, 1}));
    EXPECT_FALSE(stacks.SetCover(stack, files[0]).Changed());
  }
  const auto failed_path = temp / "failed-copy.db";
  auto       failed_wal  = failed_path;
  failed_wal += ".wal";
  std::filesystem::create_directories(failed_wal);
  std::ofstream(failed_wal / "keep", std::ios::binary) << "prevent WAL creation";
  EXPECT_THROW(maintenance.WriteSnapshot(failed_path), std::runtime_error);
  EXPECT_EQ(StackRows(), before);
  EXPECT_EQ(Number(duckorm::expr::raw(
                "SELECT COUNT(*) FROM duckdb_databases() WHERE database_name = 'pack_snapshot'")),
            0);
  const auto recovered_path = temp / "recovered-copy.db";
  EXPECT_NO_THROW(maintenance.WriteSnapshot(recovered_path));
  EXPECT_TRUE(std::filesystem::is_regular_file(recovered_path));
  EXPECT_EQ(StackRows(), before);
}

TEST_F(PhotoStackPackageTest, PackageWriteFailureReportsErrorAndKeepsWorkingStackRows) {
  Create({0, 1, 2}, 0);
  const auto before = StackRows();
  project->SaveProject(meta_path);
  const auto snapshot    = temp / "write-source.db";
  const auto destination = temp / "blocked.alcd";
  std::filesystem::create_directory(destination);
  std::ofstream(destination / "keep", std::ios::binary) << "prevent replacing the destination";
  QString error;
  ASSERT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshot, &error)) << error.toStdString();
  EXPECT_FALSE(project_pack::WritePackedProject(destination, meta_path, snapshot, &error));
  EXPECT_FALSE(error.isEmpty());
  EXPECT_TRUE(std::filesystem::is_regular_file(destination / "keep"));
  EXPECT_EQ(StackRows(), before);
  Reopen();
  EXPECT_EQ(StackRows(), before);
}

TEST_F(PhotoStackPackageTest, InvalidSavedCoverStopsProjectOpenWithoutChangingStackRows) {
  const auto stack = Create({0, 1, 2}, 0);
  Execute(std::format("UPDATE PhotoStack SET cover_file_id = {}", files[5]));
  project->SaveProject(meta_path);
  const auto before = StackRows();
  EXPECT_THROW(ProjectService(db_path, meta_path, ProjectOpenMode::kLoadExisting),
               std::runtime_error);
  EXPECT_EQ(StackRows(), before);
  Execute(std::format("UPDATE PhotoStack SET cover_file_id = {}", files[0]));
  Reopen();
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({0, 1, 2}));
}

TEST_F(PhotoStackPackageTest, OrphanMembershipStopsProjectOpenWithoutDiscardingTheRows) {
  const auto stack = Create({0, 1, 2}, 0);
  Execute("DELETE FROM PhotoStack");
  project->SaveProject(meta_path);
  const auto before = StackRows();
  EXPECT_THROW(ProjectService(db_path, meta_path, ProjectOpenMode::kLoadExisting),
               std::runtime_error);
  EXPECT_EQ(StackRows(), before);
  Execute(std::format("INSERT INTO PhotoStack VALUES ('{}', {})", stack, files[0]));
}

TEST_F(PhotoStackPackageTest, ImportAndMetadataWritesPreserveExistingStackMembership) {
  Create({0, 1, 2}, 0);
  const auto before = StackRows();
  const auto image =
      project->GetAlbumBrowseService()->ReadAlbumFileRows(Files({0})).at(0).image_id_;
  project->GetImagePoolService()->Write_NoSync<void>(image, [](std::shared_ptr<Image> value) {
    value->SetColorLabel(ColorLabel::kBlue, true);
    value->SetDeleteMark(true);
  });
  ASSERT_TRUE(project->GetImagePoolService()->SyncWithStorage().failed_images_.empty());
  const auto input = temp / "new-photo.jpg";
  test_support::WriteRgbRaster(input, ".jpg");
  {
    ImportServiceImpl importer(project->GetSleeveService(), project->GetImagePoolService(),
                               std::make_shared<PipelineMgmtService>(project->GetStorage()));
    auto              job = std::make_shared<ImportJob>();
    std::promise<ImportResult> finished;
    auto                       result = finished.get_future();
    job->on_finished_ = [&](const ImportResult& value) { finished.set_value(value); };
    importer.ImportToFolder({input.wstring()}, L"", {}, job);
    ASSERT_EQ(result.wait_for(std::chrono::seconds(60)), std::future_status::ready);
    const auto imported = result.get();
    EXPECT_EQ(imported.imported_, 1);
    EXPECT_EQ(imported.failed_, 0);
    ASSERT_NE(job->import_log_, nullptr);
    importer.SyncImports(job->import_log_->Snapshot(), L"");
  }
  EXPECT_EQ(StackRows(), before);
  Reopen();
  EXPECT_EQ(StackRows(), before);
  test::ExpectStorageInvariants(*project->GetStorage());
}

TEST_F(PhotoStackPackageTest, EmptySavedStackStopsProjectOpenWithoutImplicitDissolution) {
  const auto stack = Create({0, 1}, 0);
  Execute("DELETE FROM PhotoStackMember");
  project->SaveProject(meta_path);
  const auto before = StackRows();
  EXPECT_THROW(ProjectService(db_path, meta_path, ProjectOpenMode::kLoadExisting),
               std::runtime_error);
  EXPECT_EQ(StackRows(), before);
  Execute(std::format("INSERT INTO PhotoStackMember VALUES ({}, '{}', 0), ({}, '{}', 1)", files[0],
                      stack, files[1], stack));
}

TEST_F(PhotoStackPackageTest, MalformedSavedStackIdStopsProjectOpenWithoutChangingMemberships) {
  const auto stack = Create({0, 1}, 0);
  Execute("UPDATE PhotoStack SET stack_id = 'invalid'");
  Execute("UPDATE PhotoStackMember SET stack_id = 'invalid'");
  project->SaveProject(meta_path);
  const auto before = StackRows();
  EXPECT_THROW(ProjectService(db_path, meta_path, ProjectOpenMode::kLoadExisting),
               std::runtime_error);
  EXPECT_EQ(StackRows(), before);
  Execute(std::format("UPDATE PhotoStack SET stack_id = '{}'", stack));
  Execute(std::format("UPDATE PhotoStackMember SET stack_id = '{}'", stack));
}

}  // namespace
}  // namespace alcedo
