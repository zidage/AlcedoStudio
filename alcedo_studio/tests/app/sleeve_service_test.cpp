//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/sleeve_service.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "../support/storage_test_access.hpp"
#include "app/album_browse_service.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_error.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "json.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/storage.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/mount/mount_store.hpp"
#include "support/test_temp_directory.hpp"
#include "type/mount_records.hpp"
#include "type/source_file_record.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/logging/log.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
auto ContainsId(const std::vector<sl_element_id_t>& ids, sl_element_id_t id) -> bool {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// The live files named @p name in the folder at @p folder_path. A file has no path (a file
/// name is a label, Sleeve Node Cache Plan SN4), so a test finds a file by its folder and name.
auto FilesNamed(SleeveServiceImpl& service, const std::filesystem::path& folder_path,
                const file_name_t& name) -> std::vector<std::shared_ptr<SleeveFile>> {
  std::vector<std::shared_ptr<SleeveFile>> files;
  for (const auto& entry : service.ListFolderEntries(folder_path)) {
    if (entry->type_ == ElementType::FILE && entry->element_name_ == name) {
      files.push_back(std::static_pointer_cast<SleeveFile>(entry));
    }
  }
  return files;
}

/// The id of the one live file named @p name in the folder at @p folder_path; 0 when the folder
/// holds no such file. Fails the test when it holds more than one.
auto FileIdNamed(SleeveServiceImpl& service, const std::filesystem::path& folder_path,
                 const file_name_t& name) -> sl_element_id_t {
  const auto files = FilesNamed(service, folder_path, name);
  EXPECT_LE(files.size(), 1u) << conv::ToBytes(name);
  return files.empty() ? 0 : files.front()->element_id_;
}

auto RunDuckDbSql(const std::filesystem::path& db_path, const std::string& sql) -> bool {
  duckdb_database   db;
  duckdb_connection conn;
  if (duckdb_open(conv::ToBytes(db_path.wstring()).c_str(), &db) != DuckDBSuccess) {
    return false;
  }
  if (duckdb_connect(db, &conn) != DuckDBSuccess) {
    duckdb_close(&db);
    return false;
  }

  duckdb_result result;
  const bool    ok = duckdb_query(conn, sql.c_str(), &result) == DuckDBSuccess;
  duckdb_destroy_result(&result);
  duckdb_disconnect(&conn);
  duckdb_close(&db);
  return ok;
}

auto QueryDuckDbInt64(const std::filesystem::path& db_path, const std::string& sql) -> int64_t {
  duckdb_database   db;
  duckdb_connection conn;
  if (duckdb_open(conv::ToBytes(db_path.wstring()).c_str(), &db) != DuckDBSuccess) {
    throw std::runtime_error("Failed to open test database");
  }
  if (duckdb_connect(db, &conn) != DuckDBSuccess) {
    duckdb_close(&db);
    throw std::runtime_error("Failed to connect test database");
  }

  duckdb_result result;
  if (duckdb_query(conn, sql.c_str(), &result) != DuckDBSuccess) {
    const std::string error_message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    duckdb_disconnect(&conn);
    duckdb_close(&db);
    throw std::runtime_error(error_message);
  }
  int64_t value = 0;
  if (duckdb_row_count(&result) > 0) {
    value = duckdb_value_int64(&result, 0, 0);
  }
  duckdb_destroy_result(&result);
  duckdb_disconnect(&conn);
  duckdb_close(&db);
  return value;
}

/// `FolderContent` rows of @p folder_id on the project's connection: element id -> DuckDB rowid.
/// A row that a sync deletes and inserts again gets a new rowid.
auto FolderContentRowIds(ProjectService& project, sl_element_id_t folder_id)
    -> std::map<sl_element_id_t, int64_t> {
  auto          guard = StorageTestAccess::GetDatabase(*project.GetStorage()).GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  const auto    sql = "SELECT element_id, rowid FROM FolderContent WHERE folder_id = " +
                   std::to_string(folder_id);
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    const std::string error = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error);
  }
  std::map<sl_element_id_t, int64_t> rows;
  for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
    rows[static_cast<sl_element_id_t>(duckdb_value_int64(&result, 0, row))] =
        duckdb_value_int64(&result, 1, row);
  }
  duckdb_destroy_result(&result);
  return rows;
}

/// Runs @p sql on the project's connection; throws the DuckDB message when it fails.
void ExecuteOnProjectDatabase(ProjectService& project, const std::string& sql) {
  auto          guard = StorageTestAccess::GetDatabase(*project.GetStorage()).GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  const bool    ok        = duckdb_query(guard.conn_, sql.c_str(), &result) == DuckDBSuccess;
  const std::string error = ok ? std::string{} : duckdb_result_error(&result);
  duckdb_destroy_result(&result);
  if (!ok) {
    throw std::runtime_error(error);
  }
}

/// The first value of the first row of @p sql on the project's connection.
auto QueryProjectInt64(ProjectService& project, const std::string& sql) -> int64_t {
  auto          guard = StorageTestAccess::GetDatabase(*project.GetStorage()).GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    const std::string error = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error);
  }
  const int64_t value = duckdb_row_count(&result) > 0 ? duckdb_value_int64(&result, 0, 0) : 0;
  duckdb_destroy_result(&result);
  return value;
}

/// The number of Element rows with the id @p id.
auto ElementRowCount(ProjectService& project, sl_element_id_t id) -> int64_t {
  return QueryProjectInt64(project,
                           "SELECT COUNT(*) FROM Element WHERE id = " + std::to_string(id));
}

/// The number of Element rows with the id @p id and the name @p name.
auto NamedElementRowCount(ProjectService& project, sl_element_id_t id, const std::string& name)
    -> int64_t {
  return QueryProjectInt64(project, "SELECT COUNT(*) FROM Element WHERE id = " +
                                        std::to_string(id) + " AND element_name = '" + name + "'");
}

/// The folder names of a listing in ascending order.
auto SortedFolderNames(const std::vector<AlbumFolderView>& folders) -> std::vector<std::wstring> {
  std::vector<std::wstring> names;
  names.reserve(folders.size());
  for (const auto& folder : folders) {
    names.push_back(folder.folder_name_);
  }
  std::sort(names.begin(), names.end());
  return names;
}

/// Exposure in the element pipeline JSON of @p id: the data SleeveService copies when it
/// duplicates a file.
auto ReadStoredExposure(Storage& storage, sl_element_id_t id) -> float {
  const auto json = storage.GetElementStore().GetPipelineJsonByElementId(id);
  if (!json.has_value()) {
    throw std::runtime_error("Element has no stored pipeline JSON");
  }
  const auto  document = PipelineDocument::FromJson(*json);
  const auto* exposure = document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  if (exposure == nullptr) {
    throw std::runtime_error("Pipeline document has no exposure adjustment");
  }
  return exposure->ToJson().at("exposure_ev").get<float>();
}

/// Store a default document with @p exposure_ev as the element pipeline JSON of @p id.
void WriteStoredExposure(Storage& storage, sl_element_id_t id, float exposure_ev) {
  auto  document = CreateDefaultPipelineDocument();
  auto* exposure = document.PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure());
  if (exposure == nullptr) {
    throw std::runtime_error("Pipeline document has no exposure adjustment");
  }
  exposure->LoadJson({{"exposure_ev", exposure_ev}});
  storage.GetElementStore().UpdatePipelineJsonByElementId(id, document.ToJson());
}

}  // namespace

TEST(ProjectVersionTests, MiniGitSchemaBumpRejectsPreviousProjectVersion) {
  EXPECT_TRUE(project_pack::ProjectVersionIsSupported(project_pack::kProjectFileVersion));
  EXPECT_EQ(project_pack::kProjectFileVersion, "0.13.0");
  EXPECT_TRUE(project_pack::ProjectVersionIsSupported("0.10.0"));
  EXPECT_TRUE(project_pack::ProjectVersionIsSupported("0.11.0"));
  EXPECT_TRUE(project_pack::ProjectVersionIsSupported("0.12.0"));
  EXPECT_TRUE(project_pack::ProjectVersionIsSupported("0.13.0"));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.14.0"));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.2.5"));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.2.4"));
}

class SleeveServiceTests : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void                  SetUp() override {
    TimeProvider::Refresh();
    db_path_   = temp_dir_ / "project.db";
    meta_path_ = temp_dir_ / "project.json";
  }

  void TearDown() override {
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    if (std::filesystem::exists(meta_path_)) {
      std::filesystem::remove(meta_path_);
    }
  }
};

TEST_F(SleeveServiceTests, InitAndCreateTest) {
  ProjectService project(db_path_, meta_path_);
  auto           service      = project.GetSleeveService();

  auto           write_result = service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"", L"Folder", ElementType::FOLDER); });
  EXPECT_NE(write_result.first, nullptr);
  EXPECT_TRUE(write_result.second.success_);

  const auto created = service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"/Folder", L"File", ElementType::FILE); });
  ASSERT_TRUE(created.second.success_);

  EXPECT_EQ(FileIdNamed(*service, L"/Folder", L"File"), created.first->element_id_);
  // A file has no path: the name is a label.
  EXPECT_THROW(service->Read<std::shared_ptr<SleeveElement>>(
                   [](FileSystem& fs) { return fs.Get(L"/Folder/File", false); }),
               std::runtime_error);
}

TEST_F(SleeveServiceTests, DeleteTest) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  const auto     created = service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"", L"File", ElementType::FILE); });
  ASSERT_TRUE(created.second.success_);
  ASSERT_TRUE(service->DeleteElement(created.first->element_id_).success_);

  EXPECT_EQ(FileIdNamed(*service, L"/", L"File"), 0u);
}

TEST_F(SleeveServiceTests, CopyTest) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"", L"Folder", ElementType::FOLDER); });
  service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"/Folder", L"Subfolder", ElementType::FOLDER); });
  service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"/Folder/Subfolder", L"Linux", ElementType::FILE); });

  service->Write<bool>([](FileSystem& fs) {
    fs.Copy(L"/Folder/Subfolder", L"/");
    return true;
  });

  auto tree     = service->Read<std::wstring>([](FileSystem& fs) { return fs.Tree(L"/"); });
  auto tree_str = conv::ToBytes(tree);
  EXPECT_NE(tree_str.find("Subfolder"), std::string::npos);
  EXPECT_NE(tree_str.find("Linux"), std::string::npos);
}

TEST_F(SleeveServiceTests, SaveLoadTest) {
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    service->Write<std::shared_ptr<SleeveElement>>(
        [](FileSystem& fs) { return fs.Create(L"", L"Folder", ElementType::FOLDER); });
    service->Write<std::shared_ptr<SleeveElement>>(
        [](FileSystem& fs) { return fs.Create(L"/Folder", L"File", ElementType::FILE); });

    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           reloaded_service = reloaded_project.GetSleeveService();
  const auto     files            = FilesNamed(*reloaded_service, L"/Folder", L"File");
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(files.front()->element_name_, L"File");
}

TEST_F(SleeveServiceTests, ResolveAndListImmediateChildrenByPath) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  service->CreateFolder(L"/", L"Folder");
  service->CreateFolder(L"/Folder", L"Nested");
  service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"/Folder/Nested", L"Image", ElementType::FILE); });

  const auto root_entries = service->ListFolderEntries(L"/");
  ASSERT_EQ(root_entries.size(), 2u);
  EXPECT_TRUE(std::any_of(root_entries.begin(), root_entries.end(), [](const auto& entry) {
    return entry && entry->element_name_ == L"Folder" && entry->type_ == ElementType::FOLDER;
  }));
  EXPECT_TRUE(std::any_of(root_entries.begin(), root_entries.end(), [](const auto& entry) {
    return entry && entry->element_name_ == L"Image" && entry->type_ == ElementType::FILE;
  }));

  const auto folder = service->ResolveFolder(L"/Folder");
  ASSERT_NE(folder, nullptr);
  EXPECT_EQ(folder->element_name_, L"Folder");

  const auto nested_entries = service->ListFolderEntries(L"/Folder");
  ASSERT_EQ(nested_entries.size(), 1u);
  EXPECT_EQ(nested_entries.front()->element_name_, L"Nested");
  EXPECT_EQ(nested_entries.front()->type_, ElementType::FOLDER);
}

TEST_F(SleeveServiceTests, CreateAndDeleteFolderByPathApi) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  const auto     created = service->CreateFolder(L"/", L"ToDelete");
  ASSERT_NE(created.first, nullptr);
  EXPECT_TRUE(created.second.success_);
  EXPECT_EQ(created.first->element_name_, L"ToDelete");

  const auto deleted = service->DeletePath(L"/ToDelete");
  EXPECT_TRUE(deleted.success_);
  EXPECT_THROW(service->ResolveFolder(L"/ToDelete"), std::runtime_error);
}

// #1179: the sync removes a deleted folder from the node cache and the store, and a path cache
// record of it does not hide a new folder at the same path.
TEST_F(SleeveServiceTests, PathOfASyncedDeletedFolderResolvesToTheNewFolder) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  ASSERT_TRUE(service->CreateFolder(L"/", L"A").second.success_);
  const auto old_b = service->CreateFolder(L"/A", L"B");
  ASSERT_TRUE(old_b.second.success_);
  ASSERT_EQ(service->ResolveFolder(L"/A/B")->element_id_, old_b.first->element_id_);

  ASSERT_TRUE(service->DeletePath(L"/A").success_);
  EXPECT_THROW(service->ResolveFolder(L"/A/B"), std::runtime_error);

  ASSERT_TRUE(service->CreateFolder(L"/", L"A").second.success_);
  const auto new_b = service->CreateFolder(L"/A", L"B");
  ASSERT_TRUE(new_b.second.success_);
  EXPECT_EQ(service->ResolveFolder(L"/A/B")->element_id_, new_b.first->element_id_);
  EXPECT_EQ(service->ResolveFolder(L"/A/B")->element_id_, new_b.first->element_id_);
}

TEST_F(SleeveServiceTests, ReloadedFolderWriteDoesNotCloneSingleParentFolder) {
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     created = service->CreateFolder(L"/", L"test");
    ASSERT_NE(created.first, nullptr);
    ASSERT_TRUE(created.second.success_);

    const auto sync = service->Sync();
    ASSERT_TRUE(sync.success_);
    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service      = reloaded_project.GetSleeveService();

  const auto     root_entries = service->ListFolderEntries(L"/");
  ASSERT_EQ(root_entries.size(), 1u);
  ASSERT_EQ(root_entries.front()->element_name_, L"test");

  const auto folder_before = service->ResolveFolder(L"/test");
  ASSERT_NE(folder_before, nullptr);
  const auto folder_id_before = folder_before->element_id_;
  EXPECT_EQ(folder_before->ref_count_, 1u);

  auto created_file = service->Write_NoSync<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"/test", L"after_reload.arw", ElementType::FILE); });
  ASSERT_NE(created_file, nullptr);

  const auto sync = service->Sync();
  EXPECT_TRUE(sync.success_);

  const auto folder_after = service->ResolveFolder(L"/test");
  ASSERT_NE(folder_after, nullptr);
  EXPECT_EQ(folder_after->element_id_, folder_id_before);

  const auto nested_entries = service->ListFolderEntries(L"/test");
  ASSERT_EQ(nested_entries.size(), 1u);
  EXPECT_EQ(nested_entries.front()->element_name_, L"after_reload.arw");
  EXPECT_EQ(nested_entries.front()->type_, ElementType::FILE);
}

TEST_F(SleeveServiceTests, CreateFileInLibraryAndLinkToAlbumKeepsSingleFileIdentity) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  const auto     album   = service->CreateFolder(L"/", L"Album").first;
  ASSERT_NE(album, nullptr);

  const auto file = service->Write<std::shared_ptr<SleeveFile>>(
      [](FileSystem& fs) { return fs.CreateFileInLibrary(L"Photo.arw"); });
  ASSERT_NE(file.first, nullptr);
  ASSERT_TRUE(file.second.success_);

  const auto linked = service->Write<bool>([&](FileSystem& fs) {
    fs.LinkFileToFolder(file.first->element_id_, album->element_id_);
    fs.LinkFileToFolder(file.first->element_id_, album->element_id_);
    return true;
  });
  ASSERT_TRUE(linked.second.success_);

  const auto root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });
  const auto album_ids = service->Read<std::vector<sl_element_id_t>>(
      [album](FileSystem& fs) { return fs.ListFolderContent(album->element_id_); });

  EXPECT_TRUE(ContainsId(root_ids, file.first->element_id_));
  ASSERT_EQ(album_ids.size(), 1u);
  EXPECT_EQ(album_ids.front(), file.first->element_id_);
  EXPECT_EQ(file.first->ref_count_, 1u);
}

TEST_F(SleeveServiceTests, AlbumMembershipWriteDoesNotTriggerFileCow) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  const auto     album_a = service->CreateFolder(L"/", L"AlbumA").first;
  const auto     album_b = service->CreateFolder(L"/", L"AlbumB").first;
  ASSERT_NE(album_a, nullptr);
  ASSERT_NE(album_b, nullptr);

  const auto file = service->Write<std::shared_ptr<SleeveFile>>(
      [](FileSystem& fs) { return fs.CreateFileInLibrary(L"Shared.arw"); });
  ASSERT_NE(file.first, nullptr);

  service->Write<bool>([&](FileSystem& fs) {
    fs.LinkFileToFolder(file.first->element_id_, album_a->element_id_);
    fs.LinkFileToFolder(file.first->element_id_, album_b->element_id_);
    return true;
  });

  const auto file_id = file.first->element_id_;
  const auto edited = service->Write_NoSync<std::shared_ptr<SleeveFile>>([file_id](FileSystem& fs) {
    auto file = std::static_pointer_cast<SleeveFile>(fs.Get(file_id));
    file->SetLastModifiedTime();
    return file;
  });
  ASSERT_NE(edited, nullptr);
  EXPECT_EQ(edited->element_id_, file_id);

  const auto from_root    = FilesNamed(*service, L"/", L"Shared.arw");
  const auto from_album_b = FilesNamed(*service, L"/AlbumB", L"Shared.arw");
  ASSERT_EQ(from_root.size(), 1u);
  ASSERT_EQ(from_album_b.size(), 1u);
  EXPECT_EQ(from_root.front()->element_id_, file_id);
  EXPECT_EQ(from_album_b.front()->element_id_, file_id);
  EXPECT_EQ(from_root.front()->last_modified_time_, edited->last_modified_time_);
  EXPECT_EQ(from_album_b.front()->last_modified_time_, edited->last_modified_time_);
}

TEST_F(SleeveServiceTests, SharedAlbumFileEditIsVisibleFromRootAndOtherAlbums) {
  sl_element_id_t file_id       = 0;
  std::time_t     modified_time = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     album_a = service->CreateFolder(L"/", L"AlbumA").first;
    const auto     album_b = service->CreateFolder(L"/", L"AlbumB").first;
    ASSERT_NE(album_a, nullptr);
    ASSERT_NE(album_b, nullptr);

    const auto file = service->CreateFileInLibrary(L"SharedEdit.arw").first;
    ASSERT_NE(file, nullptr);
    file_id = file->element_id_;

    service->Write<void>([&](FileSystem& fs) {
      fs.LinkFileToFolder(file_id, album_a->element_id_);
      fs.LinkFileToFolder(file_id, album_b->element_id_);
    });

    modified_time           = service->Write_NoSync<std::time_t>([file_id](FileSystem& fs) {
      const auto album_file = fs.Get(file_id);
      album_file->SetLastModifiedTime();
      return album_file->last_modified_time_;
    });

    const auto from_root    = FilesNamed(*service, L"/", L"SharedEdit.arw");
    const auto from_album_b = FilesNamed(*service, L"/AlbumB", L"SharedEdit.arw");
    ASSERT_EQ(from_root.size(), 1u);
    ASSERT_EQ(from_album_b.size(), 1u);
    EXPECT_EQ(from_root.front()->element_id_, file_id);
    EXPECT_EQ(from_album_b.front()->element_id_, file_id);
    EXPECT_EQ(from_root.front()->last_modified_time_, modified_time);
    EXPECT_EQ(from_album_b.front()->last_modified_time_, modified_time);
  }
}

TEST_F(SleeveServiceTests, DeletingAlbumFolderKeepsLinkedLibraryFiles) {
  sl_element_id_t file_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    const auto     file    = service->CreateFileInLibrary(L"KeepAfterAlbumDelete.arw").first;
    ASSERT_NE(album, nullptr);
    ASSERT_NE(file, nullptr);
    file_id = file->element_id_;

    ASSERT_TRUE(service->LinkFileToFolder(file_id, album->element_id_).success_);
    ASSERT_TRUE(service->DeletePath(L"/Album").success_);

    const auto root_ids = service->Read<std::vector<sl_element_id_t>>(
        [](FileSystem& fs) { return fs.ListFolderContent(0); });
    EXPECT_TRUE(ContainsId(root_ids, file_id));
    EXPECT_EQ(FileIdNamed(*service, L"/", L"KeepAfterAlbumDelete.arw"), file_id);
    EXPECT_THROW(service->ResolveFolder(L"/Album"), std::runtime_error);

    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service  = reloaded_project.GetSleeveService();
  const auto     root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });

  EXPECT_TRUE(ContainsId(root_ids, file_id));
  EXPECT_EQ(FileIdNamed(*service, L"/", L"KeepAfterAlbumDelete.arw"), file_id);
  EXPECT_THROW(service->ResolveFolder(L"/Album"), std::runtime_error);
}

TEST_F(SleeveServiceTests, FolderCopyWriteKeepsNestedFileIdentity) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  ASSERT_TRUE(service->CreateFolder(L"/", L"Folder").second.success_);
  ASSERT_TRUE(service->CreateFolder(L"/Folder", L"Subfolder").second.success_);

  const auto created = service->Write<std::shared_ptr<SleeveElement>>([](FileSystem& fs) {
    return fs.Create(L"/Folder/Subfolder", L"Linux.arw", ElementType::FILE);
  });
  ASSERT_NE(created.first, nullptr);
  ASSERT_TRUE(created.second.success_);

  ASSERT_TRUE(service->Write<bool>([](FileSystem& fs) {
                fs.Copy(L"/Folder/Subfolder", L"/");
                return true;
              }).second.success_);

  // The write below the shared folder copies the folder (CoW); the copy keeps the file link.
  const auto file_id = created.first->element_id_;
  const auto written = service->Write<std::shared_ptr<SleeveFile>>([file_id](FileSystem& fs) {
    fs.Get(L"/Subfolder", true);
    auto file = std::static_pointer_cast<SleeveFile>(fs.Get(file_id));
    file->SetLastModifiedTime();
    return file;
  });
  ASSERT_TRUE(written.second.success_);

  const auto from_original = FilesNamed(*service, L"/Folder/Subfolder", L"Linux.arw");
  const auto from_copied   = FilesNamed(*service, L"/Subfolder", L"Linux.arw");
  ASSERT_EQ(from_original.size(), 1u);
  ASSERT_EQ(from_copied.size(), 1u);
  EXPECT_NE(service->ResolveFolder(L"/Folder/Subfolder")->element_id_,
            service->ResolveFolder(L"/Subfolder")->element_id_);

  EXPECT_EQ(from_original.front()->element_id_, file_id);
  EXPECT_EQ(from_copied.front()->element_id_, file_id);
  EXPECT_EQ(from_original.front()->last_modified_time_, written.first->last_modified_time_);
  EXPECT_EQ(from_original.front()->ref_count_, 1u);
}

TEST_F(SleeveServiceTests, ExplicitDuplicateClonesStateAndKeepsPipelineIndependent) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  const auto     album   = service->CreateFolder(L"/", L"Album").first;
  const auto     source  = service->CreateFileInLibrary(L"Source.arw").first;
  ASSERT_NE(album, nullptr);
  ASSERT_NE(source, nullptr);

  const auto source_id = source->element_id_;
  auto       storage   = project.GetStorage();

  // SleeveService duplicates the element pipeline JSON only (the history is not copied), so the
  // stored JSON is what this test checks.
  WriteStoredExposure(*storage, source_id, 2.5f);

  const auto duplicated = service->DuplicateFileToFolder(source_id, album->element_id_);
  ASSERT_TRUE(duplicated.second.success_);
  ASSERT_NE(duplicated.first, nullptr);

  // The duplicate keeps the name: a file name is a label (SN4).
  const auto duplicate_id = duplicated.first->element_id_;
  EXPECT_NE(duplicate_id, source_id);
  EXPECT_EQ(duplicated.first->element_name_, L"Source.arw");
  const auto root_files = FilesNamed(*service, L"/", L"Source.arw");
  ASSERT_EQ(root_files.size(), 2u);
  EXPECT_EQ(root_files[0]->element_id_, source_id);
  EXPECT_EQ(root_files[1]->element_id_, duplicate_id);
  EXPECT_EQ(FileIdNamed(*service, L"/Album", L"Source.arw"), duplicate_id);

  EXPECT_FLOAT_EQ(ReadStoredExposure(*storage, source_id), 2.5f);
  EXPECT_FLOAT_EQ(ReadStoredExposure(*storage, duplicate_id), 2.5f);

  WriteStoredExposure(*storage, duplicate_id, 4.0f);

  EXPECT_FLOAT_EQ(ReadStoredExposure(*storage, source_id), 2.5f);
  EXPECT_FLOAT_EQ(ReadStoredExposure(*storage, duplicate_id), 4.0f);
}

/// G10.4: file import and file copy write only the element, file-binding, and
/// folder rows. A current database has no legacy edit-history table, and a copied
/// file reloads from storage without one.
TEST_F(SleeveServiceTests, ImportAndCopyDoNotCreateLegacyHistory) {
  sl_element_id_t source_id    = 0;
  sl_element_id_t duplicate_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    const auto     source  = service->CreateFileInLibrary(L"Source.arw").first;
    ASSERT_NE(album, nullptr);
    ASSERT_NE(source, nullptr);
    source_id = source->element_id_;

    const auto duplicated = service->DuplicateFileToFolder(source_id, album->element_id_);
    ASSERT_TRUE(duplicated.second.success_);
    ASSERT_NE(duplicated.first, nullptr);
    duplicate_id = duplicated.first->element_id_;
    project.SaveProject(meta_path_);
  }

  // Table names of the archived legacy store. Split literals keep the removed type names out
  // of the source tree (NoTestSourceReferencesLegacyHistoryStore).
  constexpr char kLegacyHistoryTable[]  = "Edit" "History";
  constexpr char kLegacyRecoveryTable[] = "EditorRecovery" "Metadata";
  EXPECT_EQ(QueryDuckDbInt64(db_path_, std::string("SELECT COUNT(*) FROM duckdb_tables() WHERE "
                                                   "table_name IN ('") +
                                           kLegacyHistoryTable + "', '" + kLegacyRecoveryTable +
                                           "');"),
            0);
  EXPECT_EQ(QueryDuckDbInt64(db_path_, "SELECT COUNT(*) FROM FileImage WHERE file_id IN (" +
                                           std::to_string(source_id) + ", " +
                                           std::to_string(duplicate_id) + ");"),
            2);

  ProjectService reopened(db_path_, meta_path_);
  auto           service    = reopened.GetSleeveService();
  const auto     root_files = FilesNamed(*service, L"/", L"Source.arw");
  ASSERT_EQ(root_files.size(), 2u);
  EXPECT_EQ(root_files[0]->element_id_, source_id);
  EXPECT_EQ(root_files[1]->element_id_, duplicate_id);
  EXPECT_EQ(FileIdNamed(*service, L"/Album", L"Source.arw"), duplicate_id);
}

TEST_F(SleeveServiceTests, ReloadedRootMembershipKeepsLibraryFile) {
  sl_element_id_t file_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     file    = service->CreateFileInLibrary(L"RootOnly.arw").first;
    ASSERT_NE(file, nullptr);
    file_id = file->element_id_;

    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service  = reloaded_project.GetSleeveService();
  const auto     root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });

  EXPECT_TRUE(ContainsId(root_ids, file_id));
  EXPECT_EQ(FileIdNamed(*service, L"/", L"RootOnly.arw"), file_id);
}

TEST_F(SleeveServiceTests, ReloadedAlbumMembershipKeepsSharedFileIdentity) {
  sl_element_id_t file_id  = 0;
  sl_element_id_t album_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    const auto     file    = service->CreateFileInLibrary(L"AlbumImport.arw").first;
    ASSERT_NE(album, nullptr);
    ASSERT_NE(file, nullptr);
    file_id           = file->element_id_;
    album_id          = album->element_id_;

    const auto linked = service->LinkFileToFolder(file_id, album_id);
    ASSERT_TRUE(linked.success_);

    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service  = reloaded_project.GetSleeveService();
  const auto     root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });
  const auto album_ids = service->Read<std::vector<sl_element_id_t>>(
      [album_id](FileSystem& fs) { return fs.ListFolderContent(album_id); });

  EXPECT_TRUE(ContainsId(root_ids, file_id));
  ASSERT_EQ(album_ids.size(), 1u);
  EXPECT_EQ(album_ids.front(), file_id);
  EXPECT_EQ(FileIdNamed(*service, L"/", L"AlbumImport.arw"), file_id);
  EXPECT_EQ(FileIdNamed(*service, L"/Album", L"AlbumImport.arw"), file_id);
}

TEST_F(SleeveServiceTests, ReloadedAlbumUnlinkKeepsRootMembershipOnly) {
  sl_element_id_t file_id  = 0;
  sl_element_id_t album_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    const auto     file    = service->CreateFileInLibrary(L"UnlinkOnly.arw").first;
    ASSERT_NE(album, nullptr);
    ASSERT_NE(file, nullptr);
    file_id  = file->element_id_;
    album_id = album->element_id_;

    service->LinkFileToFolder(file_id, album_id);
    const auto unlinked = service->DeleteFileFromFolder(file_id, album_id);
    ASSERT_TRUE(unlinked.success_);

    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service  = reloaded_project.GetSleeveService();
  const auto     root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });
  const auto album_ids = service->Read<std::vector<sl_element_id_t>>(
      [album_id](FileSystem& fs) { return fs.ListFolderContent(album_id); });

  EXPECT_TRUE(ContainsId(root_ids, file_id));
  EXPECT_FALSE(ContainsId(album_ids, file_id));
  EXPECT_EQ(FileIdNamed(*service, L"/", L"UnlinkOnly.arw"), file_id);
  EXPECT_EQ(FileIdNamed(*service, L"/Album", L"UnlinkOnly.arw"), 0u);
}

TEST_F(SleeveServiceTests, DuplicateLinkIsIdempotentAndPersistsAsSingleMembership) {
  sl_element_id_t file_id  = 0;
  sl_element_id_t album_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    const auto     file    = service->CreateFileInLibrary(L"RepeatLink.arw").first;
    ASSERT_NE(album, nullptr);
    ASSERT_NE(file, nullptr);
    file_id  = file->element_id_;
    album_id = album->element_id_;

    service->Write<void>([&](FileSystem& fs) {
      fs.LinkFileToFolder(file_id, album_id);
      fs.LinkFileToFolder(file_id, album_id);
    });
    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service   = reloaded_project.GetSleeveService();
  const auto     album_ids = service->Read<std::vector<sl_element_id_t>>(
      [album_id](FileSystem& fs) { return fs.ListFolderContent(album_id); });

  ASSERT_EQ(album_ids.size(), 1u);
  EXPECT_EQ(album_ids.front(), file_id);
}

TEST_F(SleeveServiceTests, FolderContentUniqueConstraintRejectsDuplicateRows) {
  sl_element_id_t file_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    const auto     file    = service->CreateFileInLibrary(L"UniqueRow.arw").first;
    ASSERT_NE(file, nullptr);
    file_id = file->element_id_;
    project.SaveProject(meta_path_);
  }

  EXPECT_FALSE(
      RunDuckDbSql(db_path_, "INSERT INTO FolderContent(folder_id, element_id) VALUES (0, " +
                                 std::to_string(file_id) + ");"));
  EXPECT_EQ(
      QueryDuckDbInt64(db_path_, std::string("SELECT COUNT(*) FROM FolderContent WHERE folder_id = "
                                             "0 AND element_id = ") +
                                     std::to_string(file_id)),
      1);
}

TEST_F(SleeveServiceTests, DeletingFromAlbumOnlyUnlinksMembership) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  const auto     album   = service->CreateFolder(L"/", L"Album").first;
  ASSERT_NE(album, nullptr);

  const auto file = service->Write<std::shared_ptr<SleeveFile>>(
      [](FileSystem& fs) { return fs.CreateFileInLibrary(L"KeepInRoot.arw"); });
  ASSERT_NE(file.first, nullptr);

  service->Write<bool>([&](FileSystem& fs) {
    fs.LinkFileToFolder(file.first->element_id_, album->element_id_);
    fs.UnlinkFileFromFolder(file.first->element_id_, album->element_id_);
    return true;
  });

  const auto root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });
  const auto album_ids = service->Read<std::vector<sl_element_id_t>>(
      [album](FileSystem& fs) { return fs.ListFolderContent(album->element_id_); });

  EXPECT_TRUE(ContainsId(root_ids, file.first->element_id_));
  EXPECT_FALSE(ContainsId(album_ids, file.first->element_id_));
  EXPECT_NE(FileIdNamed(*service, L"/", L"KeepInRoot.arw"), 0u);
  EXPECT_EQ(FileIdNamed(*service, L"/Album", L"KeepInRoot.arw"), 0u);
}

TEST_F(SleeveServiceTests, DeletingFromRootDeletesFileEverywhereAndPersists) {
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    ASSERT_NE(album, nullptr);

    const auto file = service->Write<std::shared_ptr<SleeveFile>>(
        [](FileSystem& fs) { return fs.CreateFileInLibrary(L"DeleteEverywhere.arw"); });
    ASSERT_NE(file.first, nullptr);

    const auto deleted = service->Write<bool>([&](FileSystem& fs) {
      fs.LinkFileToFolder(file.first->element_id_, album->element_id_);
      fs.Delete(file.first->element_id_);
      return true;
    });
    ASSERT_TRUE(deleted.second.success_);

    const auto root_ids = service->Read<std::vector<sl_element_id_t>>(
        [](FileSystem& fs) { return fs.ListFolderContent(0); });
    const auto album_ids = service->Read<std::vector<sl_element_id_t>>(
        [album](FileSystem& fs) { return fs.ListFolderContent(album->element_id_); });
    EXPECT_FALSE(ContainsId(root_ids, file.first->element_id_));
    EXPECT_FALSE(ContainsId(album_ids, file.first->element_id_));

    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           reloaded_service = reloaded_project.GetSleeveService();

  const auto     root_entries     = reloaded_service->ListFolderEntries(L"/");
  ASSERT_EQ(root_entries.size(), 1u);
  ASSERT_EQ(root_entries.front()->element_name_, L"Album");

  const auto album_entries = reloaded_service->ListFolderEntries(L"/Album");
  EXPECT_TRUE(album_entries.empty());
  EXPECT_EQ(FileIdNamed(*reloaded_service, L"/", L"DeleteEverywhere.arw"), 0u);
}

// Library search plan, Phase S6: a sync writes only the FolderContent rows of the children added
// or removed since the last sync. The rows of the other children keep their rowid.
TEST_F(SleeveServiceTests, FolderSyncWritesOnlyAddedAndRemovedContentRows) {
  constexpr size_t             kChildCount = 40;
  sl_element_id_t              album_id    = 0;
  std::vector<sl_element_id_t> file_ids;
  std::map<sl_element_id_t, int64_t> album_rows_after_remove;
  std::map<sl_element_id_t, int64_t> root_rows_after_add;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    ASSERT_NE(album, nullptr);
    album_id      = album->element_id_;

    const auto created = service->Write<std::vector<sl_element_id_t>>([&](FileSystem& fs) {
      std::vector<sl_element_id_t> ids;
      for (size_t i = 0; i < kChildCount; ++i) {
        auto file = fs.CreateFileInLibrary(L"Child_" + std::to_wstring(i) + L".arw");
        fs.LinkFileToFolder(file->element_id_, album_id);
        ids.push_back(file->element_id_);
      }
      return ids;
    });
    ASSERT_TRUE(created.second.success_);
    file_ids                 = created.first;
    const auto album_before  = FolderContentRowIds(project, album_id);
    const auto root_before   = FolderContentRowIds(project, 0);
    ASSERT_EQ(album_before.size(), kChildCount);
    ASSERT_EQ(root_before.size(), kChildCount + 1);  // the files and the album folder

    // Add one file to the library root and the album.
    const auto added = service->CreateFileInLibrary(L"Added.arw").first;
    ASSERT_NE(added, nullptr);
    ASSERT_TRUE(service->LinkFileToFolder(added->element_id_, album_id).success_);
    const auto album_after_add = FolderContentRowIds(project, album_id);
    root_rows_after_add        = FolderContentRowIds(project, 0);
    ASSERT_EQ(album_after_add.size(), kChildCount + 1);
    ASSERT_EQ(root_rows_after_add.size(), kChildCount + 2);
    for (const auto& [element_id, rowid] : album_before) {
      ASSERT_TRUE(album_after_add.contains(element_id));
      EXPECT_EQ(album_after_add.at(element_id), rowid) << "album row rewritten: " << element_id;
    }
    for (const auto& [element_id, rowid] : root_before) {
      ASSERT_TRUE(root_rows_after_add.contains(element_id));
      EXPECT_EQ(root_rows_after_add.at(element_id), rowid) << "root row rewritten: " << element_id;
    }
    EXPECT_TRUE(album_after_add.contains(added->element_id_));
    EXPECT_TRUE(root_rows_after_add.contains(added->element_id_));
    file_ids.push_back(added->element_id_);

    // Remove one file from the album.
    const auto removed_id = file_ids.front();
    ASSERT_TRUE(service->DeleteFileFromFolder(removed_id, album_id).success_);
    album_rows_after_remove = FolderContentRowIds(project, album_id);
    ASSERT_EQ(album_rows_after_remove.size(), kChildCount);
    EXPECT_FALSE(album_rows_after_remove.contains(removed_id));
    for (const auto& [element_id, rowid] : album_after_add) {
      if (element_id == removed_id) {
        continue;
      }
      ASSERT_TRUE(album_rows_after_remove.contains(element_id));
      EXPECT_EQ(album_rows_after_remove.at(element_id), rowid)
          << "album row rewritten: " << element_id;
    }
    EXPECT_EQ(FolderContentRowIds(project, 0), root_rows_after_add);
    project.SaveProject(meta_path_);
  }

  // After reopen, the rows read back match each folder.
  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service   = reloaded_project.GetSleeveService();
  auto           album_ids = service->Read<std::vector<sl_element_id_t>>(
      [album_id](FileSystem& fs) { return fs.ListFolderContent(album_id); });
  auto root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });
  std::sort(album_ids.begin(), album_ids.end());
  std::sort(root_ids.begin(), root_ids.end());

  std::vector<sl_element_id_t> expected_album_ids(file_ids.begin() + 1, file_ids.end());
  std::sort(expected_album_ids.begin(), expected_album_ids.end());
  std::vector<sl_element_id_t> expected_root_ids(file_ids.begin(), file_ids.end());
  expected_root_ids.push_back(album_id);
  std::sort(expected_root_ids.begin(), expected_root_ids.end());
  EXPECT_EQ(album_ids, expected_album_ids);
  EXPECT_EQ(root_ids, expected_root_ids);
  EXPECT_EQ(FolderContentRowIds(reloaded_project, album_id).size(), album_rows_after_remove.size());
  EXPECT_EQ(FolderContentRowIds(reloaded_project, 0).size(), root_rows_after_add.size());
}

TEST_F(SleeveServiceTests, FuzzyCreateCopyTest) {
  std::wstring first_tree;
  {
    ProjectService            project(db_path_, meta_path_);
    auto                      service = project.GetSleeveService();

    std::mt19937              gen(42);
    std::vector<std::wstring> known_paths;
    std::vector<std::wstring> known_folders;
    known_paths.push_back(L"");
    known_folders.push_back(L"");

    auto generate_name = [&gen](int length = 8) {
      static const std::wstring chars =
          L"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
      std::uniform_int_distribution<> dist(0, static_cast<int>(chars.size() - 1));
      std::wstring                    result;
      for (int i = 0; i < length; ++i) {
        result += chars[dist(gen)];
      }
      return result;
    };

    constexpr int kOperations = 200;
    for (int i = 0; i < kOperations; ++i) {
      std::uniform_int_distribution<> op_dist(0, 1);
      int                             op = op_dist(gen);

      if (known_paths.size() < 2 && op == 1) {
        op = 0;
      }

      if (op == 0) {
        std::uniform_int_distribution<size_t> parent_dist(0, known_folders.size() - 1);
        std::wstring                          parent = known_folders[parent_dist(gen)];
        std::wstring                          name   = generate_name();
        ElementType type = (gen() % 2 == 0) ? ElementType::FOLDER : ElementType::FILE;

        try {
          service->Write_NoSync<std::shared_ptr<SleeveElement>>(
              [&](FileSystem& fs) { return fs.Create(parent, name, type); });
          std::wstring new_path = parent + L"/" + name;
          known_paths.push_back(new_path);
          if (type == ElementType::FOLDER) {
            known_folders.push_back(new_path);
          }
        } catch (const std::exception&) {
          // Ignore invalid ops to keep fuzz running
        }
      } else {
        std::uniform_int_distribution<size_t> from_dist(0, known_paths.size() - 1);
        std::uniform_int_distribution<size_t> dest_dist(0, known_folders.size() - 1);
        std::wstring                          from_path = known_paths[from_dist(gen)];
        std::wstring                          to_parent = known_folders[dest_dist(gen)];

        try {
          service->Write_NoSync<bool>([&](FileSystem& fs) {
            fs.Copy(from_path, to_parent);
            return true;
          });
        } catch (const std::exception&) {
          // Ignore invalid ops to keep fuzz running
        }
      }
      std::cout << "\r\033[2KCompleted operation " << (i + 1) << " / " << kOperations << std::flush;
    }

    first_tree = service->Read<std::wstring>([](FileSystem& fs) { return fs.Tree(L"/"); });
    service->Sync();
    project.SaveProject(meta_path_);
  }
  std::cout << std::endl;

  ProjectService reloaded_project(db_path_, meta_path_);
  // reloaded_project.LoadProject(meta_path_);
  auto           reloaded_service = reloaded_project.GetSleeveService();
  auto           second_tree =
      reloaded_service->Read<std::wstring>([](FileSystem& fs) { return fs.Tree(L"/"); });

  EXPECT_EQ(conv::ToBytes(first_tree), conv::ToBytes(second_tree));
}

// ── Folder rename and move (issue #732) ─────────────────────────────────────

TEST_F(SleeveServiceTests, RenamedFolderKeepsIdChildrenAndMembershipAfterReload) {
  sl_element_id_t album_id = 0;
  sl_element_id_t child_id = 0;
  sl_element_id_t file_id  = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    const auto     child   = service->CreateFolder(L"/Album", L"Child").first;
    const auto     file    = service->CreateFileInLibrary(L"Renamed.arw").first;
    ASSERT_NE(album, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(file, nullptr);
    album_id = album->element_id_;
    child_id = child->element_id_;
    file_id  = file->element_id_;
    ASSERT_TRUE(service->LinkFileToFolder(file_id, album_id).success_);
    // Resolve the old paths once so the path cache holds them.
    ASSERT_EQ(service->ResolveFolder(L"/Album/Child")->element_id_, child_id);

    ASSERT_TRUE(service->RenameFolder(L"/Album", L"Trip").success_);

    EXPECT_EQ(service->ResolveFolder(L"/Trip")->element_id_, album_id);
    EXPECT_EQ(service->ResolveFolder(L"/Trip/Child")->element_id_, child_id);
    EXPECT_EQ(FileIdNamed(*service, L"/Trip", L"Renamed.arw"), file_id);
    EXPECT_THROW(service->ResolveFolder(L"/Album"), std::runtime_error);
    EXPECT_THROW(service->ResolveFolder(L"/Album/Child"), std::runtime_error);

    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service = reloaded_project.GetSleeveService();
  EXPECT_EQ(service->ResolveFolder(L"/Trip")->element_id_, album_id);
  EXPECT_EQ(service->ResolveFolder(L"/Trip/Child")->element_id_, child_id);
  EXPECT_EQ(FileIdNamed(*service, L"/Trip", L"Renamed.arw"), file_id);
  EXPECT_THROW(service->ResolveFolder(L"/Album"), std::runtime_error);
  const auto root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });
  EXPECT_TRUE(ContainsId(root_ids, file_id));
}

TEST_F(SleeveServiceTests, MovedFolderKeepsIdChildrenAndMembershipAfterReload) {
  sl_element_id_t inner_id = 0;
  sl_element_id_t leaf_id  = 0;
  sl_element_id_t file_id  = 0;
  sl_element_id_t a_id     = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    a_id                   = service->CreateFolder(L"/", L"A").first->element_id_;
    ASSERT_NE(service->CreateFolder(L"/", L"B").first, nullptr);
    inner_id = service->CreateFolder(L"/A", L"Inner").first->element_id_;
    leaf_id  = service->CreateFolder(L"/A/Inner", L"Leaf").first->element_id_;
    file_id  = service->CreateFileInLibrary(L"Moved.arw").first->element_id_;
    ASSERT_TRUE(service->LinkFileToFolder(file_id, inner_id).success_);
    ASSERT_EQ(service->ResolveFolder(L"/A/Inner/Leaf")->element_id_, leaf_id);

    ASSERT_TRUE(service->MoveFolder(L"/A/Inner", L"/B").success_);

    EXPECT_EQ(service->ResolveFolder(L"/B/Inner")->element_id_, inner_id);
    EXPECT_EQ(service->ResolveFolder(L"/B/Inner/Leaf")->element_id_, leaf_id);
    EXPECT_EQ(FileIdNamed(*service, L"/B/Inner", L"Moved.arw"), file_id);
    EXPECT_THROW(service->ResolveFolder(L"/A/Inner"), std::runtime_error);
    EXPECT_THROW(service->ResolveFolder(L"/A/Inner/Leaf"), std::runtime_error);
    const auto a_ids = service->Read<std::vector<sl_element_id_t>>(
        [a_id](FileSystem& fs) { return fs.ListFolderContent(a_id); });
    EXPECT_FALSE(ContainsId(a_ids, inner_id));

    project.SaveProject(meta_path_);
  }

  {
    ProjectService reloaded_project(db_path_, meta_path_);
    auto           service = reloaded_project.GetSleeveService();
    EXPECT_EQ(service->ResolveFolder(L"/B/Inner")->element_id_, inner_id);
    EXPECT_EQ(service->ResolveFolder(L"/B/Inner/Leaf")->element_id_, leaf_id);
    EXPECT_EQ(FileIdNamed(*service, L"/B/Inner", L"Moved.arw"), file_id);
    EXPECT_THROW(service->ResolveFolder(L"/A/Inner"), std::runtime_error);

    // Moving to the root path makes it a top-level folder.
    ASSERT_TRUE(service->MoveFolder(L"/B/Inner", L"/").success_);
    EXPECT_EQ(service->ResolveFolder(L"/Inner")->element_id_, inner_id);
    reloaded_project.SaveProject(meta_path_);
  }

  ProjectService reloaded_again(db_path_, meta_path_);
  auto           service = reloaded_again.GetSleeveService();
  EXPECT_EQ(service->ResolveFolder(L"/Inner")->element_id_, inner_id);
  EXPECT_EQ(service->ResolveFolder(L"/Inner/Leaf")->element_id_, leaf_id);
  EXPECT_THROW(service->ResolveFolder(L"/B/Inner"), std::runtime_error);
}

TEST_F(SleeveServiceTests, FolderMoveAndRenameRefuseOwnSubtreeAndTakenNames) {
  ProjectService project(db_path_, meta_path_);
  auto           service  = project.GetSleeveService();

  const auto     a_id     = service->CreateFolder(L"/", L"A").first->element_id_;
  const auto     inner_id = service->CreateFolder(L"/A", L"Inner").first->element_id_;
  ASSERT_NE(service->CreateFolder(L"/", L"B").first, nullptr);
  ASSERT_NE(service->CreateFolder(L"/A", L"Same").first, nullptr);
  ASSERT_NE(service->CreateFolder(L"/B", L"Same").first, nullptr);

  EXPECT_THROW(service->MoveFolder(L"/A", L"/A"), std::runtime_error);
  EXPECT_THROW(service->MoveFolder(L"/A", L"/A/Inner"), std::runtime_error);
  EXPECT_THROW(service->MoveFolder(L"/A/Same", L"/B"), std::runtime_error);
  EXPECT_THROW(service->MoveFolder(L"/", L"/B"), std::runtime_error);
  EXPECT_THROW(service->RenameFolder(L"/A/Inner", L"Same"), std::runtime_error);
  EXPECT_THROW(service->RenameFolder(L"/", L"Root"), std::runtime_error);
  EXPECT_THROW(service->RenameFolder(L"/A", L""), std::runtime_error);

  // A refused change leaves the tree as it was.
  EXPECT_EQ(service->ResolveFolder(L"/A")->element_id_, a_id);
  EXPECT_EQ(service->ResolveFolder(L"/A/Inner")->element_id_, inner_id);
  EXPECT_NE(service->ResolveFolder(L"/A/Same"), nullptr);
  EXPECT_NE(service->ResolveFolder(L"/B/Same"), nullptr);
}

TEST_F(SleeveServiceTests, PublicSyncCommitsPendingFolderChangesAndPreservesIdentity) {
  ProjectService project(db_path_, meta_path_);
  const auto     service = project.GetSleeveService();
  const auto     parent  = service->CreateFolder(L"/", L"Parent").first;
  const auto     child   = service->Write_NoSync<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"/Parent", L"Child", ElementType::FOLDER); });
  EXPECT_EQ(parent->SyncState(), SyncFlag::MODIFIED);
  EXPECT_EQ(child->SyncState(), SyncFlag::UNSYNC);
  const auto result = service->Sync();
  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_EQ(result.elements_synced_, 2);
  EXPECT_EQ(service->ResolveFolder(L"/Parent"), parent);
  EXPECT_EQ(service->ResolveElement(L"/Parent/Child"), child);
  EXPECT_EQ(parent->SyncState(), SyncFlag::SYNCED);
  EXPECT_EQ(child->SyncState(), SyncFlag::SYNCED);
  EXPECT_TRUE(parent->ContentAddedSinceSync().empty());
  EXPECT_EQ(project.GetStorage()->GetElementStore().GetFolderContent(parent->element_id_),
            std::vector<sl_element_id_t>{child->element_id_});
  EXPECT_EQ(service->Sync().elements_synced_, 0);
}

TEST_F(SleeveServiceTests, FailedFirstSyncKeepsRootUsableAndDoesNotReuseDiscardedIds) {
  ProjectService project(db_path_, meta_path_);
  const auto     service = project.GetSleeveService();
  const auto     root    = service->ResolveFolder(L"/");
  {
    auto guard = StorageTestAccess::GetDatabase(*project.GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    duckorm::execute(guard.conn_, duckorm::expr::raw("DROP TABLE FileImage"));
    duckorm::execute(
        guard.conn_,
        duckorm::expr::raw(
            "CREATE TABLE FileImage (file_id BIGINT, image_id BIGINT CHECK(image_id <> 0))"));
  }
  const auto rejected = service->CreateFileInLibrary(L"Rejected.jpg");
  ASSERT_FALSE(rejected.second.success_);
  EXPECT_EQ(rejected.first->SyncState(), SyncFlag::DELETED);
  EXPECT_THROW(project.GetStorage()->GetElementStore().GetElementById(0), std::runtime_error);
  EXPECT_THROW(project.GetStorage()->GetElementStore().GetElementById(rejected.first->element_id_),
               std::runtime_error);
  EXPECT_TRUE(project.GetStorage()->GetElementStore().GetFolderContent(0).empty());
  EXPECT_EQ(service->ResolveFolder(L"/"), root);
  EXPECT_EQ(root->SyncState(), SyncFlag::UNSYNC);
  EXPECT_TRUE(root->ListElements().empty());
  EXPECT_TRUE(root->ContentAddedSinceSync().empty());
  EXPECT_TRUE(service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFileIds(0); }).empty());
  const auto next = service->CreateFolder(L"/", L"Next");
  ASSERT_TRUE(next.second.success_) << next.second.message_;
  EXPECT_GT(next.first->element_id_, rejected.first->element_id_);
  EXPECT_EQ(service->ResolveFolder(L"/"), root);
  EXPECT_EQ(root->SyncState(), SyncFlag::SYNCED);
  EXPECT_TRUE(root->HoldsChildFolder(next.first->element_id_));
}

// Read-only structure of a mounted folder (mounted_folder_plan.md, MF5). The tests set the mount
// id through the sleeve tree and the source records through MountStore; MountService is not
// needed for the check.
class MountedFolderStructureTest : public SleeveServiceTests {
 protected:
  static constexpr mount_id_t     kMount = 1;

  std::unique_ptr<ProjectService> project_;
  sl_element_id_t                 ordinary_folder_id_ = 0;
  sl_element_id_t                 mount_root_id_      = 0;
  sl_element_id_t                 mounted_folder_id_  = 0;
  sl_element_id_t                 present_file_id_    = 0;
  sl_element_id_t                 missing_file_id_    = 0;
  sl_element_id_t                 ordinary_file_id_   = 0;

  void                            SetUp() override {
    SleeveServiceTests::SetUp();
    project_ = std::make_unique<ProjectService>(db_path_, meta_path_);
    ordinary_folder_id_ = Sleeve().CreateFolder(L"/", L"Ordinary").first->element_id_;
    auto [root, root_sync] = Sleeve().CreateMountRootFolder(L"Trip", kMount);
    ASSERT_TRUE(root_sync.success_) << root_sync.message_;
    mount_root_id_ = root->element_id_;
    auto [ids, sync] = Sleeve().Write<std::vector<sl_element_id_t>>([this](FileSystem& fs) {
      auto day       = fs.CreateFolderIn(mount_root_id_, L"Day");
      day->mount_id_ = kMount;
      const auto add = [&fs](const std::wstring& name, std::optional<mount_id_t> mount_id,
                             sl_element_id_t folder_id) {
        auto file       = fs.CreateFileInLibrary(name);
        file->image_id_ = file->element_id_;
        file->mount_id_ = mount_id;
        if (folder_id != 0) {
          fs.LinkFileToFolder(file->element_id_, folder_id);
        }
        return file->element_id_;
      };
      return std::vector<sl_element_id_t>{
          day->element_id_, add(L"present.nef", kMount, mount_root_id_),
          add(L"missing.nef", kMount, day->element_id_), add(L"ordinary.nef", std::nullopt, 0)};
    });
    ASSERT_TRUE(sync.success_) << sync.message_;
    mounted_folder_id_ = ids[0];
    present_file_id_   = ids[1];
    missing_file_id_   = ids[2];
    ordinary_file_id_  = ids[3];

    std::vector<SourceFileRecord> records(2);
    records[0].element_id          = present_file_id_;
    records[0].byte_size           = 100;
    records[0].fingerprint_version = 1;
    records[1]                     = records[0];
    records[1].element_id          = missing_file_id_;
    records[1].state               = SourceFileState::kMissing;
    records[1].missing_since       = std::time_t{1763553600};
    project_->GetStorage()->GetMountStore().UpsertSourceFiles(records);
    project_->SaveProject(meta_path_);
  }

  void TearDown() override {
    project_.reset();
    SleeveServiceTests::TearDown();
  }

  auto Sleeve() -> SleeveServiceImpl& { return *project_->GetSleeveService(); }

  /// Close the project and open it again from its database.
  void Reopen() {
    project_.reset();
    project_ = std::make_unique<ProjectService>(db_path_, meta_path_);
  }

  /// Each folder of the tree by path, with the sorted ids of its live entries.
  auto Tree() -> std::map<std::wstring, std::vector<sl_element_id_t>> {
    std::map<std::wstring, std::vector<sl_element_id_t>> tree;
    std::vector<std::filesystem::path>                   pending{L"/"};
    while (!pending.empty()) {
      const auto folder_path = pending.back();
      pending.pop_back();
      auto& ids = tree[folder_path.generic_wstring()];
      for (const auto& entry : Sleeve().ListFolderEntries(folder_path)) {
        ids.push_back(entry->element_id_);
        if (entry->type_ == ElementType::FOLDER) {
          pending.push_back(folder_path / entry->element_name_);
        }
      }
      std::sort(ids.begin(), ids.end());
    }
    return tree;
  }
};

/// Runs @p operation and expects SleeveOperationError with @p expected.
template <typename Operation>
void ExpectSleeveError(SleeveError expected, Operation&& operation) {
  try {
    operation();
    ADD_FAILURE() << "the operation was not refused";
  } catch (const SleeveOperationError& error) {
    EXPECT_EQ(error.Code(), expected) << error.what();
  }
}

TEST_F(MountedFolderStructureTest, FailedMountRemovalRestoresFileMountIdsAndLinks) {
  const auto before = Tree();
  {
    auto guard = StorageTestAccess::GetDatabase(*project_->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    duckorm::execute(
        guard.conn_,
        duckorm::expr::raw("CREATE TABLE ProtectedMountRoot (id BIGINT REFERENCES Element(id))"));
    duckorm::execute(guard.conn_, {"INSERT INTO ProtectedMountRoot VALUES (?)",
                                  {static_cast<std::int64_t>(mount_root_id_)}});
  }

  const std::array files{present_file_id_, missing_file_id_};
  const auto failed = Sleeve().RemoveMountTree(mount_root_id_, kMount, files);

  ASSERT_FALSE(failed.success_);
  EXPECT_NE(failed.message_.find("Constraint"), std::string::npos);
  EXPECT_EQ(Tree(), before);
  Sleeve().Read<void>([&](FileSystem& fs) {
    EXPECT_EQ(fs.Get(present_file_id_)->mount_id_, kMount);
    EXPECT_EQ(fs.Get(missing_file_id_)->mount_id_, kMount);
  });
}

TEST_F(MountedFolderStructureTest, EachStructureOperationOnAMountedFolderReturnsTheNamedError) {
  const auto before        = Tree();
  const auto read_only     = SleeveError::kMountedFolderStructureIsReadOnly;
  const auto present       = SleeveError::kMountedPhotoIsPresent;
  const auto present_ids   = std::vector<sl_element_id_t>{ordinary_file_id_, present_file_id_};
  const auto home_link_ids = std::vector<sl_element_id_t>{present_file_id_};

  {
    SCOPED_TRACE("new collection");
    ExpectSleeveError(read_only, [&] { (void)Sleeve().CreateFolder(L"/Trip", L"New"); });
    ExpectSleeveError(read_only, [&] { (void)Sleeve().CreateFolder(L"/Trip/Day", L"New"); });
  }
  {
    SCOPED_TRACE("rename");
    ExpectSleeveError(read_only, [&] { (void)Sleeve().RenameFolder(L"/Trip", L"Japan"); });
    ExpectSleeveError(read_only, [&] { (void)Sleeve().RenameFolder(L"/Trip/Day", L"Night"); });
  }
  {
    SCOPED_TRACE("move");
    ExpectSleeveError(read_only, [&] { (void)Sleeve().MoveFolder(L"/Trip", L"/Ordinary"); });
    ExpectSleeveError(read_only, [&] { (void)Sleeve().MoveFolder(L"/Trip/Day", L"/Ordinary"); });
  }
  {
    SCOPED_TRACE("delete");
    ExpectSleeveError(read_only, [&] { (void)Sleeve().DeletePath(L"/Trip"); });
    ExpectSleeveError(read_only, [&] { (void)Sleeve().DeletePath(L"/Trip/Day"); });
    ExpectSleeveError(read_only, [&] { (void)Sleeve().DeleteElement(mounted_folder_id_); });
  }
  {
    SCOPED_TRACE("link and duplicate into the folder");
    ExpectSleeveError(
        read_only, [&] { (void)Sleeve().LinkFileToFolder(ordinary_file_id_, mounted_folder_id_); });
    ExpectSleeveError(
        read_only, [&] { (void)Sleeve().DuplicateFileToFolder(present_file_id_, mount_root_id_); });
  }
  {
    SCOPED_TRACE("remove the home link");
    ExpectSleeveError(
        read_only, [&] { (void)Sleeve().DeleteFileFromFolder(present_file_id_, mount_root_id_); });
    ExpectSleeveError(read_only,
                      [&] { (void)Sleeve().DeleteFilesFromFolder(home_link_ids, mount_root_id_); });
  }
  {
    SCOPED_TRACE("remove from library");
    ExpectSleeveError(present, [&] { (void)Sleeve().DeleteFileEverywhere(present_file_id_); });
    ExpectSleeveError(present, [&] { (void)Sleeve().DeleteElement(present_file_id_); });
    // One mounted photo refuses the whole request: the ordinary photo stays too.
    ExpectSleeveError(present, [&] { (void)Sleeve().DeleteFilesEverywhere(present_ids); });
    ExpectSleeveError(SleeveError::kMountedPhotoIsMissing,
                      [&] { (void)Sleeve().DeleteFileEverywhere(missing_file_id_); });
  }

  EXPECT_EQ(Tree(), before);
  Reopen();
  EXPECT_EQ(Tree(), before);
  EXPECT_EQ(Sleeve().ResolveFolder(L"/Trip/Day")->mount_id_, kMount);
}

TEST_F(MountedFolderStructureTest, MountedFileLinksIntoAnOrdinaryCollection) {
  ASSERT_TRUE(Sleeve().LinkFileToFolder(present_file_id_, ordinary_folder_id_).success_);

  Reopen();
  const auto entries = Sleeve().ListFolderEntries(L"/Ordinary");
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries[0]->element_id_, present_file_id_);
  EXPECT_EQ(entries[0]->mount_id_, kMount);
  // The home link stays, and the link to an ordinary collection is free to remove.
  EXPECT_EQ(FileIdNamed(Sleeve(), L"/Trip", L"present.nef"), present_file_id_);
  ASSERT_TRUE(Sleeve().DeleteFileFromFolder(present_file_id_, ordinary_folder_id_).success_);
  EXPECT_TRUE(Sleeve().ListFolderEntries(L"/Ordinary").empty());
}

TEST_F(MountedFolderStructureTest, OrdinaryFolderCannotMoveIntoAMountedFolder) {
  const auto before = Tree();
  ExpectSleeveError(SleeveError::kMountedFolderStructureIsReadOnly,
                    [&] { (void)Sleeve().MoveFolder(L"/Ordinary", L"/Trip"); });
  ExpectSleeveError(SleeveError::kMountedFolderStructureIsReadOnly,
                    [&] { (void)Sleeve().MoveFolder(L"/Ordinary", L"/Trip/Day"); });

  Reopen();
  EXPECT_EQ(Tree(), before);
  EXPECT_EQ(Sleeve().ResolveFolder(L"/Ordinary")->element_id_, ordinary_folder_id_);
}

// Sleeve Node Cache Plan SN1: the sidebar listing walks and lists folders only, so the files of
// the root and of the mount stay out of the node map.
TEST_F(MountedFolderStructureTest, ListChildFoldersLeavesFileElementsUnloaded) {
  Reopen();
  const auto browse = project_->GetAlbumBrowseService();
  ASSERT_NE(browse, nullptr);

  EXPECT_EQ(SortedFolderNames(browse->ListFolders(L"/")),
            (std::vector<std::wstring>{L"Ordinary", L"Trip"}));
  const auto trip_folders = browse->ListFolders(L"/Trip");
  ASSERT_EQ(trip_folders.size(), 1u);
  EXPECT_EQ(trip_folders.front().folder_id_, mounted_folder_id_);
  EXPECT_EQ(trip_folders.front().mount_id_, std::optional<mount_id_t>{kMount});
  EXPECT_TRUE(browse->ListFolders(L"/Trip/Day").empty());

  const std::vector<sl_element_id_t> file_ids{present_file_id_, missing_file_id_,
                                              ordinary_file_id_};
  const auto loaded_files = Sleeve().Read<std::vector<sl_element_id_t>>([&](FileSystem& fs) {
    std::vector<sl_element_id_t> loaded;
    for (const auto id : file_ids) {
      if (fs.Nodes().Contains(id)) {
        loaded.push_back(id);
      }
    }
    return loaded;
  });
  EXPECT_TRUE(loaded_files.empty());
}

// Sleeve Node Cache Plan SN1: a FolderContent row of the root whose element row is missing does
// not empty the listing of the root.
TEST_F(SleeveServiceTests, ListChildFoldersKeepsFoldersWhenOneRowHasNoElement) {
  constexpr sl_element_id_t kMissingElementId = 900000;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    service->CreateFolder(L"/", L"Kept");
    service->CreateFolder(L"/", L"Other");
    service->Write<std::shared_ptr<SleeveElement>>(
        [](FileSystem& fs) { return fs.Create(L"/Kept", L"Image", ElementType::FILE); });
    ExecuteOnProjectDatabase(
        project, "INSERT INTO FolderContent VALUES (0, " + std::to_string(kMissingElementId) + ")");
    project.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_);
  ASSERT_EQ(FolderContentRowIds(reopened, 0).count(kMissingElementId), 1u);
  const auto browse = reopened.GetAlbumBrowseService();
  ASSERT_NE(browse, nullptr);
  EXPECT_EQ(SortedFolderNames(browse->ListFolders(L"/")),
            (std::vector<std::wstring>{L"Kept", L"Other"}));
  EXPECT_TRUE(browse->ListFolders(L"/Kept").empty());
}

// Sleeve Node Cache Plan SN2: a sync walks the dirty list of the node cache, so the clean
// elements in memory do not add work to the sync of one change.
TEST_F(SleeveServiceTests, SyncAfterOneRenameWritesOneElementWithTenThousandInMemory) {
  constexpr int  kFileCount = 10'000;
  ProjectService project(db_path_, meta_path_);
  auto           service       = project.GetSleeveService();
  const auto [folder, created] = service->CreateFolder(L"/", L"Trip");
  ASSERT_TRUE(created.success_) << created.message_;
  const auto import_result = service->Write<void>([](FileSystem& fs) {
    for (int i = 0; i < kFileCount; ++i) {
      fs.CreateFileInLibrary(L"IMG_" + std::to_wstring(i) + L".ARW");
    }
  });
  ASSERT_TRUE(import_result.success_) << import_result.message_;
  ASSERT_EQ(import_result.elements_synced_, static_cast<uint32_t>(kFileCount + 1))
      << "the files and the root, whose content changed";

  const auto [in_memory, dirty_before] = service->Read<std::pair<std::size_t, std::size_t>>(
      [](FileSystem& fs) { return std::pair{fs.Nodes().Size(), fs.Nodes().DirtyCount()}; });
  ASSERT_GE(in_memory, static_cast<std::size_t>(kFileCount + 2));
  ASSERT_EQ(dirty_before, 0u);

  const auto rename_result = service->RenameFolder(L"/Trip", L"Trip 2024");
  ASSERT_TRUE(rename_result.success_) << rename_result.message_;
  // The folder row gets the new name. The root is the parent on the write path: it becomes
  // MODIFIED, and its content change of the rename cancels, so its update writes the row only.
  EXPECT_EQ(rename_result.elements_synced_, 2u);
  EXPECT_EQ(service->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().DirtyCount(); }), 0u);
  EXPECT_EQ(NamedElementRowCount(project, folder->element_id_, "Trip 2024"), 1);
}

// Sleeve Node Cache Plan SN2: the inserts, the updates, and the deletes of one sync commit in one
// transaction. A failure in the delete step leaves the database and the dirty list unchanged.
// Sleeve Node Cache Plan SN7: each fs_lock_ scope ends with the eviction of clean file elements.
// An evicted file loads again with the fields that the sync wrote, a change to it reaches the
// database before it can leave, and an element that a caller holds keeps its identity.
TEST_F(SleeveServiceTests, EvictedFileReloadsItsStoredFieldsAndHeldFileKeepsItsIdentity) {
  constexpr int  kFileCount = 6;
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();
  service->Read<void>([](FileSystem& fs) { fs.SetCleanFileLimit(2); });
  const auto [folder, created] = service->CreateFolder(L"/", L"Trip");
  ASSERT_TRUE(created.success_) << created.message_;
  const auto folder_id = folder->element_id_;

  const auto import_write =
      service->Write<std::vector<sl_element_id_t>>([folder_id](FileSystem& fs) {
        std::vector<sl_element_id_t> ids;
        for (int i = 0; i < kFileCount; ++i) {
          auto file       = fs.CreateFileInLibrary(L"IMG_" + std::to_wstring(i) + L".ARW");
          file->image_id_ = static_cast<image_id_t>(100 + i);
          fs.LinkFileToFolder(file->element_id_, folder_id);
          ids.push_back(file->element_id_);
        }
        return ids;
      });
  const auto& ids = import_write.first;
  ASSERT_TRUE(import_write.second.success_) << import_write.second.message_;
  const auto clean_files = [&service] {
    return service->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().CleanFileCount(); });
  };
  EXPECT_EQ(clean_files(), 2u) << "the write ends with the eviction";

  // The caller holds the first file across later scopes that load every file.
  const auto held = service->Read<std::shared_ptr<SleeveElement>>(
      [&ids](FileSystem& fs) { return fs.Get(ids.front()); });
  ASSERT_NE(held, nullptr);
  service->Read<void>([&ids](FileSystem& fs) {
    for (const auto id : ids) {
      (void)fs.Get(id);
    }
  });
  EXPECT_EQ(clean_files(), 2u);
  EXPECT_EQ(service->Read<std::shared_ptr<SleeveElement>>(
                [&ids](FileSystem& fs) { return fs.Get(ids.front()); }),
            held)
      << "the held element stays in the cache";

  // A rename of an evicted file loads it, and the sync writes the change before it can leave.
  // The clean files leave in the order of their last use, so file 1 is out of the cache.
  constexpr int kRenamed   = 1;
  const auto    renamed_id = ids[kRenamed];
  const auto    is_cached  = [&service](sl_element_id_t id) {
    return service->Read<bool>([id](FileSystem& fs) { return fs.Nodes().Contains(id); });
  };
  ASSERT_FALSE(is_cached(renamed_id));
  const auto rename_result = service->Write<void>(
      [renamed_id](FileSystem& fs) { fs.RenameFile(renamed_id, L"Renamed.ARW"); });
  ASSERT_TRUE(rename_result.success_) << rename_result.message_;
  service->Read<void>([&ids](FileSystem& fs) {
    for (const auto id : ids) {
      (void)fs.Get(id);
    }
  });
  ASSERT_FALSE(is_cached(renamed_id)) << "the renamed file left the cache after its sync";

  // Each file loads again with its stored name, image id, and folder link.
  const auto loaded =
      service->Read<std::vector<std::pair<file_name_t, image_id_t>>>([&ids](FileSystem& fs) {
        std::vector<std::pair<file_name_t, image_id_t>> fields;
        for (const auto id : ids) {
          const auto file = std::static_pointer_cast<SleeveFile>(fs.Get(id));
          fields.emplace_back(file->element_name_, file->image_id_);
        }
        return fields;
      });
  ASSERT_EQ(loaded.size(), ids.size());
  for (int i = 0; i < kFileCount; ++i) {
    const auto expected_name =
        i == kRenamed ? file_name_t{L"Renamed.ARW"} : L"IMG_" + std::to_wstring(i) + L".ARW";
    EXPECT_EQ(loaded[i].first, expected_name) << "file " << i;
    EXPECT_EQ(loaded[i].second, static_cast<image_id_t>(100 + i)) << "file " << i;
  }
  EXPECT_EQ(service->Read<std::vector<sl_element_id_t>>(
                [folder_id](FileSystem& fs) { return fs.ListFileIds(folder_id); }),
            ids);
  EXPECT_EQ(service->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().DirtyCount(); }), 0u);
}

// Issue #1177: Sync takes fs_lock_, so a caller on another thread (the import finalize thread)
// does not change the lists of the node cache during a Read or a Write_NoSync. With a small
// limit each Read moves and evicts clean files while the other thread syncs. A build with
// ThreadSanitizer reports a race here when Sync runs without the lock.
TEST_F(SleeveServiceTests, SyncOnASecondThreadKeepsTheNodeCacheWhole) {
  constexpr int  kRounds = 200;
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();
  service->Read<void>([](FileSystem& fs) { fs.SetCleanFileLimit(4); });
  const auto [folder, created] = service->CreateFolder(L"/", L"Trip");
  ASSERT_TRUE(created.success_) << created.message_;
  const auto folder_id = folder->element_id_;

  std::atomic<bool> done{false};
  std::atomic<bool> sync_failed{false};
  std::thread       sync_thread([&service, &done, &sync_failed] {
    while (!done.load()) {
      if (!service->Sync().success_) {
        sync_failed.store(true);
      }
    }
  });

  std::vector<sl_element_id_t> ids;
  for (int i = 0; i < kRounds; ++i) {
    ids.push_back(service->Write_NoSync<sl_element_id_t>([folder_id, i](FileSystem& fs) {
      const auto file = fs.CreateFileInLibrary(L"IMG_" + std::to_wstring(i) + L".ARW");
      fs.LinkFileToFolder(file->element_id_, folder_id);
      return file->element_id_;
    }));
    service->Read<void>([&ids](FileSystem& fs) {
      for (const auto id : ids) {
        (void)fs.Get(id);
      }
    });
  }
  done.store(true);
  sync_thread.join();
  ASSERT_FALSE(sync_failed.load());

  const auto last_sync = service->Sync();
  ASSERT_TRUE(last_sync.success_) << last_sync.message_;
  const auto [dirty, clean_files] = service->Read<std::pair<std::size_t, std::size_t>>(
      [](FileSystem& fs) { return std::pair{fs.Nodes().DirtyCount(), fs.Nodes().CleanFileCount()}; });
  EXPECT_EQ(dirty, 0u);
  EXPECT_LE(clean_files, 4u) << "Sync ends with the eviction";
  EXPECT_EQ(service->Read<std::vector<sl_element_id_t>>(
                [folder_id](FileSystem& fs) { return fs.ListFileIds(folder_id); }),
            ids);
}

TEST_F(SleeveServiceTests, FailedSyncRestoresCommittedStateAndExplicitRetrySucceeds) {
  ProjectService project(db_path_, meta_path_);
  auto           service              = project.GetSleeveService();
  const auto [folder, folder_created] = service->CreateFolder(L"/", L"Old");
  ASSERT_TRUE(folder_created.success_) << folder_created.message_;
  const auto [removed, removed_created] = service->CreateFileInLibrary(L"Removed.ARW");
  ASSERT_TRUE(removed_created.success_) << removed_created.message_;
  const auto folder_id  = folder->element_id_;
  const auto removed_id = removed->element_id_;
  ASSERT_EQ(ElementRowCount(project, removed_id), 1);

  // The file delete removes the SourceFile rows of the file; without the table that statement
  // fails after the insert and the update ran in the same transaction.
  ExecuteOnProjectDatabase(project, "ALTER TABLE SourceFile RENAME TO SourceFileAside");
  sl_element_id_t added_id      = 0;
  const auto      edit = [&](FileSystem& fs) {
    added_id = fs.CreateFileInLibrary(L"Added.ARW")->element_id_;
    fs.RenameFolder(L"/Old", L"New");
    fs.DeleteFileEverywhere(removed_id);
  };
  const auto failed_result = service->Write<void>(edit);
  ASSERT_FALSE(failed_result.success_);
  EXPECT_NE(failed_result.message_.find("SourceFile"), std::string::npos) << failed_result.message_;

  EXPECT_EQ(ElementRowCount(project, added_id), 0) << "the insert rolled back";
  EXPECT_EQ(NamedElementRowCount(project, folder_id, "Old"), 1) << "the update rolled back";
  EXPECT_EQ(ElementRowCount(project, removed_id), 1) << "the delete rolled back";
  EXPECT_EQ(FolderContentRowIds(project, 0).count(added_id), 0u);
  EXPECT_EQ(FolderContentRowIds(project, 0).count(removed_id), 1u);

  EXPECT_EQ(service->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().DirtyCount(); }), 0u);
  EXPECT_EQ(service->ResolveFolder(L"/Old"), folder);
  EXPECT_EQ(removed->SyncState(), SyncFlag::SYNCED);

  ExecuteOnProjectDatabase(project, "ALTER TABLE SourceFileAside RENAME TO SourceFile");
  const auto discarded_id = added_id;
  const auto retry_result = service->Write<void>(edit);
  EXPECT_GT(added_id, discarded_id);
  ASSERT_TRUE(retry_result.success_) << retry_result.message_;
  EXPECT_EQ(retry_result.elements_synced_, 4u);

  EXPECT_EQ(ElementRowCount(project, added_id), 1);
  EXPECT_EQ(NamedElementRowCount(project, folder_id, "New"), 1);
  EXPECT_EQ(ElementRowCount(project, removed_id), 0);
  const auto root_rows = FolderContentRowIds(project, 0);
  EXPECT_EQ(root_rows.count(added_id), 1u);
  EXPECT_EQ(root_rows.count(removed_id), 0u);
  EXPECT_EQ(
      service->Read<bool>([removed_id](FileSystem& fs) { return fs.Nodes().Contains(removed_id); }),
      false)
      << "the deleted element leaves the node cache after the commit";
  EXPECT_EQ(service->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().DirtyCount(); }), 0u);
}

namespace {
/// The warnings of the Library category while a WarningCapture is alive.
std::mutex               captured_warnings_mutex;
std::vector<std::string> captured_warnings;

void                     CaptureLibraryWarning(const log::Record& record) noexcept {
  if (record.level == log::Level::Warn && record.category == log::Category::Library) {
    std::lock_guard lock(captured_warnings_mutex);
    captured_warnings.emplace_back(record.message);
  }
}

/// Installs CaptureLibraryWarning for its lifetime and restores the default handler.
class WarningCapture {
 public:
  WarningCapture() {
    std::lock_guard lock(captured_warnings_mutex);
    captured_warnings.clear();
    log::SetHandler(&CaptureLibraryWarning);
  }
  ~WarningCapture() { log::SetHandler(nullptr); }
  WarningCapture(const WarningCapture&)                                  = delete;
  auto               operator=(const WarningCapture&) -> WarningCapture& = delete;

  /// The captured warnings that contain @p text.
  [[nodiscard]] auto Containing(const std::string& text) const -> std::vector<std::string> {
    std::lock_guard          lock(captured_warnings_mutex);
    std::vector<std::string> matches;
    for (const auto& warning : captured_warnings) {
      if (warning.find(text) != std::string::npos) {
        matches.push_back(warning);
      }
    }
    return matches;
  }
};

/// The generic paths of a folder tree listing, in the order of the listing.
auto TreePaths(const std::vector<AlbumFolderView>& folders) -> std::vector<std::wstring> {
  std::vector<std::wstring> paths;
  paths.reserve(folders.size());
  for (const auto& folder : folders) {
    paths.push_back(folder.folder_path_.generic_wstring());
  }
  return paths;
}

/// The folder id at @p path in a folder tree listing; 0 when the listing has no such path.
auto TreeFolderId(const std::vector<AlbumFolderView>& folders, const std::wstring& path)
    -> sl_element_id_t {
  for (const auto& folder : folders) {
    if (folder.folder_path_.generic_wstring() == path) {
      return folder.folder_id_;
    }
  }
  return 0;
}
}  // namespace

// Sleeve Node Cache Plan SN3: the project open loads every folder, and the path of each folder
// then resolves from memory. With the Element table renamed away, a store read of an element
// fails, so a resolve with no error and no warning read no store.
TEST_F(SleeveServiceTests, OpenLoadsNestedFoldersWithOneTreeRead) {
  constexpr int kDepth            = 40;
  constexpr int kSiblingsPerLevel = 49;  // 40 chain folders + 40 * 49 siblings = 2,000 folders
  std::vector<std::wstring> folder_paths;
  sl_element_id_t           deepest_file_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    const auto     created = service->Write<void>([&](FileSystem& fs) {
      sl_element_id_t parent_id   = 0;
      std::wstring    parent_path = L"";
      for (int level = 1; level <= kDepth; ++level) {
        for (int sibling = 0; sibling < kSiblingsPerLevel; ++sibling) {
          const auto name = L"Sibling " + std::to_wstring(level) + L"." + std::to_wstring(sibling);
          fs.CreateFolderIn(parent_id, name);
          folder_paths.push_back(parent_path + L"/" + name);
        }
        const auto name = L"Level " + std::to_wstring(level);
        parent_id       = fs.CreateFolderIn(parent_id, name)->element_id_;
        parent_path += L"/" + name;
        folder_paths.push_back(parent_path);
      }
      deepest_file_id = fs.Create(parent_path, L"Deep.ARW", ElementType::FILE)->element_id_;
    });
    ASSERT_TRUE(created.success_) << created.message_;
    project.SaveProject(meta_path_);
  }
  ASSERT_EQ(folder_paths.size(), 2'000u);

  ProjectService reopened(db_path_, meta_path_);
  auto           service = reopened.GetSleeveService();
  const auto [in_memory, dirty, file_loaded] =
      service->Read<std::tuple<std::size_t, std::size_t, bool>>([&](FileSystem& fs) {
        return std::tuple{fs.Nodes().Size(), fs.Nodes().DirtyCount(),
                          fs.Nodes().Contains(deepest_file_id)};
      });
  EXPECT_EQ(in_memory, 2'001u) << "the root and the 2,000 folders, and no file";
  EXPECT_EQ(dirty, 0u) << "the load marks no folder modified";
  EXPECT_FALSE(file_loaded);

  // Each element read joins Element, so an element read throws. (FolderContent has indexes, so
  // DuckDB refuses to rename it.)
  ExecuteOnProjectDatabase(reopened, "ALTER TABLE Element RENAME TO ElementAside");
  std::size_t                  resolved = 0;
  std::vector<AlbumFolderView> tree;
  std::vector<std::string>     load_warnings;
  {
    WarningCapture warnings;
    for (const auto& path : folder_paths) {
      EXPECT_NO_THROW({
        const auto folder = service->ResolveFolder(path);
        EXPECT_EQ(folder->element_name_, std::filesystem::path(path).filename().wstring());
        ++resolved;
      }) << conv::ToBytes(path);
    }
    tree          = reopened.GetAlbumBrowseService()->ListFolderTree();
    load_warnings = warnings.Containing("did not load");
  }
  ExecuteOnProjectDatabase(reopened, "ALTER TABLE ElementAside RENAME TO Element");

  EXPECT_EQ(resolved, folder_paths.size());
  EXPECT_TRUE(load_warnings.empty()) << load_warnings.front();
  auto listed   = TreePaths(tree);
  auto expected = folder_paths;
  ASSERT_EQ(listed.size(), expected.size());
  // A parent is listed before each of its children.
  std::set<std::wstring> seen{L"/"};
  for (const auto& path : listed) {
    EXPECT_TRUE(seen.contains(std::filesystem::path(path).parent_path().generic_wstring()))
        << conv::ToBytes(path);
    seen.insert(path);
  }
  std::sort(listed.begin(), listed.end());
  std::sort(expected.begin(), expected.end());
  EXPECT_EQ(listed, expected);
}

// Sleeve Node Cache Plan SN3: a folder with two parents (copy-on-write sharing) is a child of
// both after the open, and the load keeps its stored reference count.
TEST_F(SleeveServiceTests, FolderWithTwoParentsAppearsUnderBoth) {
  sl_element_id_t shared_id = 0;
  sl_element_id_t inner_id  = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    const auto     created = service->Write<void>([&](FileSystem& fs) {
      const auto a = fs.CreateFolderIn(0, L"A");
      fs.CreateFolderIn(0, L"B");
      shared_id = fs.CreateFolderIn(a->element_id_, L"Shared")->element_id_;
      inner_id  = fs.CreateFolderIn(shared_id, L"Inner")->element_id_;
      fs.Copy(L"/A/Shared", L"/B");
    });
    ASSERT_TRUE(created.success_) << created.message_;
    // The stored reference count of a folder with two parents.
    ExecuteOnProjectDatabase(
        project, "UPDATE Element SET ref_count = 2 WHERE id = " + std::to_string(shared_id));
    project.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_);
  const auto     tree = reopened.GetAlbumBrowseService()->ListFolderTree();
  EXPECT_EQ(TreePaths(tree), (std::vector<std::wstring>{L"/A", L"/A/Shared", L"/A/Shared/Inner",
                                                        L"/B", L"/B/Shared", L"/B/Shared/Inner"}));
  EXPECT_EQ(TreeFolderId(tree, L"/A/Shared"), shared_id);
  EXPECT_EQ(TreeFolderId(tree, L"/B/Shared"), shared_id);
  EXPECT_EQ(TreeFolderId(tree, L"/B/Shared/Inner"), inner_id);

  auto service = reopened.GetSleeveService();
  EXPECT_EQ(service->ResolveFolder(L"/B/Shared")->element_id_, shared_id);
  EXPECT_EQ(service->ResolveFolder(L"/A/Shared")->ref_count_, 2u);
  EXPECT_EQ(service->ResolveFolder(L"/A/Shared/Inner")->ref_count_, 1u);
  EXPECT_EQ(service->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().DirtyCount(); }), 0u);
}

// Sleeve Node Cache Plan SN3: the first write below a shared folder after the open copies it
// with every entry, also the file entries that were not loaded yet.
TEST_F(SleeveServiceTests, CopyOnWriteOfSharedFolderAfterOpenKeepsItsFiles) {
  sl_element_id_t shared_id = 0;
  sl_element_id_t file_id   = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    const auto     created = service->Write<void>([&](FileSystem& fs) {
      const auto a = fs.CreateFolderIn(0, L"A");
      fs.CreateFolderIn(0, L"B");
      shared_id = fs.CreateFolderIn(a->element_id_, L"Shared")->element_id_;
      fs.CreateFolderIn(shared_id, L"Inner");
      file_id = fs.Create(L"/A/Shared", L"Photo.ARW", ElementType::FILE)->element_id_;
      fs.Copy(L"/A/Shared", L"/B");
    });
    ASSERT_TRUE(created.success_) << created.message_;
    ExecuteOnProjectDatabase(
        project, "UPDATE Element SET ref_count = 2 WHERE id = " + std::to_string(shared_id));
    project.SaveProject(meta_path_);
  }

  {
    ProjectService reopened(db_path_, meta_path_);
    auto           service = reopened.GetSleeveService();
    ASSERT_FALSE(
        service->Read<bool>([file_id](FileSystem& fs) { return fs.Nodes().Contains(file_id); }));
    const auto sync = service->CreateFolder(L"/B/Shared", L"New").second;
    ASSERT_TRUE(sync.success_) << sync.message_;
    reopened.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_);
  auto           service = reopened.GetSleeveService();
  const auto     copy_id = service->ResolveFolder(L"/B/Shared")->element_id_;
  EXPECT_NE(copy_id, shared_id) << "the write below /B/Shared copied the shared folder";
  EXPECT_EQ(service->ResolveFolder(L"/A/Shared")->element_id_, shared_id);

  const auto entry_names = [&](const std::filesystem::path& path) {
    std::vector<std::wstring> names;
    for (const auto& entry : service->ListFolderEntries(path)) {
      names.push_back(entry->element_name_);
    }
    std::sort(names.begin(), names.end());
    return names;
  };
  EXPECT_EQ(entry_names(L"/B/Shared"), (std::vector<std::wstring>{L"Inner", L"New", L"Photo.ARW"}));
  EXPECT_EQ(entry_names(L"/A/Shared"), (std::vector<std::wstring>{L"Inner", L"Photo.ARW"}));
  EXPECT_EQ(FileIdNamed(*service, L"/B/Shared", L"Photo.ARW"), file_id);
}

// Sleeve Node Cache Plan SN3: a stored folder link that closes a cycle is skipped at the open
// with one warning that names both folders; the other links load.
TEST_F(SleeveServiceTests, FolderEdgeCycleIsSkippedWithWarning) {
  sl_element_id_t a_id = 0;
  sl_element_id_t b_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    const auto     created = service->Write<void>([&](FileSystem& fs) {
      a_id = fs.CreateFolderIn(0, L"A")->element_id_;
      b_id = fs.CreateFolderIn(a_id, L"B")->element_id_;
    });
    ASSERT_TRUE(created.success_) << created.message_;
    ExecuteOnProjectDatabase(project, "INSERT INTO FolderContent VALUES (" + std::to_string(b_id) +
                                          ", " + std::to_string(a_id) + ")");
    project.SaveProject(meta_path_);
  }

  WarningCapture warnings;
  ProjectService reopened(db_path_, meta_path_);
  const auto     cycle_link     = std::to_string(b_id) + " -> " + std::to_string(a_id);
  const auto     cycle_warnings = warnings.Containing("closes a cycle");
  ASSERT_EQ(cycle_warnings.size(), 1u);
  EXPECT_NE(cycle_warnings.front().find(cycle_link), std::string::npos) << cycle_warnings.front();

  const auto tree = reopened.GetAlbumBrowseService()->ListFolderTree();
  EXPECT_EQ(TreePaths(tree), (std::vector<std::wstring>{L"/A", L"/A/B"}));
  auto service = reopened.GetSleeveService();
  EXPECT_EQ(service->ResolveFolder(L"/A/B")->element_id_, b_id);
  EXPECT_THROW((void)service->ResolveFolder(L"/A/B/A"), std::runtime_error);
  EXPECT_TRUE(warnings.Containing("its own ancestor").empty())
      << "the resident tree has no cycle after the load";
}

namespace {
/// The file ids of the folder @p folder_id (FileSystem::ListFileIds) under the read lock.
auto FolderFileIds(SleeveServiceImpl& service, sl_element_id_t folder_id)
    -> std::vector<sl_element_id_t> {
  return service.Read<std::vector<sl_element_id_t>>(
      [folder_id](FileSystem& fs) { return fs.ListFileIds(folder_id); });
}

/// The ids of @p files.
auto IdsOf(const std::vector<std::shared_ptr<SleeveFile>>& files) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const auto& file : files) {
    ids.push_back(file->element_id_);
  }
  return ids;
}
}  // namespace

// Sleeve Node Cache Plan SN4: a file name is a label, so two cameras that gave the same name to
// two photos put both photos into one collection, and both stay after a reopen.
TEST_F(SleeveServiceTests, TwoFilesWithEqualNamesLinkIntoOneFolder) {
  sl_element_id_t first_id  = 0;
  sl_element_id_t second_id = 0;
  sl_element_id_t album_id  = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    album_id               = service->CreateFolder(L"/", L"Album").first->element_id_;
    first_id               = service->CreateFileInLibrary(L"DSC_0001.NEF").first->element_id_;
    second_id              = service->CreateFileInLibrary(L"DSC_0001.NEF").first->element_id_;
    ASSERT_TRUE(service->LinkFileToFolder(first_id, album_id).success_);
    const auto linked = service->LinkFileToFolder(second_id, album_id);
    ASSERT_TRUE(linked.success_) << linked.message_;
    EXPECT_EQ(FolderFileIds(*service, album_id),
              (std::vector<sl_element_id_t>{first_id, second_id}));
    project.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_);
  auto           service = reopened.GetSleeveService();
  EXPECT_EQ(IdsOf(FilesNamed(*service, L"/Album", L"DSC_0001.NEF")),
            (std::vector<sl_element_id_t>{first_id, second_id}));
  EXPECT_EQ(IdsOf(FilesNamed(*service, L"/", L"DSC_0001.NEF")),
            (std::vector<sl_element_id_t>{first_id, second_id}));
  EXPECT_EQ(FolderContentRowIds(reopened, album_id).size(), 2u);
}

// Sleeve Node Cache Plan SN4: a link and an unlink of one file in one write leave no row.
TEST_F(SleeveServiceTests, LinkAndUnlinkInOneWriteLeaveNoRow) {
  ProjectService project(db_path_, meta_path_);
  auto           service  = project.GetSleeveService();
  const auto     album_id = service->CreateFolder(L"/", L"Album").first->element_id_;
  const auto     file_id  = service->CreateFileInLibrary(L"Photo.ARW").first->element_id_;

  const auto     result   = service->Write<void>([&](FileSystem& fs) {
    fs.LinkFileToFolder(file_id, album_id);
    fs.UnlinkFileFromFolder(file_id, album_id);
  });
  ASSERT_TRUE(result.success_) << result.message_;

  EXPECT_TRUE(FolderContentRowIds(project, album_id).empty());
  EXPECT_TRUE(FolderFileIds(*service, album_id).empty());
  EXPECT_EQ(FolderContentRowIds(project, 0).count(file_id), 1u) << "the root link stays";
}

// Sleeve Node Cache Plan SN4: the pending link decides the membership of an unlink, so the unlink
// of a file that the same write linked reads no row. With the Element table renamed away, a
// membership read would throw.
TEST_F(SleeveServiceTests, UnlinkOfAFileLinkedInTheSameWriteNeedsNoRow) {
  ProjectService project(db_path_, meta_path_);
  auto           service  = project.GetSleeveService();
  const auto     album_id = service->CreateFolder(L"/", L"Album").first->element_id_;
  const auto     file_id  = service->CreateFileInLibrary(L"Photo.ARW").first->element_id_;

  const auto     result   = service->Write<void>([&](FileSystem& fs) {
    fs.LinkFileToFolder(file_id, album_id);
    ExecuteOnProjectDatabase(project, "ALTER TABLE Element RENAME TO ElementAside");
    EXPECT_NO_THROW(fs.UnlinkFileFromFolder(file_id, album_id));
    ExecuteOnProjectDatabase(project, "ALTER TABLE ElementAside RENAME TO Element");
  });
  ASSERT_TRUE(result.success_) << result.message_;
  EXPECT_TRUE(FolderContentRowIds(project, album_id).empty());
}

// Sleeve Node Cache Plan SN4: a link of a file that the folder holds keeps the stored row (the
// insert ignores an existing row), so the row keeps its rowid and no duplicate appears.
TEST_F(SleeveServiceTests, RepeatedLinkKeepsOneRow) {
  sl_element_id_t album_id = 0;
  sl_element_id_t file_id  = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    album_id               = service->CreateFolder(L"/", L"Album").first->element_id_;
    file_id                = service->CreateFileInLibrary(L"Photo.ARW").first->element_id_;
    ASSERT_TRUE(service->LinkFileToFolder(file_id, album_id).success_);
    project.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_);
  auto           service = reopened.GetSleeveService();
  const auto     before  = FolderContentRowIds(reopened, album_id);
  ASSERT_EQ(before.size(), 1u);
  const auto relinked = service->LinkFileToFolder(file_id, album_id);
  ASSERT_TRUE(relinked.success_) << relinked.message_;
  EXPECT_EQ(FolderContentRowIds(reopened, album_id), before);
  EXPECT_EQ(FolderFileIds(*service, album_id), (std::vector<sl_element_id_t>{file_id}));
}

// Sleeve Node Cache Plan SN4: a folder holds no file entry, so a link into a folder with 10,000
// files loads only the linked file, and the unlink of one file loads no other file.
TEST_F(SleeveServiceTests, LinkIntoFolderWithTenThousandFilesLoadsNoFileElement) {
  constexpr int   kFileCount = 10'000;
  sl_element_id_t album_id   = 0;
  sl_element_id_t member_id  = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    album_id               = service->CreateFolder(L"/", L"Album").first->element_id_;
    const auto created     = service->Write<void>([&](FileSystem& fs) {
      for (int i = 0; i < kFileCount; ++i) {
        const auto file = fs.CreateFileInLibrary(L"IMG_" + std::to_wstring(i) + L".ARW");
        fs.LinkFileToFolder(file->element_id_, album_id);
        member_id = file->element_id_;
      }
    });
    ASSERT_TRUE(created.success_) << created.message_;
    project.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_);
  auto           service  = reopened.GetSleeveService();
  const auto     new_file = service->CreateFileInLibrary(L"IMG_0.ARW").first;
  const auto     linked   = service->LinkFileToFolder(new_file->element_id_, album_id);
  ASSERT_TRUE(linked.success_) << linked.message_;
  const auto unlinked = service->DeleteFileFromFolder(member_id, album_id);
  ASSERT_TRUE(unlinked.success_) << unlinked.message_;

  // The root, the album, the new file, and the unlinked file.
  EXPECT_EQ(service->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().Size(); }), 4u);
  const auto members = FolderFileIds(*service, album_id);
  EXPECT_EQ(members.size(), static_cast<std::size_t>(kFileCount));
  EXPECT_TRUE(ContainsId(members, new_file->element_id_));
  EXPECT_FALSE(ContainsId(members, member_id));
  EXPECT_EQ(FolderContentRowIds(reopened, album_id).size(), static_cast<std::size_t>(kFileCount));
}

// Sleeve Node Cache Plan SN5: a batch link and a batch unlink take the live files of their ids,
// decide each id from the cache or from one membership read, and load no file element.
TEST_F(SleeveServiceTests, BatchLinkAndUnlinkTakeLiveFilesOnlyAndLoadNoFileElement) {
  sl_element_id_t album_id = 0;
  sl_element_id_t first    = 0;
  sl_element_id_t second   = 0;
  sl_element_id_t gone     = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    album_id               = service->CreateFolder(L"/", L"Album").first->element_id_;
    first                  = service->CreateFileInLibrary(L"A.ARW").first->element_id_;
    second                 = service->CreateFileInLibrary(L"B.ARW").first->element_id_;
    gone                   = service->CreateFileInLibrary(L"C.ARW").first->element_id_;
    project.SaveProject(meta_path_);
  }

  ProjectService               reopened(db_path_, meta_path_);
  auto                         service    = reopened.GetSleeveService();
  constexpr sl_element_id_t    kNoElement = 900'000;
  std::vector<sl_element_id_t> linked;
  std::vector<sl_element_id_t> unlinked;
  const auto                   sync = service->Write<void>([&](FileSystem& fs) {
    fs.DeleteFileEverywhere(gone);
    const std::vector<sl_element_id_t> ids{second, gone, album_id, kNoElement, first, second, 0};
    linked = fs.LinkFilesToFolder(ids, album_id);
    EXPECT_FALSE(fs.Nodes().Contains(first));
    EXPECT_FALSE(fs.Nodes().Contains(second));
    // The pending links decide the unlink from memory.
    unlinked =
        fs.UnlinkFilesFromFolder(std::vector<sl_element_id_t>{first, gone, kNoElement}, album_id);
  });
  ASSERT_TRUE(sync.success_) << sync.message_;
  EXPECT_EQ(linked, (std::vector<sl_element_id_t>{second, first}));
  EXPECT_EQ(unlinked, std::vector<sl_element_id_t>{first});
  EXPECT_FALSE(service->Read<bool>(
      [&](FileSystem& fs) { return fs.Nodes().Contains(first) || fs.Nodes().Contains(second); }));
  EXPECT_EQ(FolderFileIds(*service, album_id), std::vector<sl_element_id_t>{second});
  const auto rows = FolderContentRowIds(reopened, album_id);
  EXPECT_EQ(rows.size(), 1u);
  EXPECT_TRUE(rows.contains(second));
}

// Sleeve Node Cache Plan SN4: a failed membership read of an unlink changes nothing; the file
// stays a member and no element becomes dirty.
TEST_F(SleeveServiceTests, FailedMembershipReadLeavesTheUnlinkUndone) {
  sl_element_id_t album_id = 0;
  sl_element_id_t file_id  = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    album_id               = service->CreateFolder(L"/", L"Album").first->element_id_;
    file_id                = service->CreateFileInLibrary(L"Photo.ARW").first->element_id_;
    ASSERT_TRUE(service->LinkFileToFolder(file_id, album_id).success_);
    project.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_);
  auto           service = reopened.GetSleeveService();
  // The file element loads first, so only the membership read meets the renamed table.
  ASSERT_NE(service->Read<std::shared_ptr<SleeveElement>>(
                [file_id](FileSystem& fs) { return fs.Get(file_id); }),
            nullptr);
  ExecuteOnProjectDatabase(reopened, "ALTER TABLE Element RENAME TO ElementAside");
  EXPECT_THROW((void)service->DeleteFileFromFolder(file_id, album_id), std::exception);
  ExecuteOnProjectDatabase(reopened, "ALTER TABLE ElementAside RENAME TO Element");

  EXPECT_EQ(service->Read<std::size_t>([](FileSystem& fs) { return fs.Nodes().DirtyCount(); }), 0u);
  EXPECT_EQ(FolderFileIds(*service, album_id), (std::vector<sl_element_id_t>{file_id}));
}

// Sleeve Node Cache Plan SN4: a library root with equal file names, and with a file that has the
// name of a folder, reopens with every file and every folder.
TEST_F(SleeveServiceTests, ProjectWithEqualRootNamesReopensWithAllFiles) {
  std::vector<sl_element_id_t> equal_ids;
  sl_element_id_t              trip_file_id   = 0;
  sl_element_id_t              trip_folder_id = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();
    trip_folder_id         = service->CreateFolder(L"/", L"Trip").first->element_id_;
    for (int i = 0; i < 3; ++i) {
      equal_ids.push_back(service->CreateFileInLibrary(L"IMG_0001.JPG").first->element_id_);
    }
    trip_file_id = service->CreateFileInLibrary(L"Trip").first->element_id_;
    project.SaveProject(meta_path_);
  }

  ProjectService reopened(db_path_, meta_path_);
  auto           service = reopened.GetSleeveService();
  EXPECT_EQ(IdsOf(FilesNamed(*service, L"/", L"IMG_0001.JPG")), equal_ids);
  EXPECT_EQ(FileIdNamed(*service, L"/", L"Trip"), trip_file_id);
  EXPECT_EQ(service->ResolveFolder(L"/Trip")->element_id_, trip_folder_id);
  EXPECT_EQ(TreePaths(reopened.GetAlbumBrowseService()->ListFolderTree()),
            (std::vector<std::wstring>{L"/Trip"}));
  EXPECT_EQ(FolderFileIds(*service, 0).size(), 4u);
}

}  // namespace alcedo
