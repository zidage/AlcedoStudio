//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// StorageInvariantCheck <database> (#1191), the program that runs the storage invariant check
// (#805) on a saved project database for the file management fuzz: its exit code and the broken
// rows that it prints.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <filesystem>
#include <format>
#include <set>
#include <string>
#include <vector>

#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/storage.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "support/storage_invariants.hpp"
#include "support/storage_test_access.hpp"
#include "support/test_temp_directory.hpp"
#include "utils/clock/time_provider.hpp"

#ifndef ALCEDO_STORAGE_INVARIANT_CHECK_PATH
#error "ALCEDO_STORAGE_INVARIANT_CHECK_PATH must name the StorageInvariantCheck program"
#endif

namespace alcedo {
namespace {

using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

/// The exit code and the stdout lines of one run of the program.
struct CheckRun {
  int                      exit_code = -1;
  std::vector<std::string> lines;
};

auto RunStorageInvariantCheck(const QStringList& arguments) -> CheckRun {
  QProcess process;
  process.start(QStringLiteral(ALCEDO_STORAGE_INVARIANT_CHECK_PATH), arguments);
  if (!process.waitForStarted(30000) || !process.waitForFinished(120000)) {
    ADD_FAILURE() << "StorageInvariantCheck did not run: "
                  << process.errorString().toStdString();
    return {};
  }
  CheckRun run;
  run.exit_code = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
  for (const auto& line : process.readAllStandardOutput().split('\n')) {
    const QByteArray text = line.trimmed();
    if (!text.isEmpty()) {
      run.lines.emplace_back(text.constData(), static_cast<std::size_t>(text.size()));
    }
  }
  return run;
}

auto RunOnDatabase(const std::filesystem::path& db_path) -> CheckRun {
  return RunStorageInvariantCheck({QString::fromStdWString(db_path.wstring())});
}

/// The lines that the program prints for the violations that the in-process check finds.
auto InProcessViolationLines(const std::filesystem::path& db_path) -> std::multiset<std::string> {
  Storage                    storage(db_path);
  std::multiset<std::string> lines;
  for (const auto& violation : test::FindStorageInvariantViolations(storage)) {
    lines.insert(std::format("{}: {}", violation.rule, violation.row));
  }
  return lines;
}

class StorageInvariantCheckProgramTest : public ::testing::Test {
 protected:
  void SetUp() override { TimeProvider::Refresh(); }

  /// A project database of three imported files, each with its history root, as the production
  /// import writes them. Returns the file ids.
  auto WriteValidProject() -> std::vector<sl_element_id_t> {
    ProjectService      project(db_path_, temp_dir_.Path() / "library.json");
    PipelineMgmtService pipelines(project.GetStorage());
    SyntheticLibraryBuilder builder(project);
    const auto              ids = builder.AddFiles(
        {SyntheticImageSpec{
             .file_name_ = L"a.jpg", .image_path_ = L"D:/a.jpg", .is_raw_file_ = false},
         SyntheticImageSpec{
             .file_name_ = L"b.jpg", .image_path_ = L"D:/b.jpg", .is_raw_file_ = false},
         SyntheticImageSpec{
             .file_name_ = L"c.jpg", .image_path_ = L"D:/c.jpg", .is_raw_file_ = false}});
    for (const auto id : ids) {
      pipelines.InitializeImageRoot(id, CreateDefaultPipelineDocument(), nullptr);
    }
    test::ExpectStorageInvariants(*project.GetStorage());
    return ids;
  }

  /// Runs each statement on the database, which no project holds open.
  void Execute(const std::vector<std::string>& statements) {
    Storage storage(db_path_);
    auto    guard = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
    auto    lock  = guard.Lock();
    for (const auto& statement : statements) {
      SCOPED_TRACE(statement);
      duckorm::execute(guard.conn_, duckorm::expr::raw(statement));
    }
  }

  test::TestTempDirectory temp_dir_;
  std::filesystem::path   db_path_ = temp_dir_.Path() / "library.db";
};

TEST_F(StorageInvariantCheckProgramTest, DatabaseWithNoBrokenRowExitsWithZeroAndPrintsNothing) {
  ASSERT_EQ(WriteValidProject().size(), 3u);
  const auto run = RunOnDatabase(db_path_);
  EXPECT_EQ(run.exit_code, 0);
  EXPECT_TRUE(run.lines.empty()) << run.lines.front();
}

// One broken row of each kind that raw SQL can write. The program exits with 1, prints a line for
// each rule, and prints the same rows as the in-process check of the same database.
TEST_F(StorageInvariantCheckProgramTest, EachBrokenRowIsPrintedAndTheProgramExitsWithOne) {
  const auto ids = WriteValidProject();
  ASSERT_EQ(ids.size(), 3u);
  const auto a = ids[0];
  const auto b = ids[1];
  const auto c = ids[2];
  Execute({
      // Library.
      std::format("INSERT INTO FileImage SELECT 0, image_id FROM FileImage WHERE file_id = {}", a),
      std::format("INSERT INTO FileImage SELECT file_id, image_id FROM FileImage "
                  "WHERE file_id = {} AND image_id IN (SELECT id FROM Image)",
                  a),
      std::format("INSERT INTO FileImage VALUES ({}, 900001)", b),
      "INSERT INTO Image (id, image_path, file_name, type) "
      "VALUES (900002, 'D:/orphan.jpg', 'orphan.jpg', 0)",
      std::format("DELETE FROM FolderContent WHERE folder_id = 0 AND element_id = {}", c),
      std::format("INSERT INTO FolderContent VALUES ({}, {})", a, b),
      "INSERT INTO FolderContent VALUES (0, 900003)",
      // Mounted folders.
      std::format("UPDATE Element SET mount_id = 900004 WHERE id = {}", a),
      "INSERT INTO MountRoot (id, root_folder_id, root_path) VALUES (900005, 900006, 'D:/Mount')",
      "INSERT INTO SourceFile VALUES (900007, 1, 1, 'fingerprint', 1, 0, NULL)",
      // Edit history.
      std::format("DELETE FROM ImageEditState WHERE element_id = {}", b),
      "INSERT INTO ImageEditState VALUES (900008, 'root-900008', 'version-900008', NULL, '', NULL, "
      "1)",
      "INSERT INTO PipelineRoot VALUES ('root-900009', 900009, '{}')",
      "INSERT INTO VersionRef VALUES ('version-900010', 900010, 'Version', NULL, 0, 0)",
      std::format("UPDATE VersionRef SET head_commit_hash = 'commit-missing-head' "
                  "WHERE element_id = {}",
                  c),
      std::format("INSERT INTO EditCommit SELECT 'commit-900011', root_id, "
                  "'commit-missing-parent', NULL, 0, 0, '{{}}' FROM ImageEditState "
                  "WHERE element_id = {}",
                  c),
      "INSERT INTO EditCommit VALUES ('commit-900012', 'root-none', NULL, NULL, 0, 0, '{}')",
      "INSERT INTO EditCommitAttribution VALUES ('commit-900013', 'agent', NULL)",
      // File side tables.
      "INSERT INTO PipelineParam VALUES (900014, '{}')",
      "INSERT INTO SemanticImageEmbedding (file_id, image_id, model_key, embedding, "
      "embedding_dim, thumbnail_resolution, status) SELECT 900015, 1, 'model', "
      "array_agg(0.0::FLOAT)::FLOAT[512], 512, 224, 'ready' FROM range(512)",
      "INSERT INTO SemanticImageEmbedding768 (file_id, image_id, model_key, embedding, "
      "embedding_dim, thumbnail_resolution, status) SELECT 900016, 1, 'model', "
      "array_agg(0.0::FLOAT)::FLOAT[768], 768, 224, 'ready' FROM range(768)",
      "INSERT INTO SemanticImageLabel (file_id, model_key, label, score, confident) "
      "VALUES (900017, 'model', 'sky', 0.5, TRUE)",
      "INSERT INTO AiImageUnderstanding (file_id) VALUES (900018)",
      "INSERT INTO AiImageRating (file_id) VALUES (900019)",
      "INSERT INTO AiImageFtsDocument (file_id) VALUES (900020)",
      "INSERT INTO AiImageSearchText (file_id) VALUES (900021)",
  });
  ASSERT_FALSE(HasFatalFailure());

  const auto run = RunOnDatabase(db_path_);
  EXPECT_EQ(run.exit_code, 1);

  const std::vector<std::string> expected_rules = {
      "file has exactly one image binding",
      "image binding names an existing Image row",
      "image binding names a file element",
      "Image row is bound to a file",
      "file is listed in the library root folder",
      "folder content row names a folder element",
      "folder content row names an existing element",
      "mounted element names a stored mount",
      "mount root folder is a folder of its mount",
      "mounted file has a source record",
      "source record names a file element",
      "file has a history root",
      "history belongs to a file element",
      "history has the PipelineRoot row of its root",
      "PipelineRoot row belongs to a history",
      "active Version ref exists",
      "Version ref belongs to a history",
      "Version head commit exists in the root of its image",
      "parent commit exists in the same root",
      "commit belongs to a stored history",
      "attribution names an existing commit",
      "history loads as a commit graph",
      "PipelineParam row names a file element",
      "SemanticImageEmbedding row names a file element",
      "SemanticImageEmbedding768 row names a file element",
      "SemanticImageLabel row names a file element",
      "AiImageUnderstanding row names a file element",
      "AiImageRating row names a file element",
      "AiImageFtsDocument row names a file element",
      "AiImageSearchText row names a file element",
  };
  for (const auto& rule : expected_rules) {
    const auto prefix = rule + ": ";
    EXPECT_TRUE(std::ranges::any_of(run.lines,
                                    [&prefix](const std::string& line) {
                                      return line.starts_with(prefix);
                                    }))
        << "no line for the rule '" << rule << "'";
  }
  EXPECT_TRUE(std::ranges::find(run.lines, std::string("folder content row names an existing "
                                                       "element: folder 0 element 900003")) !=
              run.lines.end())
      << "the line names the key columns of the row";

  EXPECT_EQ(std::multiset<std::string>(run.lines.begin(), run.lines.end()),
            InProcessViolationLines(db_path_));
}

TEST_F(StorageInvariantCheckProgramTest, MissingDatabaseOrWrongArgumentsExitWithTwo) {
  EXPECT_EQ(RunOnDatabase(temp_dir_.Path() / "missing.db").exit_code, 2);
  EXPECT_EQ(RunStorageInvariantCheck({}).exit_code, 2);
  EXPECT_EQ(RunStorageInvariantCheck({QStringLiteral("a.db"), QStringLiteral("b.db")}).exit_code,
            2);
}

}  // namespace
}  // namespace alcedo
