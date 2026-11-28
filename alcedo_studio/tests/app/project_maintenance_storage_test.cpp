// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "../support/project_maintenance_fixture.hpp"
#include "storage/store/image/image_store.hpp"
#include "storage/store/mask_asset/mask_asset_store.hpp"

namespace alcedo {
namespace {
using ProjectMaintenanceStorageTest = test::MaintenanceStorageFixture;

TEST_F(ProjectMaintenanceStorageTest, RemovalProgressCountsProcessedEntriesAndKeepsSources) {
  Add(1);
  Add(2);
  Add(3);
  std::vector<std::pair<size_t, size_t>> counts;
  const std::vector<MaintenanceRemoval>  removals{{2, 1}, {3, 1}};
  const auto                             removed =
      ProjectMaintenanceStore(*database_)
          .RemovePristineProjectEntries(removals, [&](size_t completed, size_t total) {
            counts.emplace_back(completed, total);
          });
  EXPECT_EQ(counts, (std::vector<std::pair<size_t, size_t>>{{0, 2}, {1, 2}, {2, 2}}));
  EXPECT_EQ(removed, (std::vector<sl_element_id_t>{2, 3}));
  EXPECT_EQ(Rows("FileImage"), "[{\"file_id\":1,\"image_id\":1}]");
  EXPECT_TRUE(std::filesystem::exists(source_));
  EXPECT_FALSE(SourceFileStore(*database_).ReadSourceFile(2));
}
TEST_F(ProjectMaintenanceStorageTest, MountedSelectionAndChangedFingerprintRejectRemoval) {
  Add(1);
  Add(2);
  const std::vector<MaintenanceRemoval> removals{{2, 1}};
  Execute("UPDATE Element SET mount_id=9 WHERE id=2");
  EXPECT_THROW(store_->RemovePristineProjectEntries(removals), std::invalid_argument);
  Execute("UPDATE Element SET mount_id=NULL WHERE id=2");
  std::ofstream(source_) << "replacement photo";
  EXPECT_THROW(store_->RemovePristineProjectEntries(removals), std::runtime_error);
  EXPECT_TRUE(SourceFileStore(*database_).ReadSourceFile(2));
}
TEST_F(ProjectMaintenanceStorageTest, ProgressObserverFailureRollsBackProcessedEntries) {
  Add(1);
  Add(2);
  Add(3);
  const auto                            before  = Rows("FileImage");
  const auto                            history = Rows("PipelineRoot");
  const std::vector<MaintenanceRemoval> removals{{2, 1}, {3, 1}};
  EXPECT_THROW(
      ProjectMaintenanceStore(*database_)
          .RemovePristineProjectEntries(removals,
                                        [](size_t completed, size_t) {
                                          if (completed == 1)
                                            throw std::runtime_error("Progress observer failed");
                                        }),
      std::runtime_error);
  EXPECT_EQ(Rows("FileImage"), before);
  EXPECT_EQ(Rows("PipelineRoot"), history);
}
TEST_F(ProjectMaintenanceStorageTest, SelectedAlbumDoesNotLimitProjectRemoval) {
  Add(1);
  Add(2);
  Add(3, root_ / "missing.raw");
  Execute(
      "INSERT INTO Element(id,type,element_name,added_time,modified_time,ref_count) VALUES "
      "(100,1,'album',NULL,NULL,0);"
      "INSERT INTO FolderContent VALUES (100,2)");
  const std::vector<MaintenanceRemoval> removals{{2, 1}, {3, {}}};
  const auto removed = ProjectMaintenanceStore(*database_).RemovePristineProjectEntries(removals);
  EXPECT_EQ(removed, (std::vector<sl_element_id_t>{2, 3}));
  EXPECT_EQ(Rows("FileImage"), "[{\"file_id\":1,\"image_id\":1}]");
  EXPECT_EQ(Rows("FolderContent"), "[{\"folder_id\":100,\"element_id\":1}]");
  EXPECT_TRUE(std::filesystem::exists(source_));
}
TEST_F(ProjectMaintenanceStorageTest, DuplicateRemovalTransfersMembershipsWithoutDuplicates) {
  Add(1);
  Add(2);
  Execute(
      "INSERT INTO Element(id,type,element_name,added_time,modified_time,ref_count) VALUES "
      "(100,1,'first',NULL,NULL,0),(101,1,'second',NULL,NULL,0);"
      "INSERT INTO FolderContent VALUES (100,1),(100,2),(101,2)");
  ProjectMaintenanceStore               store(*database_);
  const std::vector<MaintenanceRemoval> removals{{2, 1}};
  const auto                            images   = store.ImageIds(removals);
  const auto                            expected = store.RetainedRowDigests(removals, images, true);
  store.RemovePristineProjectEntries(removals);
  EXPECT_EQ(store.RetainedRowDigests(removals, images, false), expected);
  EXPECT_EQ(Rows("FolderContent"),
            "[{\"folder_id\":100,\"element_id\":1},{\"folder_id\":101,\"element_id\":1}]");
}
TEST_F(ProjectMaintenanceStorageTest, RatedOrEditedSelectionRejectsWholeBatch) {
  Add(1);
  Add(2);
  Add(3);
  Execute("UPDATE VersionRef SET display_name='Keep' WHERE element_id=3");
  const auto                            before = Rows("Element");
  const std::vector<MaintenanceRemoval> removals{{2, 1}, {3, 1}};
  EXPECT_THROW(ProjectMaintenanceStore(*database_).RemovePristineProjectEntries(removals),
               std::runtime_error);
  EXPECT_EQ(Rows("Element"), before);
}
TEST_F(ProjectMaintenanceStorageTest, RemovalChainsCyclesAndRepeatedIdsChangeNothing) {
  Add(1);
  Add(2);
  Add(3);
  const auto before = Rows("Element");
  for (const auto& removals : std::vector<std::vector<MaintenanceRemoval>>{
           {{1, 2}, {2, 3}}, {{1, 2}, {2, 1}}, {{2, 1}, {2, 1}}, {{2, 99}}}) {
    EXPECT_THROW(ProjectMaintenanceStore(*database_).RemovePristineProjectEntries(removals),
                 std::exception);
    EXPECT_EQ(Rows("Element"), before);
  }
}
TEST_F(ProjectMaintenanceStorageTest, BrokenRetainedReferenceRejectsWholeBatch) {
  Add(1);
  Add(2);
  Add(3);
  Execute("DELETE FROM PipelineRoot WHERE element_id=1");
  const auto                            before = Rows("Element");
  const std::vector<MaintenanceRemoval> removals{{2, 1}, {3, 1}};
  EXPECT_THROW(ProjectMaintenanceStore(*database_).RemovePristineProjectEntries(removals),
               std::exception);
  EXPECT_EQ(Rows("Element"), before);
  ASSERT_TRUE(service_.Scan(*store_));
  EXPECT_FALSE(Find(1).removable);
  EXPECT_FALSE(Find(2).removable);
  EXPECT_TRUE(Find(3).removable);
  EXPECT_EQ(Find(3).retained_id, 2);
}
TEST_F(ProjectMaintenanceStorageTest, MarkAddedAfterReviewRejectsTheWholeRemovalBatch) {
  Add(1);
  Add(2);
  Add(3);
  ASSERT_TRUE(service_.Scan(*store_));
  const std::vector<sl_element_id_t> ids{3, 2};
  const auto                         removals = service_.Selection(ids);
  for (const auto* mark : {"color_labels=4", "color_labels=0,delete_mark=true"}) {
    Execute(std::string("UPDATE Image SET ") + mark + " WHERE id=2");
    const auto images  = Rows("Image");
    const auto entries = Rows("Element");
    const auto roots   = Rows("PipelineRoot");
    EXPECT_THROW(ProjectMaintenanceStore(*database_).RemovePristineProjectEntries(removals),
                 std::exception);
    EXPECT_EQ(Rows("Image"), images);
    EXPECT_EQ(Rows("Element"), entries);
    EXPECT_EQ(Rows("PipelineRoot"), roots);
  }
}
TEST_F(ProjectMaintenanceStorageTest, InjectedOwnerFailureRollsBackAllRelatedTables) {
  Add(1);
  Add(2);
  Execute(
      "INSERT INTO Element(id,type,element_name,added_time,modified_time,ref_count) VALUES "
      "(100,1,'album',NULL,NULL,0);"
      "INSERT INTO FolderContent VALUES(100,2);"
      "CREATE TABLE HeldEntry(id BIGINT REFERENCES Element(id)); INSERT INTO HeldEntry VALUES(2)");
  std::vector<std::string> tables{"Element",       "Image",      "FileImage",      "PipelineRoot",
                                  "PipelineParam", "VersionRef", "ImageEditState", "FolderContent"};
  std::vector<std::string> before;
  for (const auto& table : tables) before.push_back(Rows(table));
  const std::vector<MaintenanceRemoval> removals{{2, 1}};
  EXPECT_THROW(ProjectMaintenanceStore(*database_).RemovePristineProjectEntries(removals),
               std::runtime_error);
  for (size_t index = 0; index < tables.size(); ++index)
    EXPECT_EQ(Rows(tables[index]), before[index]) << tables[index];
}
TEST_F(ProjectMaintenanceStorageTest,
       MissingEntryRemovalRequiresPristineProofAndCurrentMissingSource) {
  Add(1);
  const std::vector<MaintenanceRemoval> removals{{1, {}}};
  EXPECT_THROW(ProjectMaintenanceStore(*database_).RemovePristineProjectEntries(removals),
               std::runtime_error);
  std::filesystem::remove(source_);
  EXPECT_EQ(ProjectMaintenanceStore(*database_).RemovePristineProjectEntries(removals).size(), 1);
  EXPECT_EQ(Rows("Image"), "");
}
TEST_F(ProjectMaintenanceStorageTest, LastImageReferenceControlsImageRemoval) {
  Add(1);
  Add(2);
  Execute("UPDATE FileImage SET image_id=1 WHERE file_id=2");
  auto             guard = database_->GetConnectionGuard();
  auto             lock  = guard.Lock();
  const image_id_t image = 1;
  ImageStore::RemoveUnreferencedImagesInTransaction(guard.conn_, std::span(&image, 1));
  EXPECT_EQ(duckorm::select_int64(guard.conn_,
                                  duckorm::expr::raw("SELECT count(*) FROM Image WHERE id=1")),
            1);
  duckorm::execute(guard.conn_, duckorm::expr::raw("DELETE FROM FileImage WHERE image_id=1"));
  ImageStore::RemoveUnreferencedImagesInTransaction(guard.conn_, std::span(&image, 1));
  EXPECT_EQ(duckorm::select_int64(guard.conn_,
                                  duckorm::expr::raw("SELECT count(*) FROM Image WHERE id=1")),
            0);
}
TEST_F(ProjectMaintenanceStorageTest, RetainedHistoryAndMasksMatchBeforeRemoval) {
  Add(1);
  Add(2);
  Add(3);
  Execute("UPDATE VersionRef SET display_name='Protected work' WHERE element_id=3");
  {
    auto      guard = database_->GetConnectionGuard();
    auto      lock  = guard.Lock();
    MaskAsset asset;
    asset.descriptor = {{2, 2}, {0, 0, 1, 1}};
    asset.pixels     = {0, 64, 128, 255};
    asset.key        = MakeMaskAssetKey(asset.descriptor, asset.pixels);
    MaskAssetStore(guard.conn_).Put(asset);
  }
  ProjectMaintenanceStore               store(*database_);
  const std::vector<MaintenanceRemoval> removals{{2, 1}};
  const auto                            images = store.ImageIds(removals);
  const auto                            before = store.RetainedRowDigests(removals, images, true);
  store.RemovePristineProjectEntries(removals);
  EXPECT_EQ(before, store.RetainedRowDigests(removals, images, false));
  EXPECT_FALSE(store.Inspect(3).protection_reasons.empty());
}
}  // namespace
}  // namespace alcedo
