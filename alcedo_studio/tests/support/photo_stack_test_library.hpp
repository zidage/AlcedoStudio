//  Copyright 2026 Alcedo Studio contributors
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "app/library_search_test_support.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"
#include "support/storage_test_access.hpp"
#include "support/test_temp_directory.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::test {

/// Minimal serialized assertion output, owned only by a test and never used to update storage.
struct PhotoStackTestTextRow {
  std::string text;
  using Columns = duckorm::RowColumns<&PhotoStackTestTextRow::text>;
};

/// Real project and synthetic photo inputs shared by File lifecycle and package tests.
class PhotoStackProjectTest : public ::testing::Test {
 protected:
  TestTempDirectory               temp;
  std::filesystem::path           db_path   = temp / "project.db";
  std::filesystem::path           meta_path = temp / "project.json";
  std::shared_ptr<ProjectService> project;
  std::vector<sl_element_id_t>    files;

  void                            SetUp() override {
    TimeProvider::Refresh();
    project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
    std::vector<library_search_test::SyntheticImageSpec> specs;
    for (int i = 0; i < 6; ++i) {
      specs.push_back({.file_name_   = std::format(L"stack{}.jpg", i),
                       .image_path_  = std::format(L"D:/photos/stack{}.jpg", i),
                       .rating_      = i % 6,
                       .is_raw_file_ = false});
    }
    files = library_search_test::SyntheticLibraryBuilder(*project).AddFiles(specs);
    ASSERT_EQ(files.size(), 6);
    PipelineMgmtService pipelines(project->GetStorage());
    for (const auto id : files) {
      pipelines.InitializeImageRoot(id, CreateDefaultPipelineDocument(), nullptr);
    }
  }

  void TearDown() override { project.reset(); }

  auto Files(std::initializer_list<std::size_t> indices) const -> std::vector<sl_element_id_t> {
    std::vector<sl_element_id_t> result;
    for (const auto index : indices) {
      result.push_back(files.at(index));
    }
    return result;
  }

  auto Stacks() -> PhotoStackService& { return *project->GetPhotoStackService(); }
  auto Create(std::initializer_list<std::size_t> indices, std::size_t cover) -> std::string {
    const auto result = Stacks().CreateStack(Files(indices), files.at(cover));
    EXPECT_TRUE(result.Changed());
    return result.stack_id;
  }

  void Execute(const std::string& sql) {
    auto guard = StorageTestAccess::GetDatabase(*project->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    duckorm::Statement statement(guard.conn_, sql);
    statement.Execute();
  }

  auto Number(const duckorm::SqlFragment& query) -> std::int64_t {
    auto guard = StorageTestAccess::GetDatabase(*project->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    return duckorm::select_int64(guard.conn_, query).value();
  }

  auto Cover(const std::string& stack) -> sl_element_id_t {
    return static_cast<sl_element_id_t>(
        Number({"SELECT cover_file_id FROM PhotoStack WHERE stack_id = ?", {stack}}));
  }

  auto TextRows(const std::string& sql) -> std::vector<std::string> {
    auto guard = StorageTestAccess::GetDatabase(*project->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    std::vector<std::string> result;
    for (auto& row : duckorm::query<PhotoStackTestTextRow>(guard.conn_, duckorm::expr::raw(sql))) {
      result.push_back(std::move(row.text));
    }
    return result;
  }

  /// Exact persisted assertion data for the rows changed by element sync and stack commands.
  auto OrganizationRows() -> std::vector<std::string> {
    return TextRows(
        "SELECT 'element/' || to_json(t)::VARCHAR AS text FROM Element t UNION ALL "
        "SELECT 'folder/' || to_json(t)::VARCHAR FROM FolderContent t UNION ALL "
        "SELECT 'binding/' || to_json(t)::VARCHAR FROM FileImage t UNION ALL "
        "SELECT 'stack/' || to_json(t)::VARCHAR FROM PhotoStack t UNION ALL "
        "SELECT 'member/' || to_json(t)::VARCHAR FROM PhotoStackMember t ORDER BY text");
  }

  auto StackRows() -> std::vector<std::string> {
    return TextRows(
        "SELECT stack_id || '/cover/' || cover_file_id AS text FROM PhotoStack UNION ALL "
        "SELECT stack_id || '/member/' || file_id || '/' || member_order "
        "FROM PhotoStackMember ORDER BY text");
  }

  auto Album() -> sl_element_id_t {
    auto result = project->GetSleeveService()->CreateFolder(L"/", L"Album");
    EXPECT_TRUE(result.second.success_);
    for (const auto id : files) {
      EXPECT_TRUE(
          project->GetSleeveService()->LinkFileToFolder(id, result.first->element_id_).success_);
    }
    return result.first->element_id_;
  }

  void Reopen() {
    project->SaveProject(meta_path);
    project.reset();
    project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kLoadExisting);
  }
};

}  // namespace alcedo::test
