//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The DNG color profile is runtime-only data (library_search_and_project_size_plan.md, Phase S2).
// Project tables store the profile fingerprint only; PipelineMgmtService binds the profile tables
// from the source file before a loaded document goes live.
//
// The cases import a copy of a CI DNG fixture into a scratch folder, so a case can delete or
// replace the source file. They skip when the fixture is missing.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/develop_color_transform.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "image/dng_color_profile.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/import/import_log.hpp"

namespace alcedo {
namespace {

auto DngFixturePath() -> std::filesystem::path {
  return std::filesystem::path(std::string(TEST_IMG_PATH)) / "ci_rawfiles" /
         "tag @ryanbreitkreutz - free raws from @signatureeditsco - DSC06683.dng";
}

/// A DNG from another camera: its profile has another fingerprint.
auto OtherCameraDngPath() -> std::filesystem::path {
  return std::filesystem::path(std::string(TEST_IMG_PATH)) / "raw/linear_dng/mfzoty.dng";
}

/// Text of every value in the first column of @p sql.
auto QueryTexts(ProjectService& project, const std::string& sql) -> std::vector<std::string> {
  auto                     guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto                     lock  = guard.Lock();
  duckdb_result            result;
  std::vector<std::string> texts;
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    ADD_FAILURE() << "Query failed: " << sql << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return texts;
  }
  for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
    char* value = duckdb_value_varchar(&result, 0, row);
    texts.emplace_back(value != nullptr ? value : "");
    duckdb_free(value);
  }
  duckdb_destroy_result(&result);
  return texts;
}

/// Upper bound for one stored JSON value of a DNG image. Without profile tables a value holds the
/// small RAW context and the Develop document; the 421 KB DNG profile of demo.alcd gave 71-137 KB.
constexpr std::size_t kMaxStoredJsonBytes = 24 * 1024;

void ExpectNoProfileTables(const std::vector<std::string>& texts, const std::string& fingerprint,
                           const std::string& column) {
  ASSERT_FALSE(texts.empty()) << column << " has no rows";
  for (const auto& text : texts) {
    ::testing::Test::RecordProperty(column + " bytes", static_cast<int>(text.size()));
    EXPECT_LE(text.size(), kMaxStoredJsonBytes) << column;
    EXPECT_EQ(text.find("hue_sat_map"), std::string::npos) << column;
    EXPECT_EQ(text.find("look_table"), std::string::npos) << column;
    EXPECT_EQ(text.find("HueSatMap"), std::string::npos) << column;
    EXPECT_EQ(text.find("LookTable"), std::string::npos) << column;
    EXPECT_NE(text.find(fingerprint), std::string::npos) << column << " lacks the fingerprint";
  }
}

class PipelineDngProfileBindingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    if (!std::filesystem::exists(DngFixturePath())) {
      GTEST_SKIP() << "CI DNG fixture missing: " << DngFixturePath().string();
    }
    const auto* test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    const auto  temp_dir  = std::filesystem::temp_directory_path();
    db_path_              = temp_dir / (std::string("dng_profile_binding_") + test_name + ".db");
    meta_path_            = temp_dir / (std::string("dng_profile_binding_") + test_name + ".json");
    scratch_dir_          = temp_dir / (std::string("dng_profile_binding_") + test_name);
    RemoveTestFiles();
    std::filesystem::create_directories(scratch_dir_);
    source_ = scratch_dir_ / "DSC06683.dng";
    std::filesystem::copy_file(DngFixturePath(), source_);

    project_                = std::make_unique<ProjectService>(db_path_, meta_path_);
    auto              pipes = std::make_shared<PipelineMgmtService>(project_->GetStorage());
    ImportServiceImpl importer(project_->GetSleeveService(), project_->GetImagePoolService(),
                               pipes);
    auto              job = std::make_shared<ImportJob>();
    std::promise<ImportResult> finished;
    auto                       finished_future = finished.get_future();
    job->on_finished_ = [&finished](const ImportResult& result) { finished.set_value(result); };
    job               = importer.ImportToFolder({source_}, L"", {}, job);
    ASSERT_EQ(finished_future.get().imported_, 1u);
    const auto snapshot = job->import_log_->Snapshot();
    ASSERT_EQ(snapshot.created_.size(), 1u);
    importer.SyncImports(snapshot, L"");
    element_id_      = snapshot.created_.front().element_id_;

    const auto image = project_->GetImagePoolService()->Read<std::shared_ptr<Image>>(
        snapshot.created_.front().image_id_,
        [](const std::shared_ptr<Image>& value) { return value; });
    ASSERT_NE(image, nullptr);
    imported_profile_ = image->GetRawColorContext().dng_profile_.Profile();
    ASSERT_NE(imported_profile_, nullptr);
    // The fixture must carry profile tables, or "no tables in the project" proves nothing.
    ASSERT_FALSE(imported_profile_->hue_sat_map_1.entries.empty() &&
                 imported_profile_->look_table.entries.empty());
  }

  void TearDown() override {
    project_.reset();
    RemoveTestFiles();
  }

  void RemoveTestFiles() {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(db_path_.string() + ".wal", ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(scratch_dir_, ec);
  }

  [[nodiscard]] auto Fingerprint() const -> std::string {
    return DngColorProfileFingerprintToText(imported_profile_->fingerprint);
  }

  static void ExpectBoundTo(const PipelineDocument& document, const DngColorProfilePtr& profile) {
    const auto payload = document.Develop()->Params().Params();
    ASSERT_TRUE(payload.camera_profile.dng_profile.IsBound());
    EXPECT_EQ(payload.camera_profile.dng_profile->fingerprint, profile->fingerprint);
    EXPECT_TRUE(ResolveDevelopColorTransform(payload).ok);
  }

  std::filesystem::path           db_path_;
  std::filesystem::path           meta_path_;
  std::filesystem::path           scratch_dir_;
  std::filesystem::path           source_;
  std::unique_ptr<ProjectService> project_;
  sl_element_id_t                 element_id_ = 0;
  DngColorProfilePtr              imported_profile_;
};

TEST_F(PipelineDngProfileBindingTest, ImportedDngStoresProfileFingerprintAndNoProfileTables) {
  ExpectNoProfileTables(QueryTexts(*project_, "SELECT CAST(metadata AS VARCHAR) FROM Image"),
                        Fingerprint(), "Image.metadata");
  ExpectNoProfileTables(
      QueryTexts(*project_, "SELECT CAST(param_json AS VARCHAR) FROM PipelineParam"), Fingerprint(),
      "PipelineParam.param_json");
  ExpectNoProfileTables(
      QueryTexts(*project_, "SELECT CAST(serialized_pipeline_state AS VARCHAR) FROM PipelineRoot"),
      Fingerprint(), "PipelineRoot.serialized_pipeline_state");
}

TEST_F(PipelineDngProfileBindingTest, LoadPipelineBindsSourceProfileBeforeDocumentGoesLive) {
  PipelineMgmtService pipelines(project_->GetStorage());
  auto                guard = pipelines.LoadPipeline(element_id_);
  ASSERT_NE(guard, nullptr);
  ExpectBoundTo(*guard->document_, imported_profile_);
  EXPECT_EQ(guard->pipeline_->GpuDagDocument(), guard->document_);
  pipelines.ReleasePipelineUse(guard);
}

TEST_F(PipelineDngProfileBindingTest, EditorLoadCheckpointAndRebuildBindSourceProfile) {
  {
    PipelineMgmtService pipelines(project_->GetStorage());
    auto                editor = pipelines.LoadEditorPipeline(element_id_);
    ASSERT_NE(editor, nullptr);
    ExpectBoundTo(*editor->document_, imported_profile_);
    ASSERT_NE(editor->root_document_, nullptr);
    ExpectBoundTo(*editor->root_document_, imported_profile_);

    std::string error;
    ASSERT_TRUE(pipelines.RebuildActiveEditorPipeline(editor, &error)) << error;
    ExpectBoundTo(*editor->document_, imported_profile_);
    // Last pin with write-back pending: SavePipeline stores the checkpoint.
    pipelines.SavePipeline(editor);
  }
  ExpectNoProfileTables(QueryTexts(*project_,
                                   "SELECT CAST(serialized_pipeline_state AS VARCHAR) FROM "
                                   "ImageEditState WHERE serialized_pipeline_state IS NOT NULL"),
                        Fingerprint(), "ImageEditState.serialized_pipeline_state");

  PipelineMgmtService reopened(project_->GetStorage());
  reopened.ResetEditorPipelineHistoryRebuildCountForTesting();
  auto editor = reopened.LoadEditorPipeline(element_id_);
  ASSERT_NE(editor, nullptr);
  EXPECT_EQ(reopened.EditorPipelineHistoryRebuildCount(), 0u)
      << "the checkpoint path must load and bind the stored document";
  ExpectBoundTo(*editor->document_, imported_profile_);
  reopened.SavePipeline(editor);
}

TEST_F(PipelineDngProfileBindingTest, MissingSourceFileFailsPipelineLoad) {
  std::filesystem::remove(source_);
  PipelineMgmtService pipelines(project_->GetStorage());
  try {
    (void)pipelines.LoadPipeline(element_id_);
    FAIL() << "LoadPipeline must fail when the DNG profile source file is missing";
  } catch (const std::exception& error) {
    EXPECT_NE(std::string(error.what()).find("source file is unavailable"), std::string::npos)
        << error.what();
  }
}

TEST_F(PipelineDngProfileBindingTest, SourceFileWithAnotherProfileWinsOnLoad) {
  if (!std::filesystem::exists(OtherCameraDngPath())) {
    GTEST_SKIP() << "Second DNG fixture missing: " << OtherCameraDngPath().string();
  }
  std::filesystem::copy_file(OtherCameraDngPath(), source_,
                             std::filesystem::copy_options::overwrite_existing);
  const auto replaced = MetadataExtractor::ReadDngColorProfileFromSource(source_);
  ASSERT_NE(replaced, nullptr);
  ASSERT_NE(replaced->fingerprint, imported_profile_->fingerprint);

  PipelineMgmtService pipelines(project_->GetStorage());
  auto                guard = pipelines.LoadPipeline(element_id_);
  ASSERT_NE(guard, nullptr);
  ExpectBoundTo(*guard->document_, replaced);
  pipelines.ReleasePipelineUse(guard);
}

}  // namespace
}  // namespace alcedo
