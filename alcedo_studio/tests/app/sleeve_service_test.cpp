//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/sleeve_service.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "app/history_mgmt_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "edit/operators/op_base.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {
auto ContainsId(const std::vector<sl_element_id_t>& ids, sl_element_id_t id) -> bool {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
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

auto ReadExposure(const std::shared_ptr<PipelineGuard>& pipeline_guard) -> float {
  const auto exported = pipeline_guard->pipeline_->ExportPipelineParams();
  return exported["Basic Adjustment"]["Basic Adjustment"]["exposure"]["params"]["exposure"]
      .get<float>();
}

}  // namespace

TEST(ProjectVersionTests, MiniGitSchemaBumpRejectsPreviousProjectVersion) {
  EXPECT_TRUE(project_pack::ProjectVersionIsSupported(project_pack::kProjectFileVersion));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.2.5"));
  EXPECT_FALSE(project_pack::ProjectVersionIsSupported("0.2.4"));
}

class SleeveServiceTests : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void                  SetUp() override {
    TimeProvider::Refresh();
    RegisterAllOperators();
    db_path_ = std::filesystem::temp_directory_path() / "sleeve_service_test.db";
    meta_path_ = std::filesystem::temp_directory_path() / "sleeve_service_test.json";
    if (std::filesystem::exists(db_path_)) {
      std::filesystem::remove(db_path_);
    }
    if (std::filesystem::exists(meta_path_)) {
      std::filesystem::remove(meta_path_);
    }
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

  service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"/Folder", L"File", ElementType::FILE); });

  auto file = service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/Folder/File", false); });
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->element_name_, L"File");
}

TEST_F(SleeveServiceTests, DeleteTest) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  service->Write<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Create(L"", L"File", ElementType::FILE); });
  service->Write<bool>([](FileSystem& fs) {
    fs.Delete(L"/File");
    return true;
  });

  EXPECT_THROW(service->Read<std::shared_ptr<SleeveElement>>(
                   [](FileSystem& fs) { return fs.Get(L"/File", false); }),
               std::runtime_error);
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
  // reloaded_project.LoadProject(meta_path_);
  auto           reloaded_service = reloaded_project.GetSleeveService();
  auto           file             = reloaded_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/Folder/File", false); });
  ASSERT_NE(file, nullptr);
  EXPECT_EQ(file->element_name_, L"File");
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

  const auto edited = service->Write_NoSync<std::shared_ptr<SleeveFile>>([](FileSystem& fs) {
    auto file = std::static_pointer_cast<SleeveFile>(fs.Get(L"/AlbumA/Shared.arw", true));
    file->SetLastModifiedTime();
    return file;
  });
  ASSERT_NE(edited, nullptr);
  EXPECT_EQ(edited->element_id_, file.first->element_id_);

  const auto from_root    = service->Read<std::shared_ptr<SleeveFile>>([](FileSystem& fs) {
    return std::static_pointer_cast<SleeveFile>(fs.Get(L"/Shared.arw", false));
  });
  const auto from_album_b = service->Read<std::shared_ptr<SleeveFile>>([](FileSystem& fs) {
    return std::static_pointer_cast<SleeveFile>(fs.Get(L"/AlbumB/Shared.arw", false));
  });

  EXPECT_EQ(from_root->element_id_, file.first->element_id_);
  EXPECT_EQ(from_album_b->element_id_, file.first->element_id_);
  EXPECT_EQ(from_root->last_modified_time_, edited->last_modified_time_);
  EXPECT_EQ(from_album_b->last_modified_time_, edited->last_modified_time_);
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

    modified_time           = service->Write_NoSync<std::time_t>([](FileSystem& fs) {
      auto album_file =
          std::static_pointer_cast<SleeveFile>(fs.Get(L"/AlbumA/SharedEdit.arw", true));
      album_file->SetLastModifiedTime();
      return album_file->last_modified_time_;
    });

    const auto from_root    = service->Read<std::shared_ptr<SleeveFile>>([](FileSystem& fs) {
      return std::static_pointer_cast<SleeveFile>(fs.Get(L"/SharedEdit.arw", false));
    });
    const auto from_album_b = service->Read<std::shared_ptr<SleeveFile>>([](FileSystem& fs) {
      return std::static_pointer_cast<SleeveFile>(fs.Get(L"/AlbumB/SharedEdit.arw", false));
    });

    EXPECT_EQ(from_root->element_id_, file_id);
    EXPECT_EQ(from_album_b->element_id_, file_id);
    EXPECT_EQ(from_root->last_modified_time_, modified_time);
    EXPECT_EQ(from_album_b->last_modified_time_, modified_time);
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
    EXPECT_EQ(service->ResolveFile(L"/KeepAfterAlbumDelete.arw")->element_id_, file_id);
    EXPECT_THROW(service->ResolveFolder(L"/Album"), std::runtime_error);

    project.SaveProject(meta_path_);
  }

  ProjectService reloaded_project(db_path_, meta_path_);
  auto           service  = reloaded_project.GetSleeveService();
  const auto     root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });

  EXPECT_TRUE(ContainsId(root_ids, file_id));
  EXPECT_EQ(service->ResolveFile(L"/KeepAfterAlbumDelete.arw")->element_id_, file_id);
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

  const auto written = service->Write_NoSync<std::shared_ptr<SleeveFile>>([](FileSystem& fs) {
    auto file = std::static_pointer_cast<SleeveFile>(fs.Get(L"/Subfolder/Linux.arw", true));
    file->SetLastModifiedTime();
    return file;
  });
  ASSERT_NE(written, nullptr);

  const auto from_original = service->ResolveFile(L"/Folder/Subfolder/Linux.arw");
  const auto from_copied   = service->ResolveFile(L"/Subfolder/Linux.arw");
  ASSERT_NE(from_original, nullptr);
  ASSERT_NE(from_copied, nullptr);

  EXPECT_EQ(from_original->element_id_, written->element_id_);
  EXPECT_EQ(from_copied->element_id_, written->element_id_);
  EXPECT_EQ(from_original->last_modified_time_, written->last_modified_time_);
  EXPECT_EQ(from_copied->last_modified_time_, written->last_modified_time_);
  EXPECT_EQ(from_original->ref_count_, 1u);
}

TEST_F(SleeveServiceTests, ExplicitDuplicateClonesStateAndKeepsHistoryAndPipelineIndependent) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  const auto     album   = service->CreateFolder(L"/", L"Album").first;
  const auto     source  = service->CreateFileInLibrary(L"Source.arw").first;
  ASSERT_NE(album, nullptr);
  ASSERT_NE(source, nullptr);

  const auto source_id = source->element_id_;

  {
    PipelineMgmtService pipeline_service(project.GetStorageService());
    auto                source_pipeline = pipeline_service.LoadPipeline(source_id);
    auto&               stage = source_pipeline->pipeline_->GetStage(PipelineStageName::Basic_Adjustment);
    stage.SetOperator(OperatorType::EXPOSURE, nlohmann::json{{"exposure", 2.5f}},
                      source_pipeline->pipeline_->GetGlobalParams());
    source_pipeline->dirty_ = true;
    pipeline_service.SavePipeline(source_pipeline);
    pipeline_service.Sync();
  }

  {
    EditHistoryMgmtService history_service(project.GetStorageService());
    auto                   source_history = history_service.LoadHistory(source_id);
    ASSERT_EQ(source_history->history_->GetVersions().size(), 1u);
    (void)history_service.CreateVersion(source_history, "Source Look");
    history_service.SaveHistory(source_history);
    history_service.Sync();
  }

  const auto duplicated = service->DuplicateFileToFolder(source_id, album->element_id_);
  ASSERT_TRUE(duplicated.second.success_);
  ASSERT_NE(duplicated.first, nullptr);

  const auto duplicate_id = duplicated.first->element_id_;
  EXPECT_NE(duplicate_id, source_id);
  EXPECT_EQ(service->ResolveFile(L"/Source.arw")->element_id_, source_id);
  EXPECT_EQ(service->ResolveFile(L"/Source.arw@")->element_id_, duplicate_id);
  EXPECT_EQ(service->ResolveFile(L"/Album/Source.arw@")->element_id_, duplicate_id);

  {
    PipelineMgmtService pipeline_service(project.GetStorageService());
    auto                source_pipeline    = pipeline_service.LoadPipeline(source_id);
    auto                duplicate_pipeline = pipeline_service.LoadPipeline(duplicate_id);
    ASSERT_NE(source_pipeline, nullptr);
    ASSERT_NE(duplicate_pipeline, nullptr);
    EXPECT_FLOAT_EQ(ReadExposure(source_pipeline), 2.5f);
    EXPECT_FLOAT_EQ(ReadExposure(duplicate_pipeline), 2.5f);
  }

  {
    EditHistoryMgmtService history_service(project.GetStorageService());
    auto                   source_history    = history_service.LoadHistory(source_id);
    auto                   duplicate_history = history_service.LoadHistory(duplicate_id);
    ASSERT_NE(source_history, nullptr);
    ASSERT_NE(duplicate_history, nullptr);
    EXPECT_EQ(source_history->history_->GetVersions().size(), 2u);
    EXPECT_EQ(duplicate_history->history_->GetVersions().size(), 2u);
    EXPECT_EQ(duplicate_history->history_->GetBoundImage(), duplicate_id);
  }

  {
    PipelineMgmtService pipeline_service(project.GetStorageService());
    auto                duplicate_pipeline = pipeline_service.LoadPipeline(duplicate_id);
    auto&               stage =
        duplicate_pipeline->pipeline_->GetStage(PipelineStageName::Basic_Adjustment);
    stage.SetOperator(OperatorType::EXPOSURE, nlohmann::json{{"exposure", 4.0f}},
                      duplicate_pipeline->pipeline_->GetGlobalParams());
    duplicate_pipeline->dirty_ = true;
    pipeline_service.SavePipeline(duplicate_pipeline);
    pipeline_service.Sync();
  }

  {
    EditHistoryMgmtService history_service(project.GetStorageService());
    auto                   duplicate_history = history_service.LoadHistory(duplicate_id);
    (void)history_service.CreateVersion(duplicate_history, "Duplicate Look");
    history_service.SaveHistory(duplicate_history);
    history_service.Sync();
  }

  {
    PipelineMgmtService pipeline_service(project.GetStorageService());
    auto                source_pipeline    = pipeline_service.LoadPipeline(source_id);
    auto                duplicate_pipeline = pipeline_service.LoadPipeline(duplicate_id);
    EXPECT_FLOAT_EQ(ReadExposure(source_pipeline), 2.5f);
    EXPECT_FLOAT_EQ(ReadExposure(duplicate_pipeline), 4.0f);
  }

  {
    EditHistoryMgmtService history_service(project.GetStorageService());
    auto                   source_history    = history_service.LoadHistory(source_id);
    auto                   duplicate_history = history_service.LoadHistory(duplicate_id);
    EXPECT_EQ(source_history->history_->GetVersions().size(), 2u);
    EXPECT_EQ(duplicate_history->history_->GetVersions().size(), 3u);
  }
}

TEST_F(SleeveServiceTests, DuplicateUsesLatestPipelineSnapshotBeforePipelineSync) {
  ProjectService project(db_path_, meta_path_);
  auto           service = project.GetSleeveService();

  const auto     album   = service->CreateFolder(L"/", L"Album").first;
  const auto     source  = service->CreateFileInLibrary(L"Source.arw").first;
  ASSERT_NE(album, nullptr);
  ASSERT_NE(source, nullptr);

  const auto source_id = source->element_id_;

  {
    PipelineMgmtService pipeline_service(project.GetStorageService());
    auto                source_pipeline = pipeline_service.LoadPipeline(source_id);
    auto&               stage = source_pipeline->pipeline_->GetStage(PipelineStageName::Basic_Adjustment);
    stage.SetOperator(OperatorType::EXPOSURE, nlohmann::json{{"exposure", 2.5f}},
                      source_pipeline->pipeline_->GetGlobalParams());
    source_pipeline->dirty_ = true;
    pipeline_service.SavePipeline(source_pipeline);
  }

  const auto duplicated = service->DuplicateFileToFolder(source_id, album->element_id_);
  ASSERT_TRUE(duplicated.second.success_);
  ASSERT_NE(duplicated.first, nullptr);

  {
    PipelineMgmtService pipeline_service(project.GetStorageService());
    auto                duplicate_pipeline = pipeline_service.LoadPipeline(duplicated.first->element_id_);
    ASSERT_NE(duplicate_pipeline, nullptr);
    EXPECT_FLOAT_EQ(ReadExposure(duplicate_pipeline), 2.5f);
  }
}

TEST_F(SleeveServiceTests, DuplicateUsesLatestHistoryWhenLoadedFileCacheIsStale) {
  sl_element_id_t source_id = 0;
  sl_element_id_t album_id  = 0;
  {
    ProjectService project(db_path_, meta_path_);
    auto           service = project.GetSleeveService();

    const auto     album   = service->CreateFolder(L"/", L"Album").first;
    const auto     source  = service->CreateFileInLibrary(L"Source.arw").first;
    ASSERT_NE(album, nullptr);
    ASSERT_NE(source, nullptr);

    album_id  = album->element_id_;
    source_id = source->element_id_;
    {
      EditHistoryMgmtService history_service(project.GetStorageService());
      auto                   source_history = history_service.LoadHistory(source_id);
      ASSERT_NE(source_history, nullptr);
      (void)history_service.CreateVersion(source_history, "Baseline");
      history_service.SaveHistory(source_history);
    }
    project.SaveProject(meta_path_);
  }

  ProjectService project(db_path_, meta_path_);
  auto           service          = project.GetSleeveService();
  const auto     resolved_source  = service->ResolveFile(L"/Source.arw");
  ASSERT_NE(resolved_source, nullptr);
  ASSERT_NE(resolved_source->GetEditHistory(), nullptr);
  ASSERT_EQ(resolved_source->GetEditHistory()->GetVersions().size(), 2u);

  {
    EditHistoryMgmtService history_service(project.GetStorageService());
    auto                   source_history = history_service.LoadHistory(source_id);
    ASSERT_NE(source_history, nullptr);
    ASSERT_EQ(source_history->history_->GetVersions().size(), 2u);
    (void)history_service.CreateVersion(source_history, "Source Look");
    history_service.SaveHistory(source_history);
  }

  ASSERT_EQ(resolved_source->GetEditHistory()->GetVersions().size(), 2u);

  const auto duplicated = service->DuplicateFileToFolder(source_id, album_id);
  ASSERT_TRUE(duplicated.second.success_);
  ASSERT_NE(duplicated.first, nullptr);

  {
    EditHistoryMgmtService history_service(project.GetStorageService());
    auto                   duplicate_history = history_service.LoadHistory(duplicated.first->element_id_);
    ASSERT_NE(duplicate_history, nullptr);
    EXPECT_EQ(duplicate_history->history_->GetVersions().size(), 3u);
  }
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
  EXPECT_EQ(service->ResolveFile(L"/RootOnly.arw")->element_id_, file_id);
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
  EXPECT_EQ(service->ResolveFile(L"/AlbumImport.arw")->element_id_, file_id);
  EXPECT_EQ(service->ResolveFile(L"/Album/AlbumImport.arw")->element_id_, file_id);
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
  EXPECT_EQ(service->ResolveFile(L"/UnlinkOnly.arw")->element_id_, file_id);
  EXPECT_THROW(service->ResolveFile(L"/Album/UnlinkOnly.arw"), std::runtime_error);
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
    fs.Delete(L"/Album/KeepInRoot.arw");
    return true;
  });

  const auto root_ids = service->Read<std::vector<sl_element_id_t>>(
      [](FileSystem& fs) { return fs.ListFolderContent(0); });
  const auto album_ids = service->Read<std::vector<sl_element_id_t>>(
      [album](FileSystem& fs) { return fs.ListFolderContent(album->element_id_); });

  EXPECT_TRUE(ContainsId(root_ids, file.first->element_id_));
  EXPECT_FALSE(ContainsId(album_ids, file.first->element_id_));
  EXPECT_NE(service->ResolveFile(L"/KeepInRoot.arw"), nullptr);
  EXPECT_THROW(service->ResolveFile(L"/Album/KeepInRoot.arw"), std::runtime_error);
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
      fs.Delete(L"/DeleteEverywhere.arw");
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
  EXPECT_THROW(reloaded_service->ResolveFile(L"/DeleteEverywhere.arw"), std::runtime_error);
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

}  // namespace alcedo
