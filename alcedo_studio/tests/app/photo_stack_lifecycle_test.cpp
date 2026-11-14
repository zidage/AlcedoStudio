//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>

#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "support/photo_stack_test_library.hpp"

namespace alcedo {
namespace {
class PhotoStackLifecycleTest : public test::PhotoStackProjectTest {};

TEST_F(PhotoStackLifecycleTest, DeletingSavedCoverPromotesFirstRemainingMemberInTheSameCommit) {
  const auto stack  = Create({2, 0, 1, 3}, 0);
  const auto album  = Album();
  const auto result = project->GetSleeveService()->DeleteFileEverywhere(files[0]);
  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({2, 1, 3}));
  EXPECT_EQ(Cover(stack), files[2]);
  EXPECT_EQ(Number({"SELECT COUNT(*) FROM Element WHERE id = ?", {std::int64_t(files[0])}}), 0);
  EXPECT_EQ(Number({"SELECT COUNT(*) FROM FileImage WHERE file_id = ?", {std::int64_t(files[0])}}),
            0);
  EXPECT_EQ(
      Number({"SELECT COUNT(*) FROM FolderContent WHERE element_id = ?", {std::int64_t(files[0])}}),
      0);
  Reopen();
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({2, 1, 3}));
  EXPECT_EQ(project->GetStorage()->GetElementStore().GetFolderContent(album).size(), 5);
}

TEST_F(PhotoStackLifecycleTest, DeletingDownToOneMemberDissolvesTheSavedStack) {
  const auto stack = Create({0, 1}, 0);
  const auto album = Album();
  ASSERT_TRUE(project->GetSleeveService()->DeleteElement(files[0]).success_);
  EXPECT_THROW(Stacks().ReadMemberIds(stack), std::invalid_argument);
  EXPECT_EQ(Number(duckorm::expr::raw("SELECT COUNT(*) FROM PhotoStackMember")), 0);
  const auto remaining = project->GetSleeveService()->Read<std::shared_ptr<SleeveElement>>(
      [&](FileSystem& fs) { return fs.Get(files[1]); });
  EXPECT_EQ(remaining->sync_flag_, SyncFlag::SYNCED);
  const auto links = project->GetStorage()->GetElementStore().GetFolderContent(album);
  EXPECT_NE(std::find(links.begin(), links.end(), files[1]), links.end());
}

TEST_F(PhotoStackLifecycleTest, AlbumUnlinkKeepsProjectStackMembership) {
  const auto stack  = Create({0, 1, 2}, 0);
  const auto album  = Album();
  const auto before = StackRows();
  ASSERT_TRUE(project->GetSleeveService()->DeleteFileFromFolder(files[0], album).success_);
  EXPECT_EQ(StackRows(), before);
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({0, 1, 2}));
  Reopen();
  EXPECT_EQ(StackRows(), before);
}

TEST_F(PhotoStackLifecycleTest, FailureAfterFolderUnlinkRestoresFolderFileAndStackRows) {
  const auto stack = Create({0, 1, 2}, 0);
  const auto album = Album();
  Execute(
      "CREATE TABLE ProtectedStackMember (file_id BIGINT REFERENCES PhotoStackMember(file_id))");
  Execute(std::format("INSERT INTO ProtectedStackMember VALUES ({})", files[0]));
  const auto before = OrganizationRows();
  const auto result = project->GetSleeveService()->DeleteFileEverywhere(files[0]);
  EXPECT_FALSE(result.success_);
  EXPECT_EQ(result.elements_synced_, 0);
  EXPECT_NE(result.message_.find("Constraint"), std::string::npos);
  EXPECT_EQ(OrganizationRows(), before);
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({0, 1, 2}));
  const auto live = project->GetSleeveService()->Read<std::vector<sl_element_id_t>>(
      [album](FileSystem& fs) { return fs.ListFolderContent(album); });
  EXPECT_NE(std::find(live.begin(), live.end(), files[0]), live.end());
}

TEST_F(PhotoStackLifecycleTest, FailedSyncRestoresFileSystemAndAllowsExplicitRetry) {
  const auto stack    = Create({0, 1, 2}, 0);
  const auto album    = Album();
  const auto service  = project->GetSleeveService();
  const auto original = service->ResolveFile(L"/stack0.jpg");
  const auto folder   = service->ResolveFolder(L"/Album");
  const auto before   = OrganizationRows();
  Execute("CREATE TABLE ProtectedElement (file_id BIGINT REFERENCES Element(id))");
  Execute(std::format("INSERT INTO ProtectedElement VALUES ({})", files[0]));
  // The final Element delete fails after folder writes, binding removal, and stack repair.
  const auto failed = service->DeletePath(L"/stack0.jpg");
  ASSERT_FALSE(failed.success_);
  EXPECT_EQ(OrganizationRows(), before);
  EXPECT_EQ(service->ResolveFile(L"/stack0.jpg"), original);
  EXPECT_EQ(service->ResolveFolder(L"/Album"), folder);
  EXPECT_EQ(original->sync_flag_, SyncFlag::SYNCED);
  EXPECT_EQ(folder->sync_flag_, SyncFlag::SYNCED);
  EXPECT_TRUE(folder->ContentAddedSinceSync().empty());
  EXPECT_TRUE(folder->ContentRemovedSinceSync().empty());
  EXPECT_EQ(Cover(stack), files[0]);
  Execute("DROP TABLE ProtectedElement");
  const auto retried = service->DeletePath(L"/stack0.jpg");
  ASSERT_TRUE(retried.success_) << retried.message_;
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({1, 2}));
  EXPECT_EQ(Cover(stack), files[1]);
  EXPECT_EQ(project->GetStorage()->GetElementStore().GetFolderContent(album).size(), 5);
}

TEST_F(PhotoStackLifecycleTest, FailedSyncDiscardsOnlyNewEntriesAndDoesNotReuseTheirIds) {
  const auto stack    = Create({0, 1, 2}, 0);
  const auto service  = project->GetSleeveService();
  const auto original = service->ResolveFile(L"/stack0.jpg");
  const auto before   = OrganizationRows();
  const auto start_id = service->GetCurrentID();
  Execute("CREATE TABLE ProtectedElement (file_id BIGINT REFERENCES Element(id))");
  Execute(std::format("INSERT INTO ProtectedElement VALUES ({})", files[0]));
  std::shared_ptr<SleeveElement> created;
  const auto                     failed = service->Write<void>([&](FileSystem& fs) {
    created = fs.Create(L"/", L"Not committed", ElementType::FOLDER);
    fs.DeleteFileEverywhere(files[0]);
  });
  ASSERT_FALSE(failed.success_);
  EXPECT_EQ(OrganizationRows(), before);
  EXPECT_EQ(service->ResolveFile(L"/stack0.jpg"), original);
  EXPECT_THROW(service->ResolveElement(L"/Not committed"), std::runtime_error);
  EXPECT_EQ(created->sync_flag_, SyncFlag::DELETED);
  EXPECT_GT(service->GetCurrentID(), start_id);
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({0, 1, 2}));
  Execute("DROP TABLE ProtectedElement");
  const auto next = service->CreateFolder(L"/", L"Next");
  EXPECT_TRUE(next.second.success_);
  EXPECT_GT(next.first->element_id_, created->element_id_);
}

TEST_F(PhotoStackLifecycleTest, StandaloneElementRemovalUsesTheSameStackRepair) {
  const auto stack = Create({0, 1, 2, 3}, 0);
  Album();
  auto& store = project->GetStorage()->GetElementStore();
  store.RemoveElement(store.GetElementById(files[0]));
  EXPECT_EQ(Cover(stack), files[1]);
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({1, 2, 3}));
  const std::array targets{store.GetElementById(files[1]), store.GetElementById(files[2])};
  store.RemoveElements(targets);
  EXPECT_THROW(Stacks().ReadMemberIds(stack), std::invalid_argument);
  EXPECT_EQ(Number(duckorm::expr::raw("SELECT COUNT(*) FROM PhotoStackMember")), 0);
  EXPECT_EQ(Number({"SELECT COUNT(*) FROM FolderContent WHERE element_id IN (?, ?, ?)",
                    {std::int64_t(files[0]), std::int64_t(files[1]), std::int64_t(files[2])}}),
            0);
}

TEST_F(PhotoStackLifecycleTest, FailedOwnerRestoreReportsBothErrorsAndRequiresProjectReopen) {
  Create({0, 1, 2}, 0);
  project->SaveProject(meta_path);
  Execute("CREATE TABLE SavedFileBindings AS SELECT * FROM FileImage");
  Execute("DROP TABLE FileImage");
  const auto failed = project->GetSleeveService()->DeleteFileEverywhere(files[0]);
  EXPECT_FALSE(failed.success_);
  const std::string separator      = "\nFileSystem restoration failed: ";
  const auto        error_position = failed.message_.find(separator);
  ASSERT_NE(error_position, std::string::npos);
  EXPECT_NE(failed.message_.substr(0, error_position).find("FileImage"), std::string::npos);
  EXPECT_NE(failed.message_.substr(error_position + separator.size()).find("FileImage"),
            std::string::npos);
  EXPECT_THROW(project->GetSleeveService()->ResolveFile(L"/stack0.jpg"), std::runtime_error);
  EXPECT_FALSE(project->GetSleeveService()->Sync().success_);
  Execute("CREATE TABLE FileImage AS SELECT * FROM SavedFileBindings");
  Execute("DROP TABLE SavedFileBindings");
  project.reset();
  project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);
  EXPECT_EQ(project->GetSleeveService()->ResolveFile(L"/stack0.jpg")->element_id_, files[0]);
}

TEST_F(PhotoStackLifecycleTest, FailedAlbumUnlinkRestoresTheSameFolderAndKeepsTheStack) {
  const auto stack  = Create({0, 1, 2}, 0);
  const auto album  = Album();
  const auto folder = project->GetSleeveService()->ResolveFolder(L"/Album");
  const auto before = OrganizationRows();
  Execute(
      "CREATE TABLE ProtectedAlbumLink (folder_id BIGINT, element_id BIGINT, "
      "FOREIGN KEY(folder_id, element_id) REFERENCES FolderContent(folder_id, element_id))");
  Execute(std::format("INSERT INTO ProtectedAlbumLink VALUES ({}, {})", album, files[0]));
  const auto failed = project->GetSleeveService()->DeleteFileFromFolder(files[0], album);
  ASSERT_FALSE(failed.success_);
  EXPECT_EQ(OrganizationRows(), before);
  EXPECT_EQ(project->GetSleeveService()->ResolveFolder(L"/Album"), folder);
  EXPECT_TRUE(folder->ContainsElementId(files[0]));
  EXPECT_TRUE(folder->ContentRemovedSinceSync().empty());
  Execute("DROP TABLE ProtectedAlbumLink");
  EXPECT_TRUE(project->GetSleeveService()->DeleteFileFromFolder(files[0], album).success_);
  EXPECT_EQ(Stacks().ReadMemberIds(stack), Files({0, 1, 2}));
}

TEST_F(PhotoStackLifecycleTest, FailedFolderRenameRestoresNamesPathsAndOwnersBeforeExplicitRetry) {
  Create({0, 1, 2}, 0);
  auto       service = project->GetSleeveService();
  const auto root    = service->ResolveFolder(L"/");
  const auto parent  = service->CreateFolder(L"/", L"Parent").first;
  const auto album   = service->CreateFolder(L"/Parent", L"Album").first;
  const auto child   = service->CreateFolder(L"/Parent/Album", L"Child").first;
  ASSERT_TRUE(service->LinkFileToFolder(files[0], album->element_id_).success_);
  ASSERT_EQ(service->ResolveFolder(L"/Parent/Album/Child"), child);
  const auto before     = OrganizationRows();
  const auto stacks     = StackRows();
  const auto references = album->ref_count_;
  Execute("CREATE TABLE ProtectedRename (id BIGINT REFERENCES Element(id))");
  Execute(std::format("INSERT INTO ProtectedRename VALUES ({})", album->element_id_));
  const auto failed = service->RenameFolder(L"/Parent/Album", L"Renamed");
  ASSERT_FALSE(failed.success_) << failed.message_;
  EXPECT_FALSE(failed.message_.empty());
  EXPECT_EQ(failed.elements_synced_, 0);
  EXPECT_EQ(OrganizationRows(), before);
  EXPECT_EQ(service->ResolveFolder(L"/"), root);
  EXPECT_EQ(service->ResolveFolder(L"/Parent"), parent);
  EXPECT_EQ(service->ResolveFolder(L"/Parent/Album"), album);
  EXPECT_EQ(service->ResolveFolder(L"/Parent/Album/Child"), child);
  EXPECT_EQ(service->ResolveFile(L"/Parent/Album/stack0.jpg")->element_id_, files[0]);
  EXPECT_THROW(service->ResolveFolder(L"/Parent/Renamed"), std::runtime_error);
  EXPECT_FALSE(parent->Contains(L"Renamed"));
  EXPECT_EQ(album->element_name_, L"Album");
  EXPECT_EQ(album->ref_count_, references);
  for (const auto& folder : {root, parent, album}) {
    EXPECT_EQ(folder->sync_flag_, SyncFlag::SYNCED);
    EXPECT_TRUE(folder->ContentAddedSinceSync().empty());
    EXPECT_TRUE(folder->ContentRemovedSinceSync().empty());
  }
  Execute("DROP TABLE ProtectedRename");
  ASSERT_TRUE(service->RenameFolder(L"/Parent/Album", L"Renamed").success_);
  EXPECT_EQ(service->ResolveFolder(L"/Parent/Renamed"), album);
  EXPECT_EQ(service->ResolveFolder(L"/Parent/Renamed/Child"), child);
  EXPECT_THROW(service->ResolveFolder(L"/Parent/Album"), std::runtime_error);
  EXPECT_EQ(StackRows(), stacks);
  service.reset();
  Reopen();
  EXPECT_EQ(project->GetSleeveService()->ResolveFolder(L"/Parent/Renamed")->element_id_,
            album->element_id_);
  EXPECT_EQ(StackRows(), stacks);
}

TEST_F(PhotoStackLifecycleTest, FailedFolderMoveRestoresBothParentsPathsAndOwnersBeforeRetry) {
  Create({0, 1, 2}, 0);
  auto       service     = project->GetSleeveService();
  const auto source      = service->CreateFolder(L"/", L"Source").first;
  const auto destination = service->CreateFolder(L"/", L"Destination").first;
  const auto album       = service->CreateFolder(L"/Source", L"Album").first;
  const auto child       = service->CreateFolder(L"/Source/Album", L"Child").first;
  ASSERT_TRUE(service->LinkFileToFolder(files[0], album->element_id_).success_);
  ASSERT_EQ(service->ResolveFolder(L"/Source/Album/Child"), child);
  const auto before     = OrganizationRows();
  const auto stacks     = StackRows();
  const auto references = album->ref_count_;
  Execute(
      "CREATE TABLE ProtectedMove (folder_id BIGINT, element_id BIGINT, "
      "FOREIGN KEY(folder_id, element_id) REFERENCES FolderContent(folder_id, element_id))");
  Execute(std::format("INSERT INTO ProtectedMove VALUES ({}, {})", source->element_id_,
                      album->element_id_));
  const auto failed = service->MoveFolder(L"/Source/Album", L"/Destination");
  ASSERT_FALSE(failed.success_) << failed.message_;
  EXPECT_FALSE(failed.message_.empty());
  EXPECT_EQ(failed.elements_synced_, 0);
  EXPECT_EQ(OrganizationRows(), before);
  EXPECT_EQ(service->ResolveFolder(L"/Source"), source);
  EXPECT_EQ(service->ResolveFolder(L"/Destination"), destination);
  EXPECT_EQ(service->ResolveFolder(L"/Source/Album"), album);
  EXPECT_EQ(service->ResolveFolder(L"/Source/Album/Child"), child);
  EXPECT_EQ(service->ResolveFile(L"/Source/Album/stack0.jpg")->element_id_, files[0]);
  EXPECT_THROW(service->ResolveFolder(L"/Destination/Album"), std::runtime_error);
  EXPECT_TRUE(source->ContainsElementId(album->element_id_));
  EXPECT_FALSE(destination->ContainsElementId(album->element_id_));
  EXPECT_EQ(album->ref_count_, references);
  for (const auto& folder : {source, destination, album}) {
    EXPECT_EQ(folder->sync_flag_, SyncFlag::SYNCED);
    EXPECT_TRUE(folder->ContentAddedSinceSync().empty());
    EXPECT_TRUE(folder->ContentRemovedSinceSync().empty());
  }
  Execute("DROP TABLE ProtectedMove");
  ASSERT_TRUE(service->MoveFolder(L"/Source/Album", L"/Destination").success_);
  EXPECT_EQ(service->ResolveFolder(L"/Destination/Album"), album);
  EXPECT_EQ(service->ResolveFolder(L"/Destination/Album/Child"), child);
  EXPECT_THROW(service->ResolveFolder(L"/Source/Album"), std::runtime_error);
  EXPECT_EQ(StackRows(), stacks);
  service.reset();
  Reopen();
  EXPECT_EQ(project->GetSleeveService()->ResolveFolder(L"/Destination/Album")->element_id_,
            album->element_id_);
  EXPECT_EQ(StackRows(), stacks);
}

}  // namespace
}  // namespace alcedo
