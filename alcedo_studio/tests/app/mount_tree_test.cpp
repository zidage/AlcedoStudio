//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Mounted Folder Plan, MF4 (#986): the sleeve operations of the mount scan. The tree of a mount
// is read, changed, and removed through SleeveServiceImpl, then read again from the project
// database.

#include "app/mount_tree.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "app/sleeve_service.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/storage.hpp"
#include "support/test_temp_directory.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

constexpr mount_id_t kMount = 1;

class MountTreeTest : public ::testing::Test {
 protected:
  test::TestTempDirectory            temp_dir_;
  std::filesystem::path              db_path_;
  std::shared_ptr<Storage>           storage_;
  std::shared_ptr<SleeveServiceImpl> sleeve_;
  sl_element_id_t                    next_id_ = 0;

  void                               SetUp() override {
    TimeProvider::Refresh();
    db_path_ = temp_dir_ / "project.db";
    Open();
  }

  void TearDown() override {
    sleeve_.reset();
    storage_.reset();
  }

  void Open() {
    storage_ = std::make_shared<Storage>(db_path_);
    sleeve_  = std::make_shared<SleeveServiceImpl>(storage_, db_path_, next_id_);
  }

  /// Close the project database and read the tree from it again.
  void Reopen() {
    ASSERT_TRUE(sleeve_->Sync().success_);
    next_id_ = sleeve_->GetCurrentID();
    sleeve_.reset();
    storage_.reset();
    Open();
  }

  auto MountRoot() -> sl_element_id_t {
    auto [folder, sync] = sleeve_->CreateMountRootFolder(0, L"Trip", kMount);
    EXPECT_TRUE(sync.success_) << sync.message_;
    return folder->element_id_;
  }

  /// A folder in @p parent_id with @p mount_id.
  auto AddFolder(sl_element_id_t parent_id, const std::wstring& name,
                 std::optional<mount_id_t> mount_id) -> sl_element_id_t {
    auto [id, sync] = sleeve_->Write<sl_element_id_t>([&](FileSystem& fs) {
      auto folder       = fs.CreateFolderIn(parent_id, name);
      folder->mount_id_ = mount_id;
      return folder->element_id_;
    });
    EXPECT_TRUE(sync.success_) << sync.message_;
    return id;
  }

  /// A file of the library with @p mount_id, linked into @p folder_id (0: the library only).
  auto AddFile(const std::wstring& name, sl_element_id_t folder_id,
               std::optional<mount_id_t> mount_id) -> sl_element_id_t {
    auto [id, sync] = sleeve_->Write<sl_element_id_t>([&](FileSystem& fs) {
      auto file       = fs.CreateFileInLibrary(name);
      file->image_id_ = file->element_id_;
      file->mount_id_ = mount_id;
      if (folder_id != 0) {
        fs.LinkFileToFolder(file->element_id_, folder_id);
      }
      return file->element_id_;
    });
    EXPECT_TRUE(sync.success_) << sync.message_;
    return id;
  }

  void Link(sl_element_id_t file_id, sl_element_id_t folder_id) {
    ASSERT_TRUE(sleeve_->LinkFileToFolder(file_id, folder_id).success_);
  }

  auto Element(sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
    return sleeve_->Read<std::shared_ptr<SleeveElement>>(
        [id](FileSystem& fs) { return fs.Get(id); });
  }

  /// The children of a folder by name.
  auto Children(sl_element_id_t folder_id) -> std::map<std::wstring, sl_element_id_t> {
    return sleeve_->Read<std::map<std::wstring, sl_element_id_t>>([folder_id](FileSystem& fs) {
      std::map<std::wstring, sl_element_id_t> children;
      for (const auto id : fs.ListFolderContent(folder_id)) {
        children.emplace(fs.Get(id)->element_name_, id);
      }
      return children;
    });
  }

  auto FolderPaths(sl_element_id_t root_id) -> std::vector<std::wstring> {
    std::vector<std::wstring> paths;
    for (const auto& folder : sleeve_->ReadMountTree(root_id, kMount).folders) {
      paths.push_back(folder.relative_path);
    }
    return paths;
  }

  auto HomeFolders(sl_element_id_t root_id, sl_element_id_t file_id)
      -> std::vector<sl_element_id_t> {
    const auto tree = sleeve_->ReadMountTree(root_id, kMount);
    const auto it   = tree.home_folders.find(file_id);
    return it == tree.home_folders.end() ? std::vector<sl_element_id_t>{} : it->second;
  }

  auto FolderAt(sl_element_id_t root_id, const std::wstring& path) -> sl_element_id_t {
    for (const auto& folder : sleeve_->ReadMountTree(root_id, kMount).folders) {
      if (folder.relative_path == path) {
        return folder.folder_id;
      }
    }
    ADD_FAILURE() << "no folder at the path";
    return 0;
  }
};

TEST_F(MountTreeTest, ReadMountTreeListsTheMountedFoldersAndHomeLinksOnly) {
  const auto root     = MountRoot();
  const auto day1     = AddFolder(root, L"Day1", kMount);
  const auto morning  = AddFolder(day1, L"Morning", kMount);
  const auto notes    = AddFolder(root, L"Notes", std::nullopt);
  const auto a        = AddFile(L"a.jpg", root, kMount);
  const auto b        = AddFile(L"b.jpg", day1, kMount);
  const auto ordinary = AddFile(L"ordinary.jpg", day1, std::nullopt);
  AddFile(L"note.jpg", notes, kMount);

  const auto check = [&] {
    const auto tree = sleeve_->ReadMountTree(root, kMount);
    EXPECT_EQ(tree.folders,
              (std::vector<MountTreeFolder>{{L"Day1", day1}, {L"Day1/Morning", morning}}));
    EXPECT_EQ(tree.home_folders.size(), 2u);
    EXPECT_EQ(tree.home_folders.at(a), std::vector<sl_element_id_t>{root});
    EXPECT_EQ(tree.home_folders.at(b), std::vector<sl_element_id_t>{day1});
    EXPECT_FALSE(tree.home_folders.contains(ordinary));
  };
  check();
  Reopen();
  check();
  EXPECT_EQ(Element(root)->mount_id_, std::optional<mount_id_t>{kMount});
  EXPECT_THROW(sleeve_->ReadMountTree(notes, kMount), std::runtime_error);
}

TEST_F(MountTreeTest, AppliedChangesAddPlaceAndRemoveFoldersAndSurviveReopen) {
  const auto root       = MountRoot();
  const auto day1       = AddFolder(root, L"Day1", kMount);
  const auto collection = AddFolder(0, L"Trips", std::nullopt);
  const auto b          = AddFile(L"b.jpg", day1, kMount);
  const auto c          = AddFile(L"c.jpg", day1, kMount);
  Link(c, collection);

  MountTreeChanges changes;
  changes.mount_id          = kMount;
  changes.root_folder_id    = root;
  changes.folders_to_add    = {L"Day One", L"Day One/Morning"};
  changes.placements        = {{b, L"Day One", L"b.jpg"}, {c, L"Day One/Morning", L"c2.jpg"}};
  changes.folders_to_remove = {L"Day1"};
  const auto sync           = sleeve_->ApplyMountTreeChanges(changes);
  ASSERT_TRUE(sync.success_) << sync.message_;

  const auto check = [&] {
    EXPECT_EQ(FolderPaths(root), (std::vector<std::wstring>{L"Day One", L"Day One/Morning"}));
    EXPECT_EQ(HomeFolders(root, b), std::vector<sl_element_id_t>{FolderAt(root, L"Day One")});
    EXPECT_EQ(HomeFolders(root, c),
              std::vector<sl_element_id_t>{FolderAt(root, L"Day One/Morning")});
    EXPECT_EQ(Element(c)->element_name_, L"c2.jpg");
    EXPECT_EQ(Element(FolderAt(root, L"Day One"))->mount_id_, std::optional<mount_id_t>{kMount});
    // The ordinary collection keeps the link, under the new name.
    EXPECT_EQ(Children(collection), (std::map<std::wstring, sl_element_id_t>{{L"c2.jpg", c}}));
    EXPECT_EQ(Children(0).at(L"c2.jpg"), c);
  };
  check();
  Reopen();
  check();
  EXPECT_THROW(Element(day1), std::exception);
}

TEST_F(MountTreeTest, PlacedFileTakesAFreeNameWhenTheLibraryHoldsItsName) {
  const auto       root     = MountRoot();
  const auto       day2     = AddFolder(root, L"Day2", kMount);
  const auto       ordinary = AddFile(L"a.jpg", 0, std::nullopt);
  const auto       mounted  = AddFile(L"x.jpg", root, kMount);

  MountTreeChanges changes{.mount_id = kMount, .root_folder_id = root};
  changes.placements = {{mounted, L"Day2", L"a.jpg"}};
  ASSERT_TRUE(sleeve_->ApplyMountTreeChanges(changes).success_);
  Reopen();

  EXPECT_EQ(Element(mounted)->element_name_, L"a.jpg@");
  EXPECT_EQ(Element(ordinary)->element_name_, L"a.jpg");
  EXPECT_EQ(Children(day2), (std::map<std::wstring, sl_element_id_t>{{L"a.jpg@", mounted}}));
  EXPECT_EQ(HomeFolders(root, mounted), std::vector<sl_element_id_t>{day2});

  // A placement with the name that the file already holds keeps it.
  changes.placements = {{mounted, L"Day2", L"a.jpg"}};
  ASSERT_TRUE(sleeve_->ApplyMountTreeChanges(changes).success_);
  EXPECT_EQ(Element(mounted)->element_name_, L"a.jpg@");
}

TEST_F(MountTreeTest, NewFolderTakesItsDirectoryNameFromAFileThatHeldIt) {
  const auto root = MountRoot();
  AddFile(L"a.jpg", 0, std::nullopt);
  const auto       mounted = AddFile(L"x.jpg", root, kMount);
  MountTreeChanges changes{.mount_id = kMount, .root_folder_id = root};
  changes.placements = {{mounted, L"", L"a.jpg"}};
  ASSERT_TRUE(sleeve_->ApplyMountTreeChanges(changes).success_);
  ASSERT_EQ(Element(mounted)->element_name_, L"a.jpg@");

  // A directory named like the free name of the file.
  MountTreeChanges folder_changes{.mount_id = kMount, .root_folder_id = root};
  folder_changes.folders_to_add = {L"a.jpg@"};
  ASSERT_TRUE(sleeve_->ApplyMountTreeChanges(folder_changes).success_);
  Reopen();

  EXPECT_EQ(FolderPaths(root), std::vector<std::wstring>{L"a.jpg@"});
  EXPECT_EQ(Element(mounted)->element_name_, L"a.jpg@@");
  EXPECT_EQ(HomeFolders(root, mounted), std::vector<sl_element_id_t>{root});
}

TEST_F(MountTreeTest, PlacementGivesAnAdoptedFileTheMountIdAndOneHomeLink) {
  const auto root       = MountRoot();
  const auto day1       = AddFolder(root, L"Day1", kMount);
  const auto day2       = AddFolder(root, L"Day2", kMount);
  const auto collection = AddFolder(0, L"Trips", std::nullopt);
  const auto adopted    = AddFile(L"b.jpg", collection, std::nullopt);
  const auto twice      = AddFile(L"t.jpg", day1, kMount);
  Link(twice, day2);

  MountTreeChanges changes{.mount_id = kMount, .root_folder_id = root};
  changes.placements = {{adopted, L"Day1", L"b.jpg"}, {twice, L"Day2", L"t.jpg"}};
  ASSERT_TRUE(sleeve_->ApplyMountTreeChanges(changes).success_);
  Reopen();

  EXPECT_EQ(Element(adopted)->mount_id_, std::optional<mount_id_t>{kMount});
  EXPECT_EQ(HomeFolders(root, adopted), std::vector<sl_element_id_t>{day1});
  EXPECT_EQ(Children(collection).at(L"b.jpg"), adopted);
  EXPECT_EQ(HomeFolders(root, twice), std::vector<sl_element_id_t>{day2});
}

TEST_F(MountTreeTest, RemovedFolderGivesItsRemainingFileToTheParent) {
  const auto       root    = MountRoot();
  const auto       day1    = AddFolder(root, L"Day1", kMount);
  const auto       morning = AddFolder(day1, L"Morning", kMount);
  const auto       file    = AddFile(L"m.jpg", morning, kMount);

  MountTreeChanges changes{.mount_id = kMount, .root_folder_id = root};
  changes.folders_to_remove = {L"Day1/Morning", L"Day1"};
  ASSERT_TRUE(sleeve_->ApplyMountTreeChanges(changes).success_);
  Reopen();

  EXPECT_TRUE(FolderPaths(root).empty());
  EXPECT_EQ(HomeFolders(root, file), std::vector<sl_element_id_t>{root});
  EXPECT_EQ(Children(0).at(L"m.jpg"), file);
}

TEST_F(MountTreeTest, MissingDirectoryOfAPlacementThrows) {
  const auto       root = MountRoot();
  const auto       file = AddFile(L"m.jpg", root, kMount);
  MountTreeChanges changes{.mount_id = kMount, .root_folder_id = root};
  changes.placements = {{file, L"Absent", L"m.jpg"}};
  EXPECT_THROW(sleeve_->ApplyMountTreeChanges(changes), std::runtime_error);
}

TEST_F(MountTreeTest, MountRootFolderTakesAFreeNameInItsParent) {
  AddFolder(0, L"Trip", std::nullopt);
  const auto root = MountRoot();
  Reopen();

  EXPECT_EQ(Element(root)->element_name_, L"Trip@");
  EXPECT_EQ(Element(root)->mount_id_, std::optional<mount_id_t>{kMount});
  EXPECT_EQ(Children(0).at(L"Trip@"), root);
}

TEST_F(MountTreeTest, RemovedMountTreeKeepsItsFilesAsOrdinaryFiles) {
  const auto root       = MountRoot();
  const auto day1       = AddFolder(root, L"Day1", kMount);
  const auto collection = AddFolder(0, L"Trips", std::nullopt);
  const auto a          = AddFile(L"a.jpg", root, kMount);
  const auto b          = AddFile(L"b.jpg", day1, kMount);
  Link(b, collection);

  const std::vector<sl_element_id_t> files{a, b};
  ASSERT_TRUE(sleeve_->RemoveMountTree(root, kMount, files).success_);
  Reopen();

  EXPECT_THROW(Element(root), std::exception);
  EXPECT_THROW(Element(day1), std::exception);
  EXPECT_EQ(Element(a)->mount_id_, std::nullopt);
  EXPECT_EQ(Element(b)->mount_id_, std::nullopt);
  EXPECT_EQ(Children(collection).at(L"b.jpg"), b);
  EXPECT_FALSE(Children(0).contains(L"Trip"));

  // A removal that stopped before the mount record went: the files convert, nothing throws.
  const auto again = AddFile(L"c.jpg", 0, kMount);
  ASSERT_TRUE(sleeve_->RemoveMountTree(root, kMount, std::vector<sl_element_id_t>{again}).success_);
  EXPECT_EQ(Element(again)->mount_id_, std::nullopt);
}

}  // namespace
}  // namespace alcedo
