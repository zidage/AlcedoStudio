//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// A project written by an earlier build opens and renders unchanged
// (raster_image_input_plan.md, section 7.4).
//
// tests/resources/compat/raw_project_abdb000c8.alcd was packed by build abdb000c8 (main before
// raster input). It holds one CI RAW image with one exposure commit. The JSON beside it records
// the committed document of that build and the render hash of
// support/project_compat_render.hpp on the build that render_hash_commit names. The render
// executor sets no backend, so it uses the default Auto preference (to be removed, issue #488),
// which takes OpenCL first on Windows: the hash is an OpenCL render on the NVIDIA GeForce RTX
// 3080 Laptop GPU of a Windows CUDA build, so only a CUDA build compares it. Every build checks
// that the project opens with the same document and renders.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <QString>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>

#include "app/pipeline_service.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/edit_commit_attribution.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_asset.hpp"
#include "image/color_label.hpp"
#include "image/image.hpp"
#include "json.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "support/project_compat_render.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

auto CompatDir() -> std::filesystem::path { return ALCEDO_COMPAT_FIXTURE_DIR; }

/// The fixture stores the RAW path of the machine that wrote it. Point it at this checkout.
void RewriteImagePath(const std::filesystem::path& db_path, const std::filesystem::path& raw_path) {
  duckdb_database   database;
  duckdb_connection connection;
  ASSERT_EQ(duckdb_open(db_path.string().c_str(), &database), DuckDBSuccess);
  ASSERT_EQ(duckdb_connect(database, &connection), DuckDBSuccess);
  duckdb_prepared_statement statement;
  ASSERT_EQ(duckdb_prepare(connection, "UPDATE Image SET image_path = ?", &statement),
            DuckDBSuccess);
  const auto path_bytes = conv::ToBytes(raw_path.wstring());
  ASSERT_EQ(duckdb_bind_varchar(statement, 1, path_bytes.c_str()), DuckDBSuccess);
  duckdb_result result;
  ASSERT_EQ(duckdb_execute_prepared(statement, &result), DuckDBSuccess);
  EXPECT_EQ(duckdb_rows_changed(&result), 1u);
  duckdb_destroy_result(&result);
  duckdb_destroy_prepare(&statement);
  duckdb_disconnect(&connection);
  duckdb_close(&database);
}

TEST(ProjectCompatibilityTest, ProjectWrittenByCurrentMainOpensAndRendersUnchanged) {
  TimeProvider::Refresh();
  nlohmann::json expected;
  {
    std::ifstream in(CompatDir() / "raw_project_abdb000c8.json", std::ios::binary);
    ASSERT_TRUE(in.is_open());
    expected = nlohmann::json::parse(in);
  }
  const auto raw_path = std::filesystem::path(TEST_IMG_PATH) / "ci_rawfiles" /
                        expected.at("raw_fixture").get<std::string>();
  if (!std::filesystem::exists(raw_path)) {
    GTEST_SKIP() << "CI RAW fixture is missing (Git LFS): " << raw_path.string();
  }

  const auto      workspace = std::filesystem::temp_directory_path() / "alcedo_project_compat";
  std::error_code ec;
  std::filesystem::remove_all(workspace, ec);
  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  QString               error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(CompatDir() / "raw_project_abdb000c8.alcd",
                                                     workspace, QStringLiteral("Compat"), &db_path,
                                                     &meta_path, &error))
      << error.toStdString();
  RewriteImagePath(db_path, raw_path);

  const auto element_id = expected.at("element_id").get<sl_element_id_t>();
  const auto image_id   = expected.at("image_id").get<image_id_t>();
  {
    ProjectService      project(db_path, meta_path);
    PipelineMgmtService pipelines(project.GetStorage());
    const auto          snapshot = pipelines.AcquireCommittedSnapshot(element_id);
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->Document().ToJson(), expected.at("committed_document"));
    EXPECT_FALSE(snapshot->Document().Develop()->Params().RasterInput().has_value());

    const auto hash = test::RenderCommittedElementHash(pipelines, *project.GetImagePoolService(),
                                                       element_id, image_id);
    EXPECT_NE(hash, 0u);
#ifdef HAVE_CUDA
    // The recorded hash is an OpenCL render (see the file comment); other backends and devices
    // round differently.
    EXPECT_EQ(std::to_string(hash), expected.at("render_hash").get<std::string>());
#endif
  }
  std::filesystem::remove_all(workspace, ec);
}

TEST(ProjectCompatibilityTest, OldProjectCommitsReadAsUserAndSaveAnAgentCommit) {
  TimeProvider::Refresh();
  nlohmann::json expected;
  {
    std::ifstream in(CompatDir() / "raw_project_abdb000c8.json", std::ios::binary);
    ASSERT_TRUE(in.is_open());
    expected = nlohmann::json::parse(in);
  }
  const auto workspace =
      std::filesystem::temp_directory_path() / "alcedo_project_compat_attribution";
  std::error_code ec;
  std::filesystem::remove_all(workspace, ec);
  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  QString               error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(CompatDir() / "raw_project_abdb000c8.alcd",
                                                     workspace, QStringLiteral("Compat"), &db_path,
                                                     &meta_path, &error))
      << error.toStdString();
  const auto element_id = expected.at("element_id").get<sl_element_id_t>();

  SetParameterChange change;
  change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
  change.target.node_id                = NodeId{"grade.primary"};
  change.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.contrast"};
  change.target.field_key              = "contrast";
  change.before_value                  = nlohmann::json{{"contrast", 0.0}};
  change.after_value                   = nlohmann::json{{"contrast", 12.0}};
  change.before_enabled                = true;
  change.after_enabled                 = true;
  auto batch = PipelineEditBatch::Make(PipelineEditOperationKind::SetParameter, {change},
                                       "history.operation.set_parameter");

  commit_hash_t agent_hash{};
  std::size_t   commit_count = 0;
  {
    ProjectService   project(db_path, meta_path);
    auto             guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             lock  = guard.Lock();
    CommitGraphStore store(guard.conn_);
    auto             loaded = store.LoadGraph(element_id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_GT(loaded->CommitCount(), 0u);
    EXPECT_TRUE(loaded->GetAllCommitAttributions().empty())
        << "the commits of an old project are the user's";

    auto                  graph = std::make_shared<CommitGraph>(std::move(*loaded));
    MiniGitWorkingHistory history(graph, std::make_shared<MiniGitJournal>());
    const auto            prepared = history.PrepareAppendEdit(
        std::move(batch),
        EditCommitAttribution{std::string(kEditCommitAuthorAgent), "compat edit"});
    ASSERT_TRUE(prepared.ready) << prepared.error;
    const auto appended = history.PublishPreparedEdit(prepared);
    ASSERT_TRUE(appended.committed) << appended.error;
    agent_hash   = appended.commit->GetCommitHash();
    commit_count = graph->CommitCount();
    ASSERT_NO_THROW(store.Materialize(graph->CaptureMaterialization()));
  }
  {
    ProjectService   project(db_path, meta_path);
    auto             guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto             lock  = guard.Lock();
    CommitGraphStore store(guard.conn_);
    const auto       reloaded = store.LoadGraph(element_id);
    ASSERT_TRUE(reloaded.has_value());
    EXPECT_EQ(reloaded->CommitCount(), commit_count);
    EXPECT_EQ(reloaded->GetAllCommitAttributions().size(), 1u);
    const auto* attribution = reloaded->FindCommitAttribution(agent_hash);
    ASSERT_NE(attribution, nullptr);
    EXPECT_EQ(attribution->author_, kEditCommitAuthorAgent);
    EXPECT_EQ(attribution->note_, "compat edit");
  }
  std::filesystem::remove_all(workspace, ec);
}

/// Value of a one-cell query on the database file at @p db_path, opened without the project.
auto QueryDatabaseFileInt64(const std::filesystem::path& db_path, const std::string& sql)
    -> std::int64_t {
  duckdb_database   database;
  duckdb_connection connection;
  EXPECT_EQ(duckdb_open(db_path.string().c_str(), &database), DuckDBSuccess);
  EXPECT_EQ(duckdb_connect(database, &connection), DuckDBSuccess);
  duckdb_result result;
  EXPECT_EQ(duckdb_query(connection, sql.c_str(), &result), DuckDBSuccess) << sql;
  const auto value = duckdb_value_int64(&result, 0, 0);
  duckdb_destroy_result(&result);
  duckdb_disconnect(&connection);
  duckdb_close(&database);
  return value;
}

constexpr const char* kMaskAssetTableCountSql =
    "SELECT COUNT(*) FROM information_schema.tables WHERE table_name = 'MaskAsset';";

/// 32 x 24 two-region R8 asset.
auto MakeCompatMaskAsset() -> MaskAsset {
  MaskAsset asset;
  asset.descriptor.extent           = {32, 24};
  asset.descriptor.reference_bounds = NormalizedRect{0.125f, 0.25f, 0.5f, 0.5f};
  asset.pixels.assign(32 * 24, 0);
  for (std::size_t y = 2; y < 10; ++y) {
    for (std::size_t x = 3; x < 12; ++x) {
      asset.pixels[y * 32 + x] = 255;
    }
  }
  for (std::size_t y = 14; y < 22; ++y) {
    for (std::size_t x = 18; x < 30; ++x) {
      asset.pixels[y * 32 + x] = 128;
    }
  }
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

// SM5.1 M10 and D19: a project database written before the MaskAsset table gains the table at
// open and keeps its rows; no project version change is needed.
TEST(ProjectCompatibilityTest, ExistingProjectDatabaseGetsTheMaskAssetTable) {
  TimeProvider::Refresh();
  const auto workspace =
      std::filesystem::temp_directory_path() / "alcedo_project_compat_mask_asset";
  std::error_code ec;
  std::filesystem::remove_all(workspace, ec);
  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  QString               error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(CompatDir() / "raw_project_abdb000c8.alcd",
                                                     workspace, QStringLiteral("Compat"), &db_path,
                                                     &meta_path, &error))
      << error.toStdString();
  ASSERT_EQ(QueryDatabaseFileInt64(db_path, kMaskAssetTableCountSql), 0)
      << "the fixture must predate the MaskAsset table";
  const auto commit_count = QueryDatabaseFileInt64(db_path, "SELECT COUNT(*) FROM EditCommit;");
  const auto image_count  = QueryDatabaseFileInt64(db_path, "SELECT COUNT(*) FROM Image;");
  ASSERT_GT(commit_count, 0);

  const auto asset = MakeCompatMaskAsset();
  {
    ProjectService      project(db_path, meta_path);
    PipelineMgmtService pipelines(project.GetStorage());
    pipelines.PutMaskAsset(asset);
    EXPECT_EQ(pipelines.ReadMaskAsset(asset.key).pixels, asset.pixels);
  }
  EXPECT_EQ(QueryDatabaseFileInt64(db_path, kMaskAssetTableCountSql), 1);
  EXPECT_EQ(QueryDatabaseFileInt64(db_path, "SELECT COUNT(*) FROM MaskAsset;"), 1);
  EXPECT_EQ(QueryDatabaseFileInt64(db_path, "SELECT COUNT(*) FROM EditCommit;"), commit_count);
  EXPECT_EQ(QueryDatabaseFileInt64(db_path, "SELECT COUNT(*) FROM Image;"), image_count);
  std::filesystem::remove_all(workspace, ec);
}

// SM5.1 M10: Save packs the DuckDB file, so the asset rows travel with the .alcd package.
TEST(ProjectCompatibilityTest, PackedProjectKeepsMaskAssetRows) {
  TimeProvider::Refresh();
  const auto source_workspace =
      std::filesystem::temp_directory_path() / "alcedo_project_compat_mask_pack_source";
  const auto unpacked_workspace =
      std::filesystem::temp_directory_path() / "alcedo_project_compat_mask_pack_unpacked";
  const auto packed_path =
      std::filesystem::temp_directory_path() / "alcedo_project_compat_mask_pack.alcd";
  std::error_code ec;
  std::filesystem::remove_all(source_workspace, ec);
  std::filesystem::remove_all(unpacked_workspace, ec);
  std::filesystem::remove(packed_path, ec);
  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  QString               error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(CompatDir() / "raw_project_abdb000c8.alcd",
                                                     source_workspace, QStringLiteral("Compat"),
                                                     &db_path, &meta_path, &error))
      << error.toStdString();

  const auto asset = MakeCompatMaskAsset();
  {
    auto                project = std::make_shared<ProjectService>(db_path, meta_path);
    PipelineMgmtService pipelines(project->GetStorage());
    pipelines.PutMaskAsset(asset);
    std::filesystem::path snapshot_path;
    ASSERT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshot_path, &error))
        << error.toStdString();
    ASSERT_TRUE(project_pack::CreateLiveDbSnapshot(project, snapshot_path, &error))
        << error.toStdString();
    ASSERT_TRUE(project_pack::WritePackedProject(packed_path, meta_path, snapshot_path, &error))
        << error.toStdString();
    std::filesystem::remove(snapshot_path, ec);
  }

  std::filesystem::path unpacked_db_path;
  std::filesystem::path unpacked_meta_path;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(packed_path, unpacked_workspace,
                                                     QStringLiteral("Compat"), &unpacked_db_path,
                                                     &unpacked_meta_path, &error))
      << error.toStdString();
  {
    ProjectService      project(unpacked_db_path, unpacked_meta_path);
    PipelineMgmtService pipelines(project.GetStorage());
    const auto          read = pipelines.ReadMaskAsset(asset.key);
    EXPECT_EQ(read.descriptor, asset.descriptor);
    EXPECT_EQ(read.pixels, asset.pixels);
  }
  std::filesystem::remove_all(source_workspace, ec);
  std::filesystem::remove_all(unpacked_workspace, ec);
  std::filesystem::remove(packed_path, ec);
}

/// Text of a one-cell query on the database file at @p db_path, opened without the project.
auto QueryDatabaseFileText(const std::filesystem::path& db_path, const std::string& sql)
    -> std::string {
  duckdb_database   database;
  duckdb_connection connection;
  EXPECT_EQ(duckdb_open(db_path.string().c_str(), &database), DuckDBSuccess);
  EXPECT_EQ(duckdb_connect(database, &connection), DuckDBSuccess);
  duckdb_result result;
  EXPECT_EQ(duckdb_query(connection, sql.c_str(), &result), DuckDBSuccess) << sql;
  std::string value;
  if (char* cell = duckdb_value_varchar(&result, 0, 0); cell != nullptr) {
    value = cell;
    duckdb_free(cell);
  }
  duckdb_destroy_result(&result);
  duckdb_disconnect(&connection);
  duckdb_close(&database);
  return value;
}

auto ReadProjectFileVersion(const std::filesystem::path& meta_path) -> std::string {
  std::ifstream in(meta_path, std::ios::binary);
  EXPECT_TRUE(in.is_open());
  return nlohmann::json::parse(in).at("project_file_version").get<std::string>();
}

constexpr const char* kImageMarkColumnCountSql =
    "SELECT COUNT(*) FROM duckdb_columns() WHERE table_name = 'Image' "
    "AND column_name IN ('color_labels', 'delete_mark');";
constexpr const char* kRootIdsSql =
    "SELECT string_agg(element_id || ':' || root_id, ',' ORDER BY element_id) FROM PipelineRoot;";
constexpr const char* kEditStateRootIdsSql =
    "SELECT string_agg(element_id || ':' || root_id, ',' ORDER BY element_id) "
    "FROM ImageEditState;";

// #769: a 0.10.0 project opens, gains the library mark columns with empty marks, keeps its root
// ids, stores marks, and saves as 0.11.0.
TEST(ProjectCompatibilityTest, Version010ProjectGainsLibraryMarksAndKeepsRootIds) {
  TimeProvider::Refresh();
  nlohmann::json expected;
  {
    std::ifstream in(CompatDir() / "raw_project_abdb000c8.json", std::ios::binary);
    ASSERT_TRUE(in.is_open());
    expected = nlohmann::json::parse(in);
  }
  const auto      workspace = std::filesystem::temp_directory_path() / "alcedo_project_compat_marks";
  std::error_code ec;
  std::filesystem::remove_all(workspace, ec);
  std::filesystem::path db_path;
  std::filesystem::path meta_path;
  QString               error;
  ASSERT_TRUE(project_pack::UnpackProjectToWorkspace(CompatDir() / "raw_project_abdb000c8.alcd",
                                                     workspace, QStringLiteral("Compat"), &db_path,
                                                     &meta_path, &error))
      << error.toStdString();
  ASSERT_EQ(ReadProjectFileVersion(meta_path), "0.10.0");
  ASSERT_EQ(QueryDatabaseFileInt64(db_path, kImageMarkColumnCountSql), 0)
      << "the fixture must predate the library mark columns";
  const auto root_ids       = QueryDatabaseFileText(db_path, kRootIdsSql);
  const auto edit_state_ids = QueryDatabaseFileText(db_path, kEditStateRootIdsSql);
  ASSERT_FALSE(root_ids.empty());

  const auto image_id = expected.at("image_id").get<image_id_t>();
  {
    ProjectService project(db_path, meta_path);
    auto           image_pool = project.GetImagePoolService();
    EXPECT_EQ(image_pool->Read<int>(image_id,
                                    [](const std::shared_ptr<Image>& image) {
                                      return static_cast<int>(image->color_labels_);
                                    }),
              0);
    EXPECT_FALSE(image_pool->Read<bool>(
        image_id, [](const std::shared_ptr<Image>& image) { return image->delete_mark_; }));

    image_pool->Write_NoSync<void>(image_id, [](const std::shared_ptr<Image>& image) {
      image->SetColorLabel(ColorLabel::kGreen, true);
      image->SetColorLabel(ColorLabel::kGray, true);
      image->SetDeleteMark(true);
    });
    EXPECT_TRUE(image_pool->SyncWithStorage().failed_images_.empty());
    project.SaveProject(meta_path);
  }

  EXPECT_EQ(ReadProjectFileVersion(meta_path), "0.11.0");
  EXPECT_EQ(QueryDatabaseFileInt64(db_path, kImageMarkColumnCountSql), 2);
  EXPECT_EQ(QueryDatabaseFileInt64(
                db_path, "SELECT color_labels FROM Image WHERE id = " + std::to_string(image_id)),
            ColorLabelBit(ColorLabel::kGreen) | ColorLabelBit(ColorLabel::kGray));
  EXPECT_EQ(QueryDatabaseFileInt64(db_path, "SELECT CAST(delete_mark AS BIGINT) FROM Image WHERE "
                                            "id = " + std::to_string(image_id)),
            1);
  EXPECT_EQ(QueryDatabaseFileText(db_path, kRootIdsSql), root_ids);
  EXPECT_EQ(QueryDatabaseFileText(db_path, kEditStateRootIdsSql), edit_state_ids);

  // The saved project opens again with the marks.
  {
    ProjectService project(db_path, meta_path);
    EXPECT_EQ(project.GetImagePoolService()->Read<int>(
                  image_id,
                  [](const std::shared_ptr<Image>& image) {
                    return static_cast<int>(image->color_labels_);
                  }),
              ColorLabelBit(ColorLabel::kGreen) | ColorLabelBit(ColorLabel::kGray));
    EXPECT_TRUE(project.GetImagePoolService()->Read<bool>(
        image_id, [](const std::shared_ptr<Image>& image) { return image->delete_mark_; }));
  }
  std::filesystem::remove_all(workspace, ec);
}
}  // namespace
}  // namespace alcedo
