//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/semantic/semantic_store.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "app/project_service.hpp"
#include "app/project_package_backend.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "storage/store/database.hpp"

namespace alcedo {
namespace {
constexpr const char* kModelKey       = "mobileclip-test";
constexpr const char* kSiglipModelKey = "siglip2-test";

auto                  OneHotForDim(size_t dim, size_t index) -> std::vector<float> {
  std::vector<float> embedding(dim, 0.0F);
  embedding.at(index) = 1.0F;
  return embedding;
}

auto OneHot(size_t index) -> std::vector<float> {
  return OneHotForDim(kSemanticEmbeddingDim, index);
}

auto OneHot768(size_t index) -> std::vector<float> {
  return OneHotForDim(kSemanticEmbeddingDim768, index);
}

auto MixedQuery(size_t primary, size_t secondary) -> std::vector<float> {
  std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
  embedding.at(primary)   = 0.95F;
  embedding.at(secondary) = 0.05F;
  return embedding;
}

auto ClosePairQuery(size_t first, size_t second) -> std::vector<float> {
  std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
  embedding.at(first)  = 0.72F;
  embedding.at(second) = 0.69F;
  return embedding;
}

// Caller-controlled weights per axis, so a fixture can craft a precise descending score
// shape (a close top group followed by a cliff) for the elbow to cut after.
auto WeightedQuery(const std::vector<std::pair<size_t, float>>& weights) -> std::vector<float> {
  std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
  for (const auto& [index, weight] : weights) {
    embedding.at(index) = weight;
  }
  return embedding;
}

auto UnitVectorWithSimilarity(size_t primary, size_t auxiliary, float similarity)
    -> std::vector<float> {
  std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
  embedding.at(primary)   = similarity;
  embedding.at(auxiliary) = std::sqrt(std::max(0.0F, 1.0F - (similarity * similarity)));
  return embedding;
}

auto CountSubstring(const std::string& text, const std::string& needle) -> size_t {
  size_t count = 0;
  size_t pos   = 0;
  while ((pos = text.find(needle, pos)) != std::string::npos) {
    ++count;
    pos += needle.size();
  }
  return count;
}

void RunRawSql(const std::filesystem::path& db_path, const char* sql) {
  duckdb_database   db   = nullptr;
  duckdb_connection conn = nullptr;
  ASSERT_EQ(duckdb_open(db_path.string().c_str(), &db), DuckDBSuccess);
  ASSERT_EQ(duckdb_connect(db, &conn), DuckDBSuccess);

  duckdb_result result;
  ASSERT_EQ(duckdb_query(conn, sql, &result), DuckDBSuccess)
      << (duckdb_result_error(&result) ? duckdb_result_error(&result) : "");
  duckdb_destroy_result(&result);
  duckdb_disconnect(&conn);
  duckdb_close(&db);
}

void RegisterTestModel(SemanticStore& semantic) {
  std::string error;
  ASSERT_TRUE(semantic.UpsertModel(SemanticModelRecord{.model_key_     = kModelKey,
                                                       .model_id_      = "mobileclip-test",
                                                       .revision_      = "test-rev",
                                                       .embedding_dim_ = kSemanticEmbeddingDim,
                                                       .image_size_    = 256},
                                   &error))
      << error;
}

void RegisterSiglipTestModel(SemanticStore& semantic) {
  std::string error;
  ASSERT_TRUE(semantic.UpsertModel(SemanticModelRecord{.model_key_     = kSiglipModelKey,
                                                       .model_id_      = "siglip2-test",
                                                       .revision_      = "test-rev",
                                                       .embedding_dim_ = kSemanticEmbeddingDim768,
                                                       .image_size_    = 256},
                                   &error))
      << error;
}
}  // namespace

class SemanticStoreTest : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void                  SetUp() override {
    RegisterAllOperators();
    const auto*       test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    const std::string suffix = std::string(test_info->test_suite_name()) + "_" + test_info->name();
    db_path_   = std::filesystem::temp_directory_path() / (suffix + ".db");
    meta_path_ = std::filesystem::temp_directory_path() / (suffix + ".json");
    std::filesystem::remove(db_path_);
    std::filesystem::remove(meta_path_);
  }

  void TearDown() override {
    std::filesystem::remove(db_path_);
    std::filesystem::remove(meta_path_);
  }

  static auto CreateSyntheticFile(ProjectService& project, const file_name_t& file_name)
      -> sl_element_id_t {
    auto image_pool          = project.GetImagePoolService();
    auto image               = image_pool->CreateAndReturnPinnedEmpty();
    auto image_id            = image.Get()->image_id_;
    image.Get()->image_name_ = file_name;
    image.Get()->image_path_ = std::filesystem::path{file_name};
    image_pool->SyncWithStorage();

    auto sleeve = project.GetSleeveService();
    auto file   = sleeve->Write<std::shared_ptr<SleeveFile>>(
        [file_name, image_id](FileSystem& fs) -> std::shared_ptr<SleeveFile> {
          auto created       = fs.CreateFileInLibrary(file_name);
          created->image_id_ = image_id;
          return created;
        });
    EXPECT_TRUE(file.second.success_);
    EXPECT_NE(file.first, nullptr);
    return file.first ? file.first->element_id_ : 0;
  }

  static void StoreEmbedding(SemanticStore& semantic, sl_element_id_t file_id,
                             image_id_t image_id, std::vector<float> embedding) {
    std::string error;
    ASSERT_TRUE(semantic.UpsertImageEmbedding(
        SemanticImageEmbeddingRecord{
            .file_id_   = file_id,
            .image_id_  = image_id,
            .model_key_ = kModelKey,
            .embedding_ = std::move(embedding),
        },
        &error))
        << error;
  }
};

TEST_F(SemanticStoreTest, VssSearchRanksWithinRootAndFolderScope) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();
  RegisterTestModel(semantic);

  const auto mountain_id = CreateSyntheticFile(project, L"mountain.raf");
  const auto beach_id    = CreateSyntheticFile(project, L"beach.raf");
  const auto portrait_id = CreateSyntheticFile(project, L"portrait.raf");

  const auto rows        = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 3U);
  for (const auto& row : rows) {
    if (row.file_id_ == mountain_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, OneHot(0));
    } else if (row.file_id_ == beach_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, OneHot(1));
    } else if (row.file_id_ == portrait_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, OneHot(2));
    }
  }

  std::string error;
  ASSERT_TRUE(semantic.EnsureVectorSearchIndex(kModelKey, &error)) << error;

  const auto root_results =
      semantic.SearchImageEmbeddings(0, kModelKey, ClosePairQuery(1, 2), 0, 3, &error);
  ASSERT_EQ(root_results.size(), 2U) << error;
  EXPECT_EQ(root_results[0].file_id_, beach_id);
  EXPECT_EQ(root_results[1].file_id_, portrait_id);

  auto sleeve = project.GetSleeveService();
  auto album  = sleeve->CreateFolder(L"/", L"SemanticScope");
  ASSERT_TRUE(album.second.success_);
  ASSERT_NE(album.first, nullptr);
  ASSERT_TRUE(sleeve->LinkFileToFolder(portrait_id, album.first->element_id_).success_);

  const auto scoped_results = semantic.SearchImageEmbeddings(album.first->element_id_, kModelKey,
                                                             ClosePairQuery(1, 2), 0, 3, &error);
  ASSERT_EQ(scoped_results.size(), 1U) << error;
  EXPECT_EQ(scoped_results[0].file_id_, portrait_id);
}

TEST_F(SemanticStoreTest, PackedSnapshotSucceedsAfterVectorSearchIndex) {
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    auto&          semantic = project.GetStorage()->GetSemanticStore();
    RegisterTestModel(semantic);

    const auto file_id = CreateSyntheticFile(project, L"indexed.raf");
    const auto rows    = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
    ASSERT_EQ(rows.size(), 1U);
    StoreEmbedding(semantic, file_id, rows.front().image_id_, OneHot(0));

    std::string error;
    ASSERT_TRUE(semantic.EnsureVectorSearchIndex(kModelKey, &error)) << error;
    project.SaveProject(meta_path_);
  }

  std::filesystem::path snapshot_path;
  ASSERT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshot_path, nullptr));
  QString snapshot_error;
  auto    reopened = std::make_shared<ProjectService>(db_path_, meta_path_,
                                                       ProjectOpenMode::kLoadExisting);
  EXPECT_TRUE(project_pack::CreateLiveDbSnapshot(
      reopened, snapshot_path, &snapshot_error))
      << snapshot_error.toStdString();

  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
}

TEST_F(SemanticStoreTest, VssSearchCutsOffWeakTailBeforePaging) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();
  RegisterTestModel(semantic);

  const auto strong_id      = CreateSyntheticFile(project, L"strong_match.raf");
  const auto near_id        = CreateSyntheticFile(project, L"near_match.raf");
  const auto weak_id        = CreateSyntheticFile(project, L"weak_term_match.raf");
  const auto weak_second_id = CreateSyntheticFile(project, L"weak_term_match_2.raf");

  const auto rows = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 4U);
  for (const auto& row : rows) {
    if (row.file_id_ == strong_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, UnitVectorWithSimilarity(0, 10, 0.94F));
    } else if (row.file_id_ == near_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, UnitVectorWithSimilarity(0, 11, 0.91F));
    } else if (row.file_id_ == weak_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, UnitVectorWithSimilarity(0, 12, 0.62F));
    } else if (row.file_id_ == weak_second_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, UnitVectorWithSimilarity(0, 13, 0.60F));
    }
  }

  std::string error;
  ASSERT_TRUE(semantic.EnsureVectorSearchIndex(kModelKey, &error)) << error;

  const auto first_page = semantic.SearchImageEmbeddings(0, kModelKey, OneHot(0), 0, 1, &error);
  ASSERT_EQ(first_page.size(), 1U) << error;
  EXPECT_EQ(first_page[0].file_id_, strong_id);

  const auto second_page = semantic.SearchImageEmbeddings(0, kModelKey, OneHot(0), 1, 1, &error);
  ASSERT_EQ(second_page.size(), 1U) << error;
  EXPECT_EQ(second_page[0].file_id_, near_id);

  const auto weak_tail = semantic.SearchImageEmbeddings(0, kModelKey, OneHot(0), 2, 10, &error);
  EXPECT_TRUE(weak_tail.empty()) << error;
}

TEST_F(SemanticStoreTest, RejectsInvalidVectorsBeforeStorageOrSearch) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();

  std::string    error;
  EXPECT_FALSE(semantic.UpsertModel(SemanticModelRecord{.model_key_     = "wrong-dim",
                                                        .model_id_      = "mobileclip-test",
                                                        .revision_      = "test-rev",
                                                        .embedding_dim_ = 3,
                                                        .image_size_    = 256},
                                    &error));
  EXPECT_FALSE(error.empty());

  RegisterTestModel(semantic);
  const auto file_id = CreateSyntheticFile(project, L"invalid_vectors.raf");
  const auto rows    = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 1U);
  const auto image_id  = rows.front().image_id_;

  auto       wrong_dim = OneHot(0);
  wrong_dim.pop_back();
  EXPECT_FALSE(semantic.UpsertImageEmbedding(SemanticImageEmbeddingRecord{.file_id_   = file_id,
                                                                          .image_id_  = image_id,
                                                                          .model_key_ = kModelKey,
                                                                          .embedding_ = wrong_dim},
                                             &error));

  auto non_finite = OneHot(0);
  non_finite[3]   = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(semantic.UpsertImageEmbedding(SemanticImageEmbeddingRecord{.file_id_   = file_id,
                                                                          .image_id_  = image_id,
                                                                          .model_key_ = kModelKey,
                                                                          .embedding_ = non_finite},
                                             &error));

  std::vector<float> zero(kSemanticEmbeddingDim, 0.0F);
  EXPECT_FALSE(semantic.UpsertImageEmbedding(
      SemanticImageEmbeddingRecord{
          .file_id_ = file_id, .image_id_ = image_id, .model_key_ = kModelKey, .embedding_ = zero},
      &error));

  StoreEmbedding(semantic, file_id, image_id, OneHot(0));
  EXPECT_EQ(semantic.CountImageEmbeddings(kModelKey), 1U);

  const auto results = semantic.SearchImageEmbeddings(0, kModelKey, wrong_dim, 0, 10, &error);
  EXPECT_TRUE(results.empty());
  EXPECT_FALSE(error.empty());
}

TEST_F(SemanticStoreTest, Supports768DimensionalModelStorageAndSearch) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();
  RegisterSiglipTestModel(semantic);

  const auto landscape_id = CreateSyntheticFile(project, L"siglip_landscape.raf");
  const auto portrait_id  = CreateSyntheticFile(project, L"siglip_portrait.raf");
  ASSERT_NE(landscape_id, portrait_id);
  const auto rows = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 2U);

  std::string                               error;
  std::vector<SemanticLabelPrototypeRecord> prototypes{
      SemanticLabelPrototypeRecord{.model_key_          = kSiglipModelKey,
                                   .label_              = "landscape",
                                   .prompt_config_hash_ = "siglip-prompts",
                                   .embedding_          = OneHot768(12)},
      SemanticLabelPrototypeRecord{.model_key_          = kSiglipModelKey,
                                   .label_              = "portrait",
                                   .prompt_config_hash_ = "siglip-prompts",
                                   .embedding_          = OneHot768(13)}};
  ASSERT_TRUE(semantic.UpsertLabelPrototypes(prototypes, &error)) << error;
  EXPECT_EQ(semantic.CountLabelPrototypes(kSiglipModelKey, "siglip-prompts"), 2U);

  const auto loaded_prototypes =
      semantic.LoadLabelPrototypes(kSiglipModelKey, "siglip-prompts", &error);
  ASSERT_EQ(loaded_prototypes.size(), 2U) << error;
  EXPECT_EQ(loaded_prototypes.front().embedding.size(),
            static_cast<size_t>(kSemanticEmbeddingDim768));

  for (const auto& row : rows) {
    const auto embedding = row.file_id_ == landscape_id ? OneHot768(12) : OneHot768(13);
    SemanticImageEmbeddingRecord record{.file_id_   = row.file_id_,
                                        .image_id_  = row.image_id_,
                                        .model_key_ = kSiglipModelKey,
                                        .embedding_ = embedding};
    if (row.file_id_ == landscape_id) {
      SemanticLabelAssignmentOptions assignment_options;
      assignment_options.prompt_config_hash_          = "siglip-prompts";
      assignment_options.confidence_score_threshold_  = 0.5;
      assignment_options.confidence_margin_threshold_ = 0.1;
      SemanticImageLabelRecord assigned_label;
      ASSERT_TRUE(semantic.UpsertImageEmbeddingAndAssignLabel(record, assignment_options,
                                                              &assigned_label, &error))
          << error;
      EXPECT_EQ(assigned_label.label_, "landscape");
    } else {
      ASSERT_TRUE(semantic.UpsertImageEmbedding(record, &error)) << error;
    }
  }
  EXPECT_EQ(semantic.CountImageEmbeddings(kSiglipModelKey), 2U);
  EXPECT_EQ(semantic.CountImageLabelsForFile(landscape_id, kSiglipModelKey), 1U);

  ASSERT_TRUE(semantic.EnsureVectorSearchIndex(kSiglipModelKey, &error)) << error;
  const auto results =
      semantic.SearchImageEmbeddings(0, kSiglipModelKey, OneHot768(12), 0, 2, &error);
  ASSERT_FALSE(results.empty()) << error;
  EXPECT_EQ(results.front().file_id_, landscape_id);
}

TEST_F(SemanticStoreTest, NewProjectSeedsDefaultLabelQueries) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();

  EXPECT_EQ(semantic.CountLabelQueries(kDefaultSemanticPhotographyPromptConfigHash),
            DefaultSemanticPhotographyLabelQueries().size());
  EXPECT_EQ(semantic.CountLabelQueries(kDefaultSemanticPhotographyZhPromptConfigHash),
            DefaultSemanticPhotographyLabelQueries(SemanticLabelLanguage::kChinese).size());

  std::string error;
  const auto  queries =
      semantic.ListLabelQueries(kDefaultSemanticPhotographyPromptConfigHash, &error);
  ASSERT_EQ(queries.size(), DefaultSemanticPhotographyLabelQueries().size()) << error;
  const auto& default_queries = DefaultSemanticPhotographyLabelQueries();
  const auto  default_portrait =
      std::find_if(default_queries.begin(), default_queries.end(),
                   [](const SemanticLabelQueryConfig& query) { return query.label == "portrait"; });
  ASSERT_NE(default_portrait, default_queries.end());
  EXPECT_NE(std::find_if(queries.begin(), queries.end(),
                         [default_portrait](const SemanticLabelQueryRecord& query) {
                           return query.label_ == default_portrait->label &&
                                  query.query_text_ == default_portrait->query;
                         }),
            queries.end());

  const auto zh_queries =
      semantic.ListLabelQueries(kDefaultSemanticPhotographyZhPromptConfigHash, &error);
  ASSERT_EQ(zh_queries.size(),
            DefaultSemanticPhotographyLabelQueries(SemanticLabelLanguage::kChinese).size())
      << error;
  EXPECT_NE(std::find_if(zh_queries.begin(), zh_queries.end(),
                         [](const SemanticLabelQueryRecord& query) {
                           return query.label_ == "\xE4\xBA\xBA\xE5\x83\x8F";
                         }),
            zh_queries.end());
  EXPECT_EQ(CanonicalSemanticLabel("\xE9\xA3\x8E\xE6\x99\xAF").value_or(""), "landscape");
  EXPECT_EQ(SemanticLabelDisplayText("landscape", SemanticLabelLanguage::kChinese),
            "\xE9\xA3\x8E\xE6\x99\xAF");
}

TEST_F(SemanticStoreTest, ActiveModelKeyAndLanguageMetadataAreStoredPerModel) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();

  std::string    error;
  ASSERT_TRUE(
      semantic.UpsertModel(SemanticModelRecord{.model_key_     = "mobileclip-en",
                                               .model_id_      = "mobileclip-test",
                                               .revision_      = "en-rev",
                                               .embedding_dim_ = kSemanticEmbeddingDim,
                                               .image_size_    = 256,
                                               .engine_id_     = "onnxruntime",
                                               .profile_id_    = "mobileclip-s2",
                                               .supported_text_languages_json_ = R"(["en"])",
                                               .active_                        = true},
                           &error))
      << error;
  EXPECT_EQ(semantic.ActiveModelKey(), "mobileclip-en");
  EXPECT_EQ(semantic.GetModelSupportedTextLanguagesJson("mobileclip-en"), R"(["en"])");

  ASSERT_TRUE(
      semantic.UpsertModel(SemanticModelRecord{.model_key_     = "multilingual-clip",
                                               .model_id_      = "multilingual-clip-test",
                                               .revision_      = "multi-rev",
                                               .embedding_dim_ = kSemanticEmbeddingDim,
                                               .image_size_    = 256,
                                               .engine_id_     = "onnxruntime",
                                               .profile_id_    = "clip-multilingual",
                                               .supported_text_languages_json_ = R"(["en","zh"])",
                                               .active_                        = true},
                           &error))
      << error;
  EXPECT_EQ(semantic.ActiveModelKey(), "multilingual-clip");
  EXPECT_EQ(semantic.GetModelSupportedTextLanguagesJson("multilingual-clip"), R"(["en","zh"])");

  ASSERT_TRUE(semantic.SetActiveModelKey("mobileclip-en", &error)) << error;
  EXPECT_EQ(semantic.ActiveModelKey(), "mobileclip-en");
  EXPECT_FALSE(semantic.SetActiveModelKey("missing-model", &error));
}

TEST_F(SemanticStoreTest, ExistingSemanticModelWithoutActiveColumnPromotesLatestModel) {
  RunRawSql(db_path_,
            "CREATE TABLE SemanticModel ("
            "model_key VARCHAR PRIMARY KEY,"
            "model_id VARCHAR NOT NULL,"
            "revision VARCHAR NOT NULL,"
            "embedding_dim INTEGER NOT NULL,"
            "image_size INTEGER NOT NULL,"
            "prompt_config_hash VARCHAR,"
            "asset_manifest_json JSON,"
            "created_at TIMESTAMP DEFAULT current_timestamp);"
            "INSERT INTO SemanticModel "
            "(model_key, model_id, revision, embedding_dim, image_size, created_at) VALUES "
            "('old-model', 'old/model', 'rev-a', 512, 256, TIMESTAMP '2026-01-01 00:00:00'),"
            "('latest-model', 'latest/model', 'rev-b', 512, 256, "
            "TIMESTAMP '2026-01-02 00:00:00');");

  Database              db_controller(db_path_);
  SemanticStore semantic(db_controller);
  EXPECT_EQ(semantic.ActiveModelKey(), "latest-model");
}

TEST_F(SemanticStoreTest, PersistsEmbeddingAndLabelTransactionally) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();
  RegisterTestModel(semantic);

  const auto file_id = CreateSyntheticFile(project, L"labeled.raf");
  const auto rows    = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 1U);
  const auto                                image_id = rows.front().image_id_;

  std::string                               error;
  std::vector<SemanticLabelPrototypeRecord> prototypes{
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = "landscape",
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(4)},
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = "portrait",
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(5)}};
  ASSERT_TRUE(semantic.UpsertLabelPrototypes(prototypes, &error)) << error;
  EXPECT_EQ(semantic.CountLabelPrototypes(kModelKey, "test-prompts"), 2U);

  SemanticImageEmbeddingRecord embedding{
      .file_id_ = file_id, .image_id_ = image_id, .model_key_ = kModelKey, .embedding_ = OneHot(4)};
  SemanticImageLabelRecord bad_label{.file_id_   = file_id + 1,
                                     .model_key_ = kModelKey,
                                     .label_     = "landscape",
                                     .score_     = 0.9,
                                     .confident_ = true};
  EXPECT_FALSE(semantic.UpsertImageEmbeddingWithLabel(embedding, &bad_label, &error));
  EXPECT_EQ(semantic.CountImageEmbeddingsForFile(file_id, kModelKey), 0U);
  EXPECT_EQ(semantic.CountImageLabelsForFile(file_id, kModelKey), 0U);

  SemanticImageLabelRecord label{.file_id_         = file_id,
                                 .model_key_       = kModelKey,
                                 .label_           = "landscape",
                                 .score_           = 0.91,
                                 .second_label_    = "portrait",
                                 .second_score_    = 0.12,
                                 .margin_          = 0.79,
                                 .confident_       = true,
                                 .top_scores_json_ = R"([{"label":"landscape","score":0.91}])"};
  ASSERT_TRUE(semantic.UpsertImageEmbeddingWithLabel(embedding, &label, &error)) << error;
  EXPECT_EQ(semantic.CountImageEmbeddingsForFile(file_id, kModelKey), 1U);
  EXPECT_EQ(semantic.CountImageLabelsForFile(file_id, kModelKey), 1U);

  const auto stored_label = semantic.GetImageLabelForFile(file_id, kModelKey, &error);
  ASSERT_TRUE(stored_label.has_value()) << error;
  EXPECT_EQ(stored_label->label_, "landscape");
  EXPECT_EQ(stored_label->second_label_, "portrait");
  EXPECT_TRUE(stored_label->confident_);
  EXPECT_NEAR(stored_label->margin_, 0.79, 1.0e-6);
}

TEST_F(SemanticStoreTest, AssignsLabelInDatabaseTransaction) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();
  RegisterTestModel(semantic);

  const auto file_id = CreateSyntheticFile(project, L"db_labeled.raf");
  const auto rows    = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 1U);
  const auto                                image_id = rows.front().image_id_;

  std::string                               error;
  std::vector<SemanticLabelPrototypeRecord> prototypes{
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = "landscape",
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(4)},
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = "portrait",
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(5)},
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = "architecture",
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(6)},
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = "street",
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(7)},
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = "product",
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(8)}};
  ASSERT_TRUE(semantic.UpsertLabelPrototypes(prototypes, &error)) << error;

  SemanticImageEmbeddingRecord   embedding{.file_id_   = file_id,
                                           .image_id_  = image_id,
                                           .model_key_ = kModelKey,
                                           .embedding_ = MixedQuery(4, 5)};
  SemanticLabelAssignmentOptions missing_options;
  missing_options.prompt_config_hash_ = "missing-prompts";
  EXPECT_FALSE(
      semantic.UpsertImageEmbeddingAndAssignLabel(embedding, missing_options, nullptr, &error));
  EXPECT_EQ(semantic.CountImageEmbeddingsForFile(file_id, kModelKey), 0U);
  EXPECT_EQ(semantic.CountImageLabelsForFile(file_id, kModelKey), 0U);

  SemanticLabelAssignmentOptions assignment_options;
  assignment_options.prompt_config_hash_          = "test-prompts";
  assignment_options.confidence_score_threshold_  = 0.5;
  assignment_options.confidence_margin_threshold_ = 0.1;
  assignment_options.top_score_count_             = 8;

  SemanticImageLabelRecord assigned_label;
  ASSERT_TRUE(semantic.UpsertImageEmbeddingAndAssignLabel(embedding, assignment_options,
                                                          &assigned_label, &error))
      << error;
  EXPECT_EQ(semantic.CountImageEmbeddingsForFile(file_id, kModelKey), 1U);
  EXPECT_EQ(semantic.CountImageLabelsForFile(file_id, kModelKey), 1U);
  EXPECT_EQ(assigned_label.label_, "landscape");
  // MixedQuery(4,5) scores [0.95, 0.05, 0, 0, 0]: a cliff after the top match. The elbow
  // keeps only one label, so the 0.05 runner-up is dropped rather than assigned as a
  // spurious second tag.
  EXPECT_TRUE(assigned_label.second_label_.empty());
  EXPECT_TRUE(assigned_label.confident_);
  EXPECT_NE(assigned_label.top_scores_json_.find("landscape"), std::string::npos);
  EXPECT_EQ(CountSubstring(assigned_label.top_scores_json_, "\"label\""), 1U);

  const auto stored_label = semantic.GetImageLabelForFile(file_id, kModelKey, &error);
  ASSERT_TRUE(stored_label.has_value()) << error;
  EXPECT_EQ(stored_label->label_, "landscape");
  EXPECT_TRUE(stored_label->second_label_.empty());
  EXPECT_TRUE(stored_label->confident_);
}

TEST_F(SemanticStoreTest, AssignsElbowTruncatedLabelsByScoreDistribution) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();
  RegisterTestModel(semantic);

  std::vector<SemanticLabelPrototypeRecord> prototypes{
      SemanticLabelPrototypeRecord{.model_key_ = kModelKey, .label_ = "landscape",
                                   .prompt_config_hash_ = "test-prompts", .embedding_ = OneHot(4)},
      SemanticLabelPrototypeRecord{.model_key_ = kModelKey, .label_ = "portrait",
                                   .prompt_config_hash_ = "test-prompts", .embedding_ = OneHot(5)},
      SemanticLabelPrototypeRecord{.model_key_ = kModelKey, .label_ = "architecture",
                                   .prompt_config_hash_ = "test-prompts", .embedding_ = OneHot(6)},
      SemanticLabelPrototypeRecord{.model_key_ = kModelKey, .label_ = "street",
                                   .prompt_config_hash_ = "test-prompts", .embedding_ = OneHot(7)},
      SemanticLabelPrototypeRecord{.model_key_ = kModelKey, .label_ = "product",
                                   .prompt_config_hash_ = "test-prompts", .embedding_ = OneHot(8)}};
  std::string error;
  ASSERT_TRUE(semantic.UpsertLabelPrototypes(prototypes, &error)) << error;

  SemanticLabelAssignmentOptions assignment_options;
  assignment_options.prompt_config_hash_ = "test-prompts";

  // Two close top matches (0.72 / 0.69) then a cliff to zero: the elbow keeps both, so a
  // genuine runner-up survives as a second tag instead of being clipped by a hard top-1.
  {
    const auto file_id = CreateSyntheticFile(project, L"db_pair.raf");
    const auto rows    = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
    ASSERT_EQ(rows.size(), 1U);
    SemanticImageEmbeddingRecord embedding{.file_id_   = file_id,
                                            .image_id_  = rows.front().image_id_,
                                            .model_key_ = kModelKey,
                                            .embedding_ = ClosePairQuery(4, 5)};
    SemanticImageLabelRecord     assigned;
    ASSERT_TRUE(semantic.UpsertImageEmbeddingAndAssignLabel(embedding, assignment_options,
                                                            &assigned, &error))
        << error;
    EXPECT_EQ(assigned.label_, "landscape");
    EXPECT_EQ(assigned.second_label_, "portrait");
    EXPECT_EQ(CountSubstring(assigned.top_scores_json_, "\"label\""), 2U);
  }

  // Three close top matches (1.0 / 0.9 / 0.8) then a cliff: the elbow keeps all three,
  // hitting the kMaxSemanticImageLabelCount display cap.
  {
    const auto file_id = CreateSyntheticFile(project, L"db_triple.raf");
    const auto rows    = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
    ASSERT_EQ(rows.size(), 2U);
    uint32_t image_id = 0;
    for (const auto& row : rows) {
      if (row.file_id_ == file_id) {
        image_id = row.image_id_;
        break;
      }
    }
    ASSERT_NE(image_id, 0U);
    SemanticImageEmbeddingRecord embedding{
        .file_id_   = file_id,
        .image_id_  = image_id,
        .model_key_ = kModelKey,
        .embedding_ = WeightedQuery({{4, 1.0F}, {5, 0.9F}, {6, 0.8F}})};
    SemanticImageLabelRecord assigned;
    ASSERT_TRUE(semantic.UpsertImageEmbeddingAndAssignLabel(embedding, assignment_options,
                                                            &assigned, &error))
        << error;
    EXPECT_EQ(assigned.label_, "landscape");
    EXPECT_EQ(assigned.second_label_, "portrait");
    EXPECT_EQ(CountSubstring(assigned.top_scores_json_, "\"label\""),
              kMaxSemanticImageLabelCount);
    EXPECT_NE(assigned.top_scores_json_.find("architecture"), std::string::npos);
  }
}

TEST_F(SemanticStoreTest, DeletingFileRemovesSemanticRows) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&          semantic = project.GetStorage()->GetSemanticStore();
  RegisterTestModel(semantic);

  const auto delete_id = CreateSyntheticFile(project, L"delete_me.raf");
  const auto keep_id   = CreateSyntheticFile(project, L"keep_me.raf");

  const auto rows      = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 2U);
  for (const auto& row : rows) {
    StoreEmbedding(semantic, row.file_id_, row.image_id_,
                   row.file_id_ == delete_id ? OneHot(0) : OneHot(1));
  }
  EXPECT_EQ(semantic.CountImageEmbeddings(kModelKey), 2U);

  ASSERT_TRUE(project.GetSleeveService()->DeleteFileEverywhere(delete_id).success_);

  EXPECT_EQ(semantic.CountImageEmbeddingsForFile(delete_id, kModelKey), 0U);
  EXPECT_EQ(semantic.CountImageEmbeddingsForFile(keep_id, kModelKey), 1U);

  std::string error;
  const auto  results =
      semantic.SearchImageEmbeddings(0, kModelKey, MixedQuery(0, 1), 0, 10, &error);
  ASSERT_EQ(results.size(), 1U) << error;
  EXPECT_EQ(results.front().file_id_, keep_id);
}
}  // namespace alcedo
