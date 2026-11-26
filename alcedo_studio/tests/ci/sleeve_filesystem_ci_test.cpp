//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_filesystem.hpp"

namespace alcedo {
namespace {

auto ContainsId(const std::vector<sl_element_id_t>& ids, sl_element_id_t id) -> bool {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

class SleeveFilesystemCiTest : public ::testing::Test {
 protected:
  std::filesystem::path temp_dir_;
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void SetUp() override {
    temp_dir_ = std::filesystem::temp_directory_path() /
                ("alcedo_sleeve_fs_ci_" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(temp_dir_);
    db_path_ = temp_dir_ / "sleeve_ci.db";
    meta_path_ = temp_dir_ / "sleeve_ci.json";
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);
  }
};

TEST_F(SleeveFilesystemCiTest, AlbumMembershipPersistsWithoutDuplicatingLibraryFile) {
  sl_element_id_t album_id = 0;
  sl_element_id_t file_id = 0;

  {
    Storage storage_service{db_path_};
    FileSystem     fs{db_path_, storage_service, 0};
    fs.InitRoot();

    const auto album = fs.Create(L"", L"Album", ElementType::FOLDER);
    const auto file = fs.CreateFileInLibrary(L"Shared.arw");
    ASSERT_NE(album, nullptr);
    ASSERT_NE(file, nullptr);

    album_id = album->element_id_;
    file_id = file->element_id_;
    fs.LinkFileToFolder(file_id, album_id);
    fs.LinkFileToFolder(file_id, album_id);

    fs.SyncToDB();
    fs.WriteSleeveMeta(meta_path_);
  }

  {
    Storage storage_service{db_path_};
    FileSystem     fs{db_path_, storage_service, 0};
    fs.ReadSleeveMeta(meta_path_);
    fs.InitRoot();

    const auto root_ids = fs.ListFolderContent(0);
    const auto album_ids = fs.ListFolderContent(album_id);
    EXPECT_TRUE(ContainsId(root_ids, file_id));
    EXPECT_EQ(album_ids, (std::vector<sl_element_id_t>{file_id}));

    const auto file = fs.Get(file_id);
    ASSERT_NE(file, nullptr);
    EXPECT_EQ(file->element_name_, L"Shared.arw");
    EXPECT_EQ(file->ref_count_, 1u);
  }
}

TEST_F(SleeveFilesystemCiTest, AlbumDeleteUnlinksButRootDeleteRemovesEveryMembership) {
  sl_element_id_t album_id = 0;
  sl_element_id_t file_id = 0;

  {
    Storage storage_service{db_path_};
    FileSystem     fs{db_path_, storage_service, 0};
    fs.InitRoot();

    const auto album = fs.Create(L"", L"Album", ElementType::FOLDER);
    const auto file = fs.CreateFileInLibrary(L"DeleteScope.arw");
    ASSERT_NE(album, nullptr);
    ASSERT_NE(file, nullptr);

    album_id = album->element_id_;
    file_id = file->element_id_;
    fs.LinkFileToFolder(file_id, album_id);

    fs.UnlinkFileFromFolder(file_id, album_id);
    EXPECT_TRUE(ContainsId(fs.ListFolderContent(0), file_id));
    EXPECT_FALSE(ContainsId(fs.ListFolderContent(album_id), file_id));

    fs.LinkFileToFolder(file_id, album_id);
    fs.Delete(file_id);
    EXPECT_FALSE(ContainsId(fs.ListFolderContent(0), file_id));
    EXPECT_FALSE(ContainsId(fs.ListFolderContent(album_id), file_id));
  }
}

}  // namespace
}  // namespace alcedo
