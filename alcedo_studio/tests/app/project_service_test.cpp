//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <QString>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <json.hpp>

#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

auto QueryInt64(const std::filesystem::path& db_path, const std::string& sql) -> int64_t {
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
  const bool    ok    = duckdb_query(conn, sql.c_str(), &result) == DuckDBSuccess;
  int64_t       value = -1;
  if (ok && duckdb_row_count(&result) > 0) {
    value = duckdb_value_int64(&result, 0, 0);
  }
  duckdb_destroy_result(&result);
  duckdb_disconnect(&conn);
  duckdb_close(&db);
  if (!ok) {
    throw std::runtime_error("Test query failed: " + sql);
  }
  return value;
}

void RewriteProjectFileVersion(const std::filesystem::path& meta_path, const std::string& version) {
  nlohmann::json metadata;
  {
    std::ifstream in(meta_path);
    ASSERT_TRUE(in.is_open());
    in >> metadata;
  }
  metadata["project_file_version"] = version;
  std::ofstream out(meta_path, std::ios::trunc);
  ASSERT_TRUE(out.is_open());
  out << metadata.dump(4);
}

}  // namespace

class ProjectServiceUUIDTests : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void SetUp() override {
    db_path_  = std::filesystem::temp_directory_path() / "project_uuid_test.db";
    meta_path_ = std::filesystem::temp_directory_path() / "project_uuid_test.json";

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

TEST_F(ProjectServiceUUIDTests, NewProjectHasUUID) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto&    uuid = project.GetProjectUUID();
  EXPECT_FALSE(uuid.empty());
  EXPECT_EQ(uuid.length(), 36U);  // Standard UUID format: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
}

TEST_F(ProjectServiceUUIDTests, UUIDSurvivesSaveLoad) {
  std::string original_uuid;

  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    original_uuid = project.GetProjectUUID();
    EXPECT_FALSE(original_uuid.empty());
    project.SaveProject(meta_path_);
  }

  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
    EXPECT_EQ(project.GetProjectUUID(), original_uuid);
  }
}

TEST_F(ProjectServiceUUIDTests, UUIDLoadOrCreateNewProject) {
  // When meta file doesn't exist, kLoadOrCreate creates new project with UUID.
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kLoadOrCreate);
  const auto&    uuid = project.GetProjectUUID();
  EXPECT_FALSE(uuid.empty());
  EXPECT_EQ(uuid.length(), 36U);
}

TEST_F(ProjectServiceUUIDTests, UUIDWrittenToMetadataJSON) {
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    project.SaveProject(meta_path_);
  }

  std::ifstream file(meta_path_);
  ASSERT_TRUE(file.is_open());
  nlohmann::json metadata;
  file >> metadata;

  ASSERT_TRUE(metadata.contains("project_uuid"));
  EXPECT_TRUE(metadata.at("project_uuid").is_string());
  EXPECT_EQ(metadata.at("project_uuid").get<std::string>().length(), 36U);
}

TEST_F(ProjectServiceUUIDTests, UUIDGeneratedForMetadataWithoutUUID) {
  // Create a project first to get a valid database file.
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    project.SaveProject(meta_path_);
  }

  // Read the metadata, remove project_uuid, and write back.
  {
    std::ifstream in(meta_path_);
    ASSERT_TRUE(in.is_open());
    nlohmann::json metadata;
    in >> metadata;
    in.close();

    ASSERT_TRUE(metadata.contains("project_uuid"));
    metadata.erase("project_uuid");

    std::ofstream out(meta_path_);
    ASSERT_TRUE(out.is_open());
    out << metadata.dump(4);
    out.close();
  }

  // Load again — a new UUID should be generated for metadata without one.
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
    const auto&    uuid = project.GetProjectUUID();
    EXPECT_FALSE(uuid.empty());
    EXPECT_EQ(uuid.length(), 36U);
  }
}

TEST_F(ProjectServiceUUIDTests, CopyPreservesUUID) {
  // A project copy (same db, different meta path) keeps the same UUID.
  std::string original_uuid;
  auto        meta_copy_path = std::filesystem::temp_directory_path() / "project_uuid_copy.json";

  // Clean up copy path
  if (std::filesystem::exists(meta_copy_path)) {
    std::filesystem::remove(meta_copy_path);
  }

  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    original_uuid = project.GetProjectUUID();
    project.SaveProject(meta_path_);
  }

  // Simulate a copy: copy the meta file to a new location
  std::filesystem::copy_file(meta_path_, meta_copy_path);

  {
    // Load from the copied meta; it should have the same UUID
    ProjectService project(db_path_, meta_copy_path, ProjectOpenMode::kLoadExisting);
    EXPECT_EQ(project.GetProjectUUID(), original_uuid);
  }

  if (std::filesystem::exists(meta_copy_path)) {
    std::filesystem::remove(meta_copy_path);
  }
}

/// A new project saves the current metadata version (0.10.0) and its database has no legacy
/// edit-history or recovery-metadata table. The Mini-Git tables are still created.
TEST_F(ProjectServiceUUIDTests, NewProjectWritesCurrentVersionAndHasNoEditHistoryTable) {
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    project.SaveProject(meta_path_);
  }

  std::ifstream  in(meta_path_);
  ASSERT_TRUE(in.is_open());
  nlohmann::json metadata;
  in >> metadata;
  in.close();
  EXPECT_EQ(metadata.at("project_file_version").get<std::string>(), "0.10.0");
  EXPECT_EQ(metadata.at("project_file_min_supported_version").get<std::string>(), "0.10.0");

  // Split literals keep the archived type names out of the source tree.
  const std::string legacy_tables = std::string("('Edit") + "History', 'EditorRecovery" +
                                    "Metadata')";
  EXPECT_EQ(QueryInt64(db_path_, "SELECT COUNT(*) FROM duckdb_tables() WHERE table_name IN " +
                                     legacy_tables + ";"),
            0);
  EXPECT_EQ(QueryInt64(db_path_,
                       "SELECT COUNT(*) FROM duckdb_tables() WHERE table_name IN "
                       "('EditCommit', 'VersionRef', 'ImageEditState', 'PipelineRoot', "
                       "'PipelineParam');"),
            5);
}

/// G10.4: a packed `.alcd` whose metadata says 0.8.0 is rejected before its database bytes
/// are written to the workspace, so no temporary project database is created.
TEST_F(ProjectServiceUUIDTests, PackedProjectVersion080FailsBeforeDatabaseOpen) {
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    project.SaveProject(meta_path_);
  }
  RewriteProjectFileVersion(meta_path_, "0.8.0");

  const auto temp      = std::filesystem::temp_directory_path();
  const auto packed    = temp / "project_version_080.alcd";
  const auto workspace = temp / "project_version_080_workspace";
  std::error_code ec;
  std::filesystem::remove(packed, ec);
  std::filesystem::remove_all(workspace, ec);
  ASSERT_TRUE(project_pack::WritePackedProject(packed, meta_path_, db_path_, nullptr));

  std::filesystem::path db_out;
  std::filesystem::path meta_out;
  QString               error;
  EXPECT_FALSE(project_pack::UnpackProjectToWorkspace(packed, workspace, QStringLiteral("Legacy"),
                                                      &db_out, &meta_out, &error));
  EXPECT_TRUE(error.contains(QStringLiteral("not supported")));
  const auto runtime_pair = project_pack::BuildRuntimeProjectPair(workspace, "Legacy");
  EXPECT_FALSE(std::filesystem::exists(runtime_pair.first));
  EXPECT_FALSE(std::filesystem::exists(runtime_pair.second));
  EXPECT_TRUE(db_out.empty());

  std::filesystem::remove(packed, ec);
  std::filesystem::remove_all(workspace, ec);
}
}  // namespace alcedo
