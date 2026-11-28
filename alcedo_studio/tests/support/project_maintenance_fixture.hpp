// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <format>
#include <fstream>

#include "app/project_maintenance_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/pipeline/pipeline_mapper.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"

namespace alcedo::test {
auto MaintenanceTestDirectory() -> QString {
  const auto root = std::filesystem::path(__FILE__)
                        .lexically_normal()
                        .parent_path()
                        .parent_path()
                        .parent_path()
                        .parent_path() /
                    "build/tmp/project_maintenance/cases";
  std::filesystem::create_directories(root);
  return QString::fromStdWString((root / "case-XXXXXX").wstring());
}
class MaintenanceStorageFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.isValid());
    root_          = std::filesystem::path(directory_.path().toStdWString());
    database_path_ = root_ / "project.db";
    database_      = std::make_unique<Database>(database_path_);
    store_         = std::make_unique<ProjectMaintenanceStore>(*database_);
    source_        = root_ / "photo.raw";
    std::ofstream(source_) << "source bytes";
  }
  void Execute(const std::string& query) {
    auto guard = database_->GetConnectionGuard();
    auto lock  = guard.Lock();
    duckorm::execute(guard.conn_, duckorm::expr::raw(query));
  }
  auto Rows(const std::string& table) -> std::string {
    auto guard = database_->GetConnectionGuard();
    auto lock  = guard.Lock();
    return duckorm::select_string(
               guard.conn_, duckorm::expr::raw("SELECT to_json(list(t)) FROM (SELECT * FROM " +
                                               table + " ORDER BY ALL) t"))
        .value_or("");
  }
  void Add(sl_element_id_t id, const image_path_t& path = {}, bool record_source = true) {
    const auto& image_path = path.empty() ? source_ : path;
    auto guard = database_->GetConnectionGuard();
    auto lock  = guard.Lock();
    auto query = duckorm::expr::raw("INSERT INTO Image(id,image_path,metadata) VALUES (");
    query.append(duckorm::expr::param(static_cast<int64_t>(id)));
    query.append(duckorm::expr::raw(","));
    query.append(duckorm::expr::param(image_path.generic_string()));
    query.append(duckorm::expr::raw(",'{\"Rating\":0}')"));
    duckorm::execute(guard.conn_, query);
    duckorm::execute(guard.conn_,
                     duckorm::expr::raw(std::format(
                         "INSERT INTO Element(id,type,element_name,added_time,ref_count) "
                         "VALUES ({0},0,'photo',TIMESTAMP '2026-01-01',0);"
                         "INSERT INTO FileImage VALUES ({0},{0})",
                         id)));
    const auto document = CreateDefaultPipelineDocument();
    CommitGraphStore(guard.conn_).CreateRootPipelinePersisted(id, document);
    PipelineMapper(guard.conn_)
        .InsertParams({id, std::make_unique<std::string>(document.ToJson().dump())});
    if (record_source && std::filesystem::is_regular_file(image_path)) {
      const auto fingerprint = ComputeSourceFileFingerprint(image_path);
      ASSERT_TRUE(fingerprint.HasFingerprint());
      const SourceFileRecord source{.element_id          = id,
                                    .byte_size           = std::filesystem::file_size(image_path),
                                    .fingerprint         = fingerprint.fingerprint.hash,
                                    .fingerprint_version = fingerprint.fingerprint.version};
      SourceFileStore(*database_).UpsertSourceFiles(std::span(&source, 1));
    }
  }
  auto Find(sl_element_id_t id) -> const MaintenanceFinding& {
    for (const auto& finding : service_.Findings()) {
      if (finding.evidence.entry_id == id) return finding;
    }
    throw std::runtime_error("Finding absent");
  }
  QTemporaryDir             directory_{MaintenanceTestDirectory()};
  image_path_t              root_, source_, database_path_;
  std::unique_ptr<Database> database_;
  std::unique_ptr<ProjectMaintenanceStore> store_;
  ProjectMaintenanceService service_;
};

}  // namespace alcedo::test
