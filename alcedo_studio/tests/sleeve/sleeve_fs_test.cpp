//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <random>
#include <stdexcept>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/string/convert.hpp"

std::filesystem::path db_path(TEST_DB_PATH);

std::filesystem::path meta_path(TEST_META_PATH);

namespace alcedo {
namespace {

auto CountId(const std::vector<sl_element_id_t>& ids, sl_element_id_t id) -> size_t {
  return static_cast<size_t>(std::count(ids.begin(), ids.end(), id));
}

auto ContainsId(const std::vector<sl_element_id_t>& ids, sl_element_id_t id) -> bool {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void CleanupTestFiles() {
  if (std::filesystem::exists(::db_path)) {
    std::filesystem::remove(::db_path);
  }
  if (std::filesystem::exists(::meta_path)) {
    std::filesystem::remove(::meta_path);
  }
}

}  // namespace

TEST(SleeveFSTest, InitTest1) {
  TimeProvider::Refresh();
  {
    try {
      Storage storage_service{db_path};
      FileSystem     fs{db_path, storage_service, 0};
      fs.InitRoot();
    } catch (std::exception& e) {
      std::cout << e.what() << std::endl;
      FAIL();
    }
  }
  // Test whether the connection is released
  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
}

TEST(SleeveFSTest, AddGetTest1) {
  {
    try {
      Storage storage_service{db_path};
      FileSystem     fs{db_path, storage_service, 0};
      fs.InitRoot();

      fs.Create(L"", L"FILE", ElementType::FILE);
      auto new_file = fs.Get("/FILE", false);
      EXPECT_FALSE(new_file == nullptr);
      EXPECT_EQ(new_file->element_name_, L"FILE");
      EXPECT_EQ(new_file->element_id_, 1);
      EXPECT_EQ(new_file->ref_count_, 1);
    } catch (std::exception& e) {
      std::cout << e.what() << std::endl;
      FAIL();
    }
  }
  // Test whether the connection is released
  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
}

TEST(SleeveFSTest, AddGetTest2) {
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    fs.Create(L"", L"Folder", ElementType::FOLDER);
    fs.Create(L"/Folder", L"Subfolder", ElementType::FOLDER);
    fs.Create(L"/Folder", L"File", ElementType::FILE);

    auto folder = fs.Get(L"/Folder", false);
    EXPECT_FALSE(folder == nullptr);
    auto subfolder = fs.Get(L"/Folder/Subfolder", false);
    EXPECT_FALSE(subfolder == nullptr);
    auto file = fs.Get(L"/Folder/File", false);
    EXPECT_FALSE(file == nullptr);

    std::cout << conv::ToBytes(fs.Tree(L"/"));
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
}

/* THIS test is being disabled*/
// Under the new design, re-initialization is managed through SleeveService
//
// TEST(SleeveFSTest, ReInitTest1) {
//   {
//     try {
//       Storage storage_service{db_path};
//       FileSystem     fs{db_path, storage_service, 0};
//       fs.InitRoot();

//       fs.Create(L"", L"Folder", ElementType::FOLDER);
//       fs.Create(L"/Folder", L"Subfolder", ElementType::FOLDER);
//       fs.Create(L"/Folder", L"File", ElementType::FILE);

//       auto folder = fs.Get(L"/Folder", false);
//       EXPECT_FALSE(folder == nullptr);
//       auto subfolder = fs.Get(L"/Folder/Subfolder", false);
//       EXPECT_FALSE(subfolder == nullptr);
//       auto file = fs.Get(L"/Folder/File", false);
//       EXPECT_FALSE(file == nullptr);

//       fs.SyncToDB();
//       fs.WriteSleeveMeta(meta_path);
//       auto tree_str = conv::ToBytes(fs.Tree(L""));
//       std::cout << "Before reloading:\n" << tree_str << std::endl;
//     } catch (std::exception& e) {
//       std::cout << e.what() << std::endl;
//       FAIL();
//     }
//   }

//   // Sync function has not been implemented
//   {
//     try {
//       // Auto recovered start id has not been implemented
//       Storage storage_service{db_path};
//       FileSystem     fs{db_path, storage_service, 0};
//       fs.ReadSleeveMeta(meta_path);
//       fs.InitRoot();

//       auto folder = fs.Get(L"/Folder", false);
//       EXPECT_FALSE(folder == nullptr);
//       auto subfolder = fs.Get(L"/Folder/Subfolder", false);
//       EXPECT_FALSE(subfolder == nullptr);
//       auto file = fs.Get(L"/Folder/File", false);
//       EXPECT_FALSE(file == nullptr);

//       std::cout << "After reloading:\n" << conv::ToBytes(fs.Tree(L"")) << std::endl;
//     } catch (std::exception& e) {
//       std::cout << e.what() << std::endl;
//       FAIL();
//     }
//   }

//   if (std::filesystem::exists(db_path)) {
//     std::filesystem::remove(db_path);
//   }
// }

TEST(SleeveFSTest, DeleteTest1) {
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    fs.Create(L"", L"FILE", ElementType::FILE);
    auto new_file = fs.Get("/FILE", false);
    EXPECT_FALSE(new_file == nullptr);
    EXPECT_EQ(new_file->element_name_, L"FILE");
    EXPECT_EQ(new_file->element_id_, 1);
    EXPECT_EQ(new_file->ref_count_, 1);

    fs.Delete(L"/FILE");
    fs.Get("/FILE", false);
  } catch (std::runtime_error& e) {
    std::cout << "Expected exception: " << e.what() << std::endl;
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
}

TEST(SleeveFSTest, DeleteTest2) {
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    fs.Create(L"", L"Folder", ElementType::FOLDER);
    fs.Create(L"/Folder", L"Subfolder", ElementType::FOLDER);
    fs.Create(L"/Folder", L"File", ElementType::FILE);

    fs.Delete(L"/Folder");
    fs.Get("/Folder/Subfolder", false);
  } catch (std::runtime_error& e) {
    std::cout << "Expected exception: " << e.what() << std::endl;
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
}

TEST(SleeveFSTest, CopyTest1) {
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    fs.Create(L"", L"Folder", ElementType::FOLDER);
    fs.Create(L"/Folder", L"Subfolder", ElementType::FOLDER);
    fs.Create(L"/Folder/Subfolder", L"Linux", ElementType::FILE);
    fs.Create(L"/Folder", L"File", ElementType::FILE);
    fs.Copy(L"/Folder/Subfolder", L"/");

    std::cout << conv::ToBytes(fs.Tree(L"/"));
  } catch (std::runtime_error& e) {
    std::cout << "Expected exception: " << e.what() << std::endl;
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
}

TEST(SleeveFSTest, CoWTest1) {
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    fs.Create(L"", L"Folder", ElementType::FOLDER);
    fs.Create(L"/Folder", L"Subfolder", ElementType::FOLDER);
    fs.Create(L"/Folder/Subfolder", L"Linux", ElementType::FILE);
    fs.Create(L"/Folder", L"File", ElementType::FILE);
    fs.Copy(L"/Folder/Subfolder", L"/");

    fs.Create(L"/Folder/Subfolder", L"Windows", ElementType::FILE);

    std::cout << conv::ToBytes(fs.Tree(L"/"));
  } catch (std::exception& e) {
    std::cout << "Unexpected exception: " << e.what() << std::endl;
    FAIL();
  }

  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
}

TEST(SleeveFSTest, CoWTest2) {
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    fs.Create(L"", L"Folder", ElementType::FOLDER);
    fs.Create(L"/Folder", L"Subfolder", ElementType::FOLDER);
    fs.Create(L"/Folder/Subfolder", L"Linux", ElementType::FILE);
    fs.Create(L"/Folder", L"File", ElementType::FILE);
    fs.Copy(L"/Folder", L"/Folder/Subfolder");
  } catch (std::runtime_error& e) {
    auto exception_msg = std::string(e.what());
    std::cout << "Expected exception: " << e.what() << std::endl;
    EXPECT_EQ(exception_msg,
              "Filesystem: Target folder cannot be a subfolder of the original folder");
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
}

TEST(SleeveFSTest, LinkFileToFoldersKeepsSingleFileIdentity) {
  CleanupTestFiles();
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    auto album_a = fs.Create(L"", L"AlbumA", ElementType::FOLDER);
    auto album_b = fs.Create(L"", L"AlbumB", ElementType::FOLDER);
    auto file    = fs.CreateFileInLibrary(L"Shared.arw");
    ASSERT_NE(file, nullptr);

    fs.LinkFileToFolder(file->element_id_, album_a->element_id_);
    fs.LinkFileToFolder(file->element_id_, album_a->element_id_);
    fs.LinkFileToFolder(file->element_id_, album_b->element_id_);

    const auto root_ids    = fs.ListFolderContent(0);
    const auto album_a_ids = fs.ListFolderContent(album_a->element_id_);
    const auto album_b_ids = fs.ListFolderContent(album_b->element_id_);

    EXPECT_EQ(CountId(root_ids, file->element_id_), 1u);
    EXPECT_EQ(CountId(album_a_ids, file->element_id_), 1u);
    EXPECT_EQ(CountId(album_b_ids, file->element_id_), 1u);
    EXPECT_EQ(file->ref_count_, 1u);

    auto from_root =
        std::static_pointer_cast<SleeveFile>(fs.Get(L"/Shared.arw", false));
    auto from_album_a =
        std::static_pointer_cast<SleeveFile>(fs.Get(L"/AlbumA/Shared.arw", false));
    auto from_album_b =
        std::static_pointer_cast<SleeveFile>(fs.Get(L"/AlbumB/Shared.arw", false));

    EXPECT_EQ(from_root->element_id_, file->element_id_);
    EXPECT_EQ(from_album_a->element_id_, file->element_id_);
    EXPECT_EQ(from_album_b->element_id_, file->element_id_);

    auto edited =
        std::static_pointer_cast<SleeveFile>(fs.Get(L"/AlbumA/Shared.arw", true));
    edited->SetLastModifiedTime();
    EXPECT_EQ(from_root->last_modified_time_, edited->last_modified_time_);
    EXPECT_EQ(from_album_b->last_modified_time_, edited->last_modified_time_);
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  CleanupTestFiles();
}

TEST(SleeveFSTest, CopyFilePathLinksMembershipInsteadOfDuplicatingFile) {
  CleanupTestFiles();
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    auto album = fs.Create(L"", L"Album", ElementType::FOLDER);
    auto file  = fs.CreateFileInLibrary(L"CopyMe.arw");
    ASSERT_NE(file, nullptr);

    const auto next_id_before_copy = fs.GetCurrentID();
    fs.Copy(L"/CopyMe.arw", L"/Album");
    EXPECT_EQ(fs.GetCurrentID(), next_id_before_copy);

    auto from_root = fs.Get(L"/CopyMe.arw", false);
    auto from_album = fs.Get(L"/Album/CopyMe.arw", false);
    EXPECT_EQ(from_root->element_id_, file->element_id_);
    EXPECT_EQ(from_album->element_id_, file->element_id_);
    EXPECT_TRUE(ContainsId(fs.ListFolderContent(album->element_id_), file->element_id_));
    EXPECT_EQ(file->ref_count_, 1u);
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  CleanupTestFiles();
}

TEST(SleeveFSTest, DeletingFromAlbumOnlyUnlinksMembership) {
  CleanupTestFiles();
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    auto album = fs.Create(L"", L"Album", ElementType::FOLDER);
    auto file  = fs.CreateFileInLibrary(L"KeepInRoot.arw");
    ASSERT_NE(file, nullptr);
    fs.LinkFileToFolder(file->element_id_, album->element_id_);

    fs.Delete(L"/Album/KeepInRoot.arw");

    auto from_root = fs.Get(L"/KeepInRoot.arw", false);
    EXPECT_EQ(from_root->element_id_, file->element_id_);
    EXPECT_TRUE(ContainsId(fs.ListFolderContent(0), file->element_id_));
    EXPECT_FALSE(ContainsId(fs.ListFolderContent(album->element_id_), file->element_id_));
    EXPECT_NE(file->sync_flag_, SyncFlag::DELETED);
    EXPECT_THROW(fs.Get(L"/Album/KeepInRoot.arw", false), std::runtime_error);
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  CleanupTestFiles();
}

TEST(SleeveFSTest, DeletingFromRootDeletesFileEverywhere) {
  CleanupTestFiles();
  try {
    Storage storage_service{db_path};
    FileSystem     fs{db_path, storage_service, 0};
    fs.InitRoot();

    auto album_a = fs.Create(L"", L"AlbumA", ElementType::FOLDER);
    auto album_b = fs.Create(L"", L"AlbumB", ElementType::FOLDER);
    auto file    = fs.CreateFileInLibrary(L"DeleteEverywhere.arw");
    ASSERT_NE(file, nullptr);
    fs.LinkFileToFolder(file->element_id_, album_a->element_id_);
    fs.LinkFileToFolder(file->element_id_, album_b->element_id_);

    fs.Delete(L"/DeleteEverywhere.arw");

    EXPECT_EQ(file->sync_flag_, SyncFlag::DELETED);
    EXPECT_FALSE(ContainsId(fs.ListFolderContent(0), file->element_id_));
    EXPECT_FALSE(ContainsId(fs.ListFolderContent(album_a->element_id_), file->element_id_));
    EXPECT_FALSE(ContainsId(fs.ListFolderContent(album_b->element_id_), file->element_id_));
    EXPECT_THROW(fs.Get(L"/DeleteEverywhere.arw", false), std::runtime_error);
    EXPECT_THROW(fs.Get(L"/AlbumA/DeleteEverywhere.arw", false), std::runtime_error);
    EXPECT_THROW(fs.Get(L"/AlbumB/DeleteEverywhere.arw", false), std::runtime_error);
  } catch (std::exception& e) {
    std::cout << e.what() << std::endl;
    FAIL();
  }

  CleanupTestFiles();
}

// Due to the new design of SleeveService managing the re-initialization,
// these tests are disabled for now.
// And they will be re-tested under SleeveService tests.
//
//
// TEST(SleeveFSTest, CoWTest3) {
//   if (std::filesystem::exists(db_path)) {
//     std::filesystem::remove(db_path);
//   }
//   try {
//     Storage storage_service{db_path};
//       FileSystem fs{db_path, storage_service, 0};
//     fs.InitRoot();

//     fs.Create(L"", L"B", ElementType::FOLDER);
//     fs.Create(L"", L"C", ElementType::FOLDER);
//     fs.Create(L"/B", L"D", ElementType::FOLDER);
//     fs.Create(L"/C", L"E", ElementType::FOLDER);
//     fs.Copy(L"/C", L"/B/D");
//     fs.Copy(L"/B/D", L"/C/E");

//     std::cout << "Before reloading:\n" << conv::ToBytes(fs.Tree(L"/"));
//     fs.SyncToDB();
//     fs.WriteSleeveMeta(meta_path);

//   } catch (std::exception& e) {
//     std::cout << "Unexpected exception: " << e.what() << std::endl;
//     FAIL();
//   }

//   try {
//     Storage storage_service{db_path};
//       FileSystem fs{db_path, storage_service, 0};
//     fs.ReadSleeveMeta(meta_path);
//     fs.InitRoot();

//     std::cout << "After reloading:\n" << conv::ToBytes(fs.Tree(L"/"));
//   } catch (std::exception& e) {
//     std::cout << "Unexpected exception: " << e.what() << std::endl;
//     FAIL();
//   }

//   if (std::filesystem::exists(db_path)) {
//     std::filesystem::remove(db_path);
//   }
// }

// TEST(SleeveFSTest, ReCoWTest1) {
//   std::string first_tree;
//   try {
//     Storage storage_service{db_path};
//       FileSystem fs{db_path, storage_service, 0};
//     fs.InitRoot();

//     fs.Create(L"", L"Folder", ElementType::FOLDER);
//     fs.Create(L"/Folder", L"Subfolder", ElementType::FOLDER);
//     fs.Create(L"/Folder/Subfolder", L"Linux", ElementType::FILE);
//     fs.Create(L"/Folder", L"File", ElementType::FILE);
//     fs.Copy(L"/Folder/Subfolder", L"/");

//     fs.Create(L"/Folder/Subfolder", L"Windows", ElementType::FILE);

//     first_tree = conv::ToBytes(fs.Tree(L"/"));
//     std::cout << first_tree;

//     fs.SyncToDB();
//   } catch (std::runtime_error& e) {
//     std::cout << "Unexpected exception: " << e.what() << std::endl;
//     FAIL();
//   } catch (std::exception& e) {
//     std::cout << e.what() << std::endl;
//     FAIL();
//   }

//   std::string second_tree;
//   try {
//     // 8 is just a big enough number...
//     Storage storage_service{db_path};
//       FileSystem fs{db_path, storage_service, 0};
//     fs.InitRoot();

//     second_tree = conv::ToBytes(fs.Tree(L"/"));
//     std::cout << second_tree;

//     EXPECT_EQ(first_tree, second_tree);

//   } catch (std::runtime_error& e) {
//     std::cout << "Unexpected exception: " << e.what() << std::endl;
//     FAIL();
//   } catch (std::exception& e) {
//     std::cout << e.what() << std::endl;
//     FAIL();
//   }

//   if (std::filesystem::exists(db_path)) {
//     std::filesystem::remove(db_path);
//   }
// }

// // 1. Using a test fixture to manage resources
// class RandomizedFileSystemTest : public ::testing::Test {
//  protected:
//   std::filesystem::path db_path_;

//   // Run before any unit test runs
//   void                  SetUp() override {
//     // Create a unique db file location
//     db_path_ = std::filesystem::temp_directory_path() / "test_db.db";
//     // Make sure there is not existing db
//     if (std::filesystem::exists(db_path_)) {
//       std::filesystem::remove(db_path_);
//     }
//   }

//   // Run before any unit test runs
//   void TearDown() override {
//     // Clean up the DB file
//     if (std::filesystem::exists(db_path_)) {
//       std::filesystem::remove(db_path_);
//     }
//   }

//   // Helper function: generate a unique file or folder name
//   std::wstring GenerateRandomName(std::mt19937& gen, int length = 8) {
//     std::wstring chars = L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
//     std::uniform_int_distribution<> dist(0, chars.length() - 1);
//     std::wstring                    result;
//     for (int i = 0; i < length; ++i) {
//       result += chars[dist(gen)];
//     }
//     return result;
//   }
// };

// // Fuzzing Test1
// TEST_F(RandomizedFileSystemTest, SaveAndLoadConsistencyAfterRandomOps) {
//   // 设置随机数生成器
//   std::random_device        rd;
//   std::mt19937              gen(rd());

//   // Following the valid paths
//   std::vector<std::wstring> known_paths;
//   std::vector<std::wstring> known_paths_folder;
//   known_paths.push_back(L"");         // root path always exists
//   known_paths_folder.push_back(L"");  // root path always exists

//   std::string first_tree;

//   // Part1: randomized operation sequence

//   {
//     Storage storage_service{db_path};
//       FileSystem fs{db_path, storage_service, 0};
//     fs.InitRoot();

//     const int num_operations = 500;  // execute 100 random tests
//     for (int i = 0; i < num_operations; ++i) {
//       // Randomly choose one operation：0=Create, 1=Copy
//       std::uniform_int_distribution<> op_dist(0, 1);
//       int                             operation = op_dist(gen);

//       // Make sure there are enouth paths for copy
//       if (known_paths.size() < 2 && operation == 1) {
//         operation = 0;  // Force Create()
//       }

//       switch (operation) {
//         case 0: {  // Random Create()
//           // Chose one parent path
//           std::uniform_int_distribution<size_t> path_dist(0, known_paths_folder.size() - 1);
//           std::wstring                          parent_path = known_paths_folder[path_dist(gen)];

//           // Randomly generate the filename and folder
//           std::wstring                          new_name    = GenerateRandomName(gen);
//           ElementType type = (gen() % 2 == 0) ? ElementType::FOLDER : ElementType::FILE;

//           try {
//             // execute the operation
//             fs.Create(parent_path, new_name, type);

//             // Once success, add the new path to the known paths
//             std::wstring new_path = parent_path + L"/" + new_name;
//             known_paths.push_back(new_path);
//             if (type == ElementType::FOLDER) {
//               known_paths_folder.push_back(new_path);
//             }
//           } catch (const std::exception& e) {
//             std::string error_message = e.what();

//             if (error_message.find("Target folder cannot be a subfolder") != std::string::npos ||
//                 error_message.find("Cannot create element under a file") != std::string::npos) {
//               std::cout << "Caught expected exception: " << error_message << "\n";
//             } else {
//               FAIL() << "Caught UNEXPECTED exception during random operations: " << e.what();
//             }
//           }
//           break;
//         }
//         case 1: {  // Random Copy()
//           std::uniform_int_distribution<size_t> path_dist(0, known_paths.size() - 1);
//           std::uniform_int_distribution<size_t> path_folder_dist(0, known_paths_folder.size() -
//           1); std::wstring                          from_path = known_paths[path_dist(gen)];
//           std::wstring to_path_parent = known_paths_folder[path_folder_dist(gen)];

//           try {
//             fs.Copy(from_path, to_path_parent);
//           } catch (const std::exception& e) {
//             std::string error_message = e.what();

//             if (error_message.find("Target folder cannot be a subfolder") != std::string::npos ||
//                 error_message.find("Cannot create element under a file") != std::string::npos) {
//               std::cout << "Caught expected exception: " << error_message << "\n";
//             } else {
//               FAIL() << "Caught UNEXPECTED exception during random operations: " << e.what();
//             }
//           }
//           break;
//         }
//       }
//     }

//     // Once operation is done, display the tree
//     first_tree = conv::ToBytes(fs.Tree(L"/"));

//     // Store to the database
//     fs.SyncToDB();
//     fs.WriteSleeveMeta(meta_path);
//   }

//   // Part2: Reloading
//   std::string second_tree;
//   try {
//     Storage storage_service{db_path};
//       FileSystem fs{db_path, storage_service, 8};
//     fs.InitRoot();
//     fs.ReadSleeveMeta(meta_path);

//     second_tree = conv::ToBytes(fs.Tree(L"/"));

//   } catch (const std::exception& e) {
//     FAIL() << "Unexpected exception during reload: " << e.what();
//   }

//   // Core assertion: check if the original tree and the recovered tree are identical
//   EXPECT_EQ(first_tree, second_tree);
//   // std::cout << second_tree << "\n";
// }
};  // namespace alcedo
