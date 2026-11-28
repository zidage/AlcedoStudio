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
#include "storage/store/sleeve/folder_entry_store.hpp"
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
    auto [folder, sync] = sleeve_->CreateMountRootFolder(L"Trip", kMount);
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

  /// Link through the tree, as a scan does: SleeveServiceImpl::LinkFileToFolder refuses a
  /// mounted folder (MF5).
  void Link(sl_element_id_t file_id, sl_element_id_t folder_id) {
    const auto sync = sleeve_->Write<void>(
        [file_id, folder_id](FileSystem& fs) { fs.LinkFileToFolder(file_id, folder_id); });
    ASSERT_TRUE(sync.success_) << sync.message_;
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

  /// @p count files of the library with @p mount_id in @p folder_id, created in one write.
  auto AddFiles(const std::wstring& prefix, int count, sl_element_id_t folder_id,
                std::optional<mount_id_t> mount_id) -> std::vector<sl_element_id_t> {
    auto [ids, sync] = sleeve_->Write<std::vector<sl_element_id_t>>([&](FileSystem& fs) {
      std::vector<sl_element_id_t> created;
      for (int i = 0; i < count; ++i) {
        auto file       = fs.CreateFileInLibrary(prefix + std::to_wstring(i) + L".jpg");
        file->image_id_ = file->element_id_;
        file->mount_id_ = mount_id;
        fs.LinkFileToFolder(file->element_id_, folder_id);
        created.push_back(file->element_id_);
      }
      return created;
    });
    EXPECT_TRUE(sync.success_) << sync.message_;
    return ids;
  }

  /// The number of elements in the node cache.
  auto CachedCount() -> std::size_t {
    return sleeve_->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().Size(); });
  }

  /// True when the node cache holds @p id.
  auto IsCached(sl_element_id_t id) -> bool {
    return sleeve_->Read<bool>([id](FileSystem& fs) { return fs.Nodes().Contains(id); });
  }

  /// The ids of @p ids that the node cache holds.
  auto CachedOf(const std::vector<sl_element_id_t>& ids) -> std::vector<sl_element_id_t> {
    return sleeve_->Read<std::vector<sl_element_id_t>>([&ids](FileSystem& fs) {
      std::vector<sl_element_id_t> cached;
      for (const auto id : ids) {
        if (fs.Nodes().Contains(id)) {
          cached.push_back(id);
        }
      }
      return cached;
    });
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

// Sleeve Node Cache Plan SN5: the stored links of the mount folders, with the stored mount id of
// each file, in file id order.
TEST_F(MountTreeTest, MountFileFolderRowsHoldTheFileLinksOfTheMountFoldersOnly) {
  constexpr mount_id_t kOtherMount = 2;
  const auto           root        = MountRoot();
  const auto           day1        = AddFolder(root, L"Day1", kMount);
  const auto           notes       = AddFolder(0, L"Notes", std::nullopt);
  const auto           a           = AddFile(L"a.jpg", root, kMount);
  const auto           b           = AddFile(L"b.jpg", day1, kMount);
  const auto           ordinary    = AddFile(L"ordinary.jpg", day1, std::nullopt);
  const auto           other       = AddFile(L"other.jpg", day1, kOtherMount);
  AddFile(L"note.jpg", notes, kMount);
  Reopen();

  const auto rows = storage_->GetFolderEntryStore().ReadMountFileFolders(kMount);
  EXPECT_EQ(rows, (std::vector<MountFileFolderRow>{{a, root, kMount},
                                                   {b, day1, kMount},
                                                   {ordinary, day1, std::nullopt},
                                                   {other, day1, kOtherMount}}));
  EXPECT_TRUE(storage_->GetFolderEntryStore().ReadMountFileFolders(kOtherMount).empty());
}

// Sleeve Node Cache Plan SN5: after a reopen the cache holds the folders only, and the tree read
// of a mount with 1,000 files leaves it so.
TEST_F(MountTreeTest, ReadMountTreeLoadsNoFileElement) {
  const auto root  = MountRoot();
  const auto day1  = AddFolder(root, L"Day1", kMount);
  const auto day2  = AddFolder(root, L"Day2", kMount);
  const auto first = AddFiles(L"d1_", 600, day1, kMount);
  const auto other = AddFiles(L"d2_", 400, day2, kMount);
  Reopen();
  // The library root, the mount root, Day1, and Day2.
  ASSERT_EQ(CachedCount(), 4u);

  const auto tree = sleeve_->ReadMountTree(root, kMount);
  EXPECT_EQ(tree.folders, (std::vector<MountTreeFolder>{{L"Day1", day1}, {L"Day2", day2}}));
  ASSERT_EQ(tree.home_folders.size(), 1000u);
  EXPECT_EQ(tree.home_folders.at(first.front()), std::vector<sl_element_id_t>{day1});
  EXPECT_EQ(tree.home_folders.at(other.back()), std::vector<sl_element_id_t>{day2});
  EXPECT_EQ(CachedCount(), 4u);
  EXPECT_EQ(sleeve_->ReadMountFolders(root, kMount), tree.folders);
  EXPECT_EQ(CachedCount(), 4u);
}

// Sleeve Node Cache Plan SN5: inside a write, the tree read applies the pending links and
// unlinks of the mount folders and decides a cached file from memory.
TEST_F(MountTreeTest, ReadMountTreeAppliesThePendingChangesOfTheWrite) {
  const auto root     = MountRoot();
  const auto day1     = AddFolder(root, L"Day1", kMount);
  const auto kept     = AddFile(L"kept.jpg", day1, kMount);
  const auto unlinked = AddFile(L"unlinked.jpg", day1, kMount);
  const auto deleted  = AddFile(L"deleted.jpg", day1, kMount);
  const auto adopted  = AddFile(L"adopted.jpg", day1, std::nullopt);
  const auto released = AddFile(L"released.jpg", root, kMount);
  Reopen();

  sl_element_id_t created = 0;
  const auto      sync    = sleeve_->Write<void>([&](FileSystem& fs) {
    auto file       = fs.CreateFileInLibrary(L"new.jpg");
    file->mount_id_ = kMount;
    fs.LinkFileToFolder(file->element_id_, root);
    created = file->element_id_;
    fs.UnlinkFileFromFolder(unlinked, day1);
    fs.DeleteFileEverywhere(deleted);
    auto adopted_file       = fs.Get(adopted);
    adopted_file->mount_id_ = kMount;
    adopted_file->MarkModified();
    auto released_file = fs.Get(released);
    released_file->mount_id_.reset();
    released_file->MarkModified();

    const auto tree = ReadMountTree(fs, root, kMount);
    EXPECT_EQ(tree.home_folders.size(), 3u);
    EXPECT_EQ(tree.home_folders.at(kept), std::vector<sl_element_id_t>{day1});
    EXPECT_EQ(tree.home_folders.at(adopted), std::vector<sl_element_id_t>{day1});
    EXPECT_EQ(tree.home_folders.at(created), std::vector<sl_element_id_t>{root});
    EXPECT_FALSE(tree.home_folders.contains(unlinked));
    EXPECT_FALSE(tree.home_folders.contains(deleted));
    EXPECT_FALSE(tree.home_folders.contains(released));
  });
  EXPECT_TRUE(sync.success_) << sync.message_;
}

// Sleeve Node Cache Plan SN5: a placement in a mount with 10,000 files loads the placed file
// only, and the unlink from its old home folder needs no other element.
TEST_F(MountTreeTest, PlacementOfOneFileLoadsOneFileElement) {
  const auto root  = MountRoot();
  const auto day1  = AddFolder(root, L"Day1", kMount);
  const auto day2  = AddFolder(root, L"Day2", kMount);
  const auto files = AddFiles(L"img_", 10'000, day1, kMount);
  Reopen();
  ASSERT_EQ(CachedCount(), 4u);

  const auto       moved = files[1234];
  MountTreeChanges changes{.mount_id = kMount, .root_folder_id = root};
  changes.placements = {{moved, L"Day2", L"moved.jpg"}};
  const auto sync    = sleeve_->ApplyMountTreeChanges(changes);
  ASSERT_TRUE(sync.success_) << sync.message_;

  // The four folders and the placed file.
  EXPECT_EQ(CachedCount(), 5u);
  EXPECT_TRUE(IsCached(moved));
  Reopen();
  const auto tree = sleeve_->ReadMountTree(root, kMount);
  EXPECT_EQ(tree.home_folders.size(), 10'000u);
  EXPECT_EQ(tree.home_folders.at(moved), std::vector<sl_element_id_t>{day2});
  EXPECT_EQ(tree.home_folders.at(files.front()), std::vector<sl_element_id_t>{day1});
  EXPECT_EQ(Element(moved)->element_name_, L"moved.jpg");
}

// Sleeve Node Cache Plan SN5: a removed folder gives its files to its parent with no file element
// load, and a file that a placement moved out of it stays in its new folder.
TEST_F(MountTreeTest, RemovedFolderMovesItsFilesWithNoFileElementLoad) {
  const auto root    = MountRoot();
  const auto day1    = AddFolder(root, L"Day1", kMount);
  const auto morning = AddFolder(day1, L"Morning", kMount);
  const auto day2    = AddFolder(root, L"Day2", kMount);
  const auto files   = AddFiles(L"m_", 1000, morning, kMount);
  const auto upper   = AddFiles(L"d_", 10, day1, kMount);
  Reopen();

  const auto       placed = files.front();
  MountTreeChanges changes{.mount_id = kMount, .root_folder_id = root};
  changes.placements        = {{placed, L"Day2", L"placed.jpg"}};
  changes.folders_to_remove = {L"Day1/Morning", L"Day1"};
  const auto sync           = sleeve_->ApplyMountTreeChanges(changes);
  ASSERT_TRUE(sync.success_) << sync.message_;

  EXPECT_EQ(CachedOf(files), std::vector<sl_element_id_t>{placed});
  EXPECT_TRUE(CachedOf(upper).empty());
  Reopen();
  EXPECT_EQ(FolderPaths(root), std::vector<std::wstring>{L"Day2"});
  const auto tree = sleeve_->ReadMountTree(root, kMount);
  EXPECT_EQ(tree.home_folders.size(), 1010u);
  EXPECT_EQ(tree.home_folders.at(placed), std::vector<sl_element_id_t>{day2});
  EXPECT_EQ(tree.home_folders.at(files.back()), std::vector<sl_element_id_t>{root});
  EXPECT_EQ(tree.home_folders.at(upper.front()), std::vector<sl_element_id_t>{root});
  EXPECT_EQ(
      sleeve_->Read<std::size_t>([root](FileSystem& fs) { return fs.ListFileIds(root).size(); }),
      1009u);
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

// Sleeve Node Cache Plan SN4: a file name is a label, so a placed file takes the name of its file
// on the disk when another photo of the library holds it.
TEST_F(MountTreeTest, PlacedFileTakesItsDiskNameWhenTheLibraryHoldsIt) {
  const auto       root     = MountRoot();
  const auto       day2     = AddFolder(root, L"Day2", kMount);
  const auto       ordinary = AddFile(L"a.jpg", 0, std::nullopt);
  const auto       mounted  = AddFile(L"x.jpg", root, kMount);

  MountTreeChanges changes{.mount_id = kMount, .root_folder_id = root};
  changes.placements = {{mounted, L"Day2", L"a.jpg"}};
  ASSERT_TRUE(sleeve_->ApplyMountTreeChanges(changes).success_);
  Reopen();

  EXPECT_EQ(Element(mounted)->element_name_, L"a.jpg");
  EXPECT_EQ(Element(ordinary)->element_name_, L"a.jpg");
  EXPECT_EQ(Children(day2), (std::map<std::wstring, sl_element_id_t>{{L"a.jpg", mounted}}));
  EXPECT_EQ(HomeFolders(root, mounted), std::vector<sl_element_id_t>{day2});
}

// Sleeve Node Cache Plan SN4: only folder names are unique among the child folders of a parent,
// so a new folder takes the name of its directory and a file with that name keeps its name.
TEST_F(MountTreeTest, NewFolderTakesTheNameOfAFileInItsParent) {
  const auto       root    = MountRoot();
  const auto       mounted = AddFile(L"a.jpg", root, kMount);

  MountTreeChanges folder_changes{.mount_id = kMount, .root_folder_id = root};
  folder_changes.folders_to_add = {L"a.jpg"};
  ASSERT_TRUE(sleeve_->ApplyMountTreeChanges(folder_changes).success_);
  Reopen();

  EXPECT_EQ(FolderPaths(root), std::vector<std::wstring>{L"a.jpg"});
  EXPECT_EQ(Element(mounted)->element_name_, L"a.jpg");
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
