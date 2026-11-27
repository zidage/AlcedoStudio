//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Mounted Folder Plan, MF1 (#983): the MountRoot and SourceFile rows and Element.mount_id are
// written, read, updated, and removed through the stores, and read again after the project
// database opens again.

#include "storage/store/mount/mount_store.hpp"

#include <gtest/gtest.h>

#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"
#include "sleeve/storage.hpp"
#include "storage/store/database.hpp"
#include "support/test_temp_directory.hpp"
#include "type/hash_type.hpp"
#include "type/mount_records.hpp"
#include "type/source_file_record.hpp"

namespace alcedo {
namespace {

/// 2026-11-19 12:00:00 UTC.
constexpr std::time_t kScanTime    = 1763553600;
/// 2026-11-20 08:30:15 UTC.
constexpr std::time_t kMissingTime = 1763627415;

auto MakeMountRoot(mount_id_t id, sl_element_id_t root_folder_id) -> MountRootRecord {
  MountRootRecord record;
  record.id              = id;
  record.root_folder_id  = root_folder_id;
  record.volume_identity = "\\\\?\\Volume{4f1c2e9a-0b6d-4c1e-9a7e-2d5b8c3f6a10}\\";
  record.root_path       = std::filesystem::path(L"D:/Photos/2026 \u65e5\u672c");
  record.last_scan_at    = kScanTime;
  return record;
}

auto MakeSourceFile(sl_element_id_t element_id, std::uint64_t byte_size) -> SourceFileRecord {
  SourceFileRecord record;
  record.element_id          = element_id;
  record.byte_size           = byte_size;
  record.modified_ns         = 1763553600123456789;
  record.fingerprint         = Hash128(0x0123456789abcdefULL + element_id, 0xfedcba9876543210ULL);
  record.fingerprint_version = 1;
  record.state               = SourceFileState::kPresent;
  return record;
}

class MountStoreTest : public ::testing::Test {
 protected:
  test::TestTempDirectory  temp_dir_;
  std::filesystem::path    db_path_;
  std::unique_ptr<Storage> storage_;

  void                     SetUp() override {
    db_path_ = temp_dir_ / "project.db";
    storage_ = std::make_unique<Storage>(db_path_);
  }

  void TearDown() override { storage_.reset(); }

  /// Close the project database and open it again.
  void Reopen() {
    storage_.reset();
    storage_ = std::make_unique<Storage>(db_path_);
  }

  auto Mounts() -> MountStore& { return storage_->GetMountStore(); }

  /// A folder and its files, stored through ElementStore with @p mount_id on each element.
  auto AddFolderWithFiles(sl_element_id_t folder_id, std::vector<sl_element_id_t> file_ids,
                          std::optional<mount_id_t> mount_id)
      -> std::vector<std::shared_ptr<SleeveElement>> {
    auto folder       = std::make_shared<SleeveFolder>(folder_id, L"Folder");
    folder->mount_id_ = mount_id;
    std::vector<std::shared_ptr<SleeveElement>> elements{folder};
    for (const auto file_id : file_ids) {
      auto file =
          std::make_shared<SleeveFile>(file_id, L"IMG_" + std::to_wstring(file_id) + L".ARW");
      file->image_id_ = file_id;
      file->mount_id_ = mount_id;
      // A folder holds no file entry; the insert of the new folder writes its pending links.
      file->IncrementRefCount();
      folder->RecordFileLinked(file->element_id_);
      elements.push_back(file);
    }
    storage_->GetElementStore().AddElements(elements);
    return elements;
  }
};

TEST_F(MountStoreTest, MountRootRowSurvivesProjectReopen) {
  const auto with_all_fields = MakeMountRoot(1, 10);
  auto       without_values  = MakeMountRoot(2, 20);
  without_values.volume_identity.clear();
  without_values.last_scan_at.reset();
  Mounts().InsertMountRoot(with_all_fields);
  Mounts().InsertMountRoot(without_values);

  Reopen();

  EXPECT_EQ(Mounts().ReadMountRoots(),
            (std::vector<MountRootRecord>{with_all_fields, without_values}));
}

TEST_F(MountStoreTest, MountRootLocationAndScanTimeUpdatesSurviveProjectReopen) {
  auto record = MakeMountRoot(1, 10);
  Mounts().InsertMountRoot(record);

  record.root_path       = std::filesystem::path(L"E:/Archive/2026 \u65e5\u672c");
  record.volume_identity = "\\\\NAS\\photos";
  Mounts().UpdateMountRootLocation(record.id, record.root_path, record.volume_identity);
  record.last_scan_at = kScanTime + 3600;
  Mounts().UpdateMountLastScanTime(record.id, *record.last_scan_at);
  Reopen();
  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{record});

  // An empty volume identity is stored as NULL and reads empty.
  record.volume_identity.clear();
  Mounts().UpdateMountRootLocation(record.id, record.root_path, record.volume_identity);
  Reopen();
  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{record});
}

// #1076: the last read volume name; an empty name is stored as NULL and reads empty.
TEST_F(MountStoreTest, MountVolumeNameUpdateSurvivesProjectReopen) {
  auto record        = MakeMountRoot(1, 10);
  record.volume_name = "EOS_DIGITAL";
  Mounts().InsertMountRoot(record);
  Reopen();
  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{record});

  record.volume_name = "\u5916\u4ed8\u3051 SSD";
  Mounts().UpdateMountVolumeName(record.id, record.volume_name);
  Reopen();
  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{record});

  record.volume_name.clear();
  Mounts().UpdateMountVolumeName(record.id, record.volume_name);
  Reopen();
  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{record});
  EXPECT_THROW(Mounts().UpdateMountVolumeName(2, "Other"), std::runtime_error);
}

// #1076: a project file written before the volume_name column opens; its mounts read an empty
// name, and the column takes a name after the open.
TEST_F(MountStoreTest, MountRootTableWithoutTheVolumeNameColumnGainsIt) {
  storage_.reset();
  {
    Database db(db_path_);
    auto     guard = db.GetConnectionGuard();
    auto     lock  = guard.Lock();
    for (const char* sql : {"ALTER TABLE MountRoot DROP COLUMN volume_name;",
                            "INSERT INTO MountRoot VALUES (1, 10, 'volume', 'D:/Photos', NULL);"}) {
      duckdb_result result;
      ASSERT_EQ(duckdb_query(guard.conn_, sql, &result), DuckDBSuccess)
          << duckdb_result_error(&result);
      duckdb_destroy_result(&result);
    }
  }
  storage_ = std::make_unique<Storage>(db_path_);

  MountRootRecord expected;
  expected.id              = 1;
  expected.root_folder_id  = 10;
  expected.volume_identity = "volume";
  expected.root_path       = std::filesystem::path(L"D:/Photos");
  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{expected});

  expected.volume_name = "Photos SSD";
  Mounts().UpdateMountVolumeName(1, expected.volume_name);
  Reopen();
  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{expected});
}

TEST_F(MountStoreTest, RemovedMountRootIsAbsentAfterProjectReopen) {
  const auto kept = MakeMountRoot(1, 10);
  Mounts().InsertMountRoot(kept);
  Mounts().InsertMountRoot(MakeMountRoot(2, 20));

  Mounts().RemoveMountRoot(2);
  Reopen();

  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{kept});
}

TEST_F(MountStoreTest, NextMountIdIsOneThenTheLargestStoredIdPlusOne) {
  EXPECT_EQ(Mounts().ReadNextMountId(), 1u);
  Mounts().InsertMountRoot(MakeMountRoot(1, 10));
  Mounts().InsertMountRoot(MakeMountRoot(7, 20));
  EXPECT_EQ(Mounts().ReadNextMountId(), 8u);
  Reopen();
  EXPECT_EQ(Mounts().ReadNextMountId(), 8u);
}

TEST_F(MountStoreTest, MountIdZeroAndAStoredMountIdAreRefused) {
  EXPECT_THROW(Mounts().InsertMountRoot(MakeMountRoot(0, 10)), std::invalid_argument);
  Mounts().InsertMountRoot(MakeMountRoot(1, 10));
  EXPECT_THROW(Mounts().InsertMountRoot(MakeMountRoot(1, 20)), std::runtime_error);
  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{MakeMountRoot(1, 10)});
}

TEST_F(MountStoreTest, UpdateAndRemoveOfAnUnknownMountThrowAndChangeNothing) {
  const auto record = MakeMountRoot(1, 10);
  Mounts().InsertMountRoot(record);

  EXPECT_THROW(Mounts().UpdateMountRootLocation(2, "F:/Other", ""), std::runtime_error);
  EXPECT_THROW(Mounts().UpdateMountLastScanTime(2, kScanTime), std::runtime_error);
  EXPECT_THROW(Mounts().RemoveMountRoot(2), std::runtime_error);
  EXPECT_THROW(Mounts().UpdateMountRootLocation(1, std::filesystem::path{}, ""),
               std::invalid_argument);

  EXPECT_EQ(Mounts().ReadMountRoots(), std::vector<MountRootRecord>{record});
}

TEST_F(MountStoreTest, ElementMountIdSurvivesProjectReopen) {
  AddFolderWithFiles(10, {11, 12}, 1);
  AddFolderWithFiles(20, {21}, std::nullopt);

  Reopen();

  auto& elements = storage_->GetElementStore();
  EXPECT_EQ(elements.GetElementById(10)->mount_id_, std::optional<mount_id_t>{1});
  EXPECT_EQ(elements.GetElementById(11)->mount_id_, std::optional<mount_id_t>{1});
  EXPECT_EQ(elements.GetElementById(20)->mount_id_, std::nullopt);
  EXPECT_EQ(elements.GetElementById(21)->mount_id_, std::nullopt);
  // GetFolderChildren reads the elements with its own projection of the Element columns.
  for (const auto& child : elements.GetFolderChildren(10)) {
    EXPECT_EQ(child->mount_id_, std::optional<mount_id_t>{1}) << child->element_id_;
  }
  ASSERT_EQ(elements.GetFolderChildren(20).size(), 1u);
  EXPECT_EQ(elements.GetFolderChildren(20).front()->mount_id_, std::nullopt);

  // An update writes the column with the other element fields.
  auto folder       = elements.GetElementById(20);
  folder->mount_id_ = 3;
  elements.UpdateElement(folder);
  Reopen();
  EXPECT_EQ(storage_->GetElementStore().GetElementById(20)->mount_id_,
            std::optional<mount_id_t>{3});
}

// #795: the update of a file element wrote its FileImage row with an upsert, which DuckDB refuses
// on a table with no key.
TEST_F(MountStoreTest, FileElementUpdateWritesItsFieldsAndKeepsOneImageBinding) {
  AddFolderWithFiles(10, {11, 12}, std::nullopt);

  auto& elements      = storage_->GetElementStore();
  auto  file          = std::static_pointer_cast<SleeveFile>(elements.GetElementById(11));
  file->element_name_ = L"IMG_0011_moved.ARW";
  file->mount_id_     = 4;
  elements.UpdateElements(std::vector<std::shared_ptr<SleeveElement>>{file});
  Reopen();

  const auto stored =
      std::static_pointer_cast<SleeveFile>(storage_->GetElementStore().GetElementById(11));
  EXPECT_EQ(stored->element_name_, L"IMG_0011_moved.ARW");
  EXPECT_EQ(stored->mount_id_, std::optional<mount_id_t>{4});
  // GetElementById reads the binding with FileMapper::GetFileById and gives the image id 0 unless
  // the file has exactly one FileImage row.
  EXPECT_EQ(stored->image_id_, 11u);
  EXPECT_EQ(std::static_pointer_cast<SleeveFile>(storage_->GetElementStore().GetElementById(12))
                ->image_id_,
            12u);
}

TEST_F(MountStoreTest, SourceFileRecordSurvivesProjectReopenAndUpsertReplacesIt) {
  auto record = MakeSourceFile(11, 25'165'824);
  Mounts().UpsertSourceFiles(std::vector<SourceFileRecord>{record});
  Reopen();
  EXPECT_EQ(Mounts().ReadSourceFile(11), record);
  EXPECT_EQ(Mounts().ReadSourceFile(12), std::nullopt);

  record.byte_size   = 25'166'000;
  record.modified_ns = 1763600000000000000;
  record.fingerprint = Hash128(1, 2);
  Mounts().UpsertSourceFiles(std::vector<SourceFileRecord>{record});
  Reopen();
  EXPECT_EQ(Mounts().ReadSourceFile(11), record);
}

TEST_F(MountStoreTest, SourceFileStateChangeIsStored) {
  Mounts().UpsertSourceFiles(
      std::vector<SourceFileRecord>{MakeSourceFile(11, 100), MakeSourceFile(12, 200)});

  const std::vector<sl_element_id_t> missing_ids{11};
  Mounts().UpdateSourceFileState(missing_ids, SourceFileState::kMissing, kMissingTime);
  Reopen();
  auto missing          = MakeSourceFile(11, 100);
  missing.state         = SourceFileState::kMissing;
  missing.missing_since = kMissingTime;
  EXPECT_EQ(Mounts().ReadSourceFile(11), missing);
  EXPECT_EQ(Mounts().ReadSourceFile(12), MakeSourceFile(12, 200));

  Mounts().UpdateSourceFileState(missing_ids, SourceFileState::kPresent, std::nullopt);
  Reopen();
  EXPECT_EQ(Mounts().ReadSourceFile(11), MakeSourceFile(11, 100));
}

TEST_F(MountStoreTest, SourceFileStateChangeWithAnUnrecordedElementChangesNothing) {
  Mounts().UpsertSourceFiles(std::vector<SourceFileRecord>{MakeSourceFile(11, 100)});

  const std::vector<sl_element_id_t> ids{11, 99};
  EXPECT_THROW(Mounts().UpdateSourceFileState(ids, SourceFileState::kMissing, kMissingTime),
               std::runtime_error);
  EXPECT_EQ(Mounts().ReadSourceFile(11), MakeSourceFile(11, 100));

  const std::vector<sl_element_id_t> recorded{11};
  EXPECT_THROW(Mounts().UpdateSourceFileState(recorded, SourceFileState::kMissing, std::nullopt),
               std::invalid_argument);
  EXPECT_THROW(Mounts().UpdateSourceFileState(recorded, SourceFileState::kPresent, kMissingTime),
               std::invalid_argument);
  EXPECT_EQ(Mounts().ReadSourceFile(11), MakeSourceFile(11, 100));
}

TEST_F(MountStoreTest, InvalidSourceFileRecordsAreRefusedBeforeAnyWrite) {
  auto missing_without_time  = MakeSourceFile(12, 100);
  missing_without_time.state = SourceFileState::kMissing;
  EXPECT_THROW(Mounts().UpsertSourceFiles(
                   std::vector<SourceFileRecord>{MakeSourceFile(11, 100), missing_without_time}),
               std::invalid_argument);
  EXPECT_THROW(Mounts().UpsertSourceFiles(
                   std::vector<SourceFileRecord>{MakeSourceFile(11, 100), MakeSourceFile(11, 5)}),
               std::invalid_argument);
  EXPECT_EQ(Mounts().ReadSourceFile(11), std::nullopt);
}

TEST_F(MountStoreTest, SourceFilesOfAMountAreTheRecordsOfItsElements) {
  AddFolderWithFiles(10, {13, 11}, 1);
  AddFolderWithFiles(20, {21}, 2);
  AddFolderWithFiles(30, {31}, std::nullopt);
  Mounts().UpsertSourceFiles(
      std::vector<SourceFileRecord>{MakeSourceFile(13, 300), MakeSourceFile(21, 2100),
                                    MakeSourceFile(11, 100), MakeSourceFile(31, 3100)});
  Reopen();

  EXPECT_EQ(Mounts().ReadSourceFilesOfMount(1),
            (std::vector<SourceFileRecord>{MakeSourceFile(11, 100), MakeSourceFile(13, 300)}));
  EXPECT_EQ(Mounts().ReadSourceFilesOfMount(2),
            std::vector<SourceFileRecord>{MakeSourceFile(21, 2100)});
  EXPECT_TRUE(Mounts().ReadSourceFilesOfMount(3).empty());
}

TEST_F(MountStoreTest, RemovedElementHasNoSourceFileRow) {
  const auto elements = AddFolderWithFiles(10, {11, 12, 13}, 1);
  Mounts().UpsertSourceFiles(std::vector<SourceFileRecord>{
      MakeSourceFile(11, 100), MakeSourceFile(12, 200), MakeSourceFile(13, 300)});

  // The batch removal of the sleeve sync and the removal of one element.
  storage_->GetElementStore().RemoveElements(
      std::vector<std::shared_ptr<SleeveElement>>{elements[1]});
  storage_->GetElementStore().RemoveElement(elements[2]);
  Reopen();

  EXPECT_EQ(Mounts().ReadSourceFile(11), std::nullopt);
  EXPECT_EQ(Mounts().ReadSourceFile(12), std::nullopt);
  EXPECT_EQ(Mounts().ReadSourceFile(13), MakeSourceFile(13, 300));
}

TEST_F(MountStoreTest, RemoveSourceFilesRemovesOnlyTheNamedRecords) {
  Mounts().UpsertSourceFiles(
      std::vector<SourceFileRecord>{MakeSourceFile(11, 100), MakeSourceFile(12, 200)});

  const std::vector<sl_element_id_t> removed{11, 99};
  Mounts().RemoveSourceFiles(removed);
  Reopen();

  EXPECT_EQ(Mounts().ReadSourceFile(11), std::nullopt);
  EXPECT_EQ(Mounts().ReadSourceFile(12), MakeSourceFile(12, 200));
}

}  // namespace
}  // namespace alcedo
