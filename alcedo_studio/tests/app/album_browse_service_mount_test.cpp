//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The album operations on a mounted folder (mounted_folder_plan.md, MF5): the refusal of
// SleeveServiceImpl reaches the caller as a named result, and the project stays unchanged.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_error.hpp"
#include "app/sleeve_service.hpp"
#include "storage/store/mount/mount_store.hpp"
#include "support/test_temp_directory.hpp"
#include "type/mount_records.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

constexpr mount_id_t kMount = 1;

class AlbumBrowseServiceMountTest : public ::testing::Test {
 protected:
  test::TestTempDirectory         temp_dir_;
  std::filesystem::path           db_path_   = temp_dir_ / "project.db";
  std::filesystem::path           meta_path_ = temp_dir_ / "project.json";
  std::unique_ptr<ProjectService> project_;
  sl_element_id_t                 album_id_         = 0;
  sl_element_id_t                 mount_root_id_    = 0;
  sl_element_id_t                 mounted_file_id_  = 0;
  sl_element_id_t                 ordinary_file_id_ = 0;

  void                            SetUp() override {
    TimeProvider::Refresh();
    project_  = std::make_unique<ProjectService>(db_path_, meta_path_);
    album_id_ = Sleeve().CreateFolder(L"/", L"Album").first->element_id_;
    auto [root, root_sync] = Sleeve().CreateMountRootFolder(0, L"Trip", kMount);
    ASSERT_TRUE(root_sync.success_) << root_sync.message_;
    mount_root_id_ = root->element_id_;
    auto [ids, sync] = Sleeve().Write<std::vector<sl_element_id_t>>([this](FileSystem& fs) {
      auto mounted       = fs.CreateFileInLibrary(L"mounted.nef");
      mounted->image_id_ = mounted->element_id_;
      mounted->mount_id_ = kMount;
      fs.LinkFileToFolder(mounted->element_id_, mount_root_id_);
      auto ordinary       = fs.CreateFileInLibrary(L"ordinary.nef");
      ordinary->image_id_ = ordinary->element_id_;
      return std::vector<sl_element_id_t>{mounted->element_id_, ordinary->element_id_};
    });
    ASSERT_TRUE(sync.success_) << sync.message_;
    mounted_file_id_  = ids[0];
    ordinary_file_id_ = ids[1];

    SourceFileRecord record;
    record.element_id          = mounted_file_id_;
    record.byte_size           = 100;
    record.fingerprint_version = 1;
    project_->GetStorage()->GetMountStore().UpsertSourceFiles(std::vector{record});
  }

  void TearDown() override { project_.reset(); }

  auto Sleeve() -> SleeveServiceImpl& { return *project_->GetSleeveService(); }
  auto Browse() -> AlbumBrowseService& { return *project_->GetAlbumBrowseService(); }

  /// The ids of the live entries of @p folder_path.
  auto EntryIds(const std::filesystem::path& folder_path) -> std::vector<sl_element_id_t> {
    std::vector<sl_element_id_t> ids;
    for (const auto& entry : Sleeve().ListFolderEntries(folder_path)) {
      ids.push_back(entry->element_id_);
    }
    return ids;
  }
};

TEST_F(AlbumBrowseServiceMountTest, DeleteOfAPresentMountedPhotoIsRefused) {
  const auto root_before = EntryIds(L"/");
  const auto trip_before = EntryIds(L"/Trip");

  // Remove from Library (folder 0) of a mounted photo whose file is present.
  const auto from_library =
      Browse().DeleteFilesInFolderByElementIds(0, {ordinary_file_id_, mounted_file_id_});
  EXPECT_EQ(from_library.refusal_, SleeveError::kMountedPhotoIsPresent);
  EXPECT_TRUE(from_library.deleted_files_.empty());
  EXPECT_EQ(from_library.failed_element_ids_,
            (std::vector<sl_element_id_t>{ordinary_file_id_, mounted_file_id_}));
  EXPECT_EQ(Browse().DeleteFilesByElementIds({mounted_file_id_}).refusal_,
            SleeveError::kMountedPhotoIsPresent);

  // Remove from the mount root: the home link of the mounted photo.
  const auto from_mount =
      Browse().DeleteFilesInFolderByElementIds(mount_root_id_, {mounted_file_id_});
  EXPECT_EQ(from_mount.refusal_, SleeveError::kMountedFolderStructureIsReadOnly);
  EXPECT_TRUE(from_mount.deleted_files_.empty());

  EXPECT_EQ(EntryIds(L"/"), root_before);
  EXPECT_EQ(EntryIds(L"/Trip"), trip_before);
}

TEST_F(AlbumBrowseServiceMountTest, LinkAndFolderEditsOfAMountedFolderReturnTheNamedResult) {
  const auto link = Browse().LinkFilesToFolder({ordinary_file_id_}, mount_root_id_);
  EXPECT_EQ(link.refusal_, SleeveError::kMountedFolderStructureIsReadOnly);
  EXPECT_EQ(link.failed_element_ids_, std::vector<sl_element_id_t>{ordinary_file_id_});

  EXPECT_EQ(Browse().RenameFolder(L"/Trip", L"Japan").status_,
            AlbumFolderEditStatus::StructureIsReadOnly);
  EXPECT_EQ(Browse().MoveFolder(L"/Album", L"/Trip").status_,
            AlbumFolderEditStatus::StructureIsReadOnly);
  EXPECT_THROW((void)Browse().CreateFolder(L"/Trip", L"New"), SleeveOperationError);
  EXPECT_THROW((void)Browse().DeleteFolder(L"/Trip"), SleeveOperationError);

  // The mounted photo links into an ordinary album (D8).
  const auto into_album = Browse().LinkFilesToFolder({mounted_file_id_}, album_id_);
  EXPECT_FALSE(into_album.refusal_.has_value());
  EXPECT_EQ(EntryIds(L"/Album"), std::vector<sl_element_id_t>{mounted_file_id_});
  EXPECT_EQ(EntryIds(L"/Trip"), std::vector<sl_element_id_t>{mounted_file_id_});
}

}  // namespace
}  // namespace alcedo
