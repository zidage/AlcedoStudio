//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "support/semantic_stores_test_support.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <clocale>
#include <cmath>
#include <filesystem>
#include <format>
#include <limits>
#include <string>
#include <vector>

#include "app/project_service.hpp"
#include "app/project_package_backend.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "storage/store/database.hpp"
#include "storage/store/duckdb_extension.hpp"

namespace alcedo {
namespace {
constexpr const char* kModelKey       = "mobileclip-test";
constexpr const char* kSiglipModelKey = "siglip2-test";
/// The neutral text embedding of the tests is this axis; file embeddings do not use it unless a
/// test sets a neutral similarity on purpose.
constexpr size_t      kNeutralAxis    = kSemanticEmbeddingDim - 1;

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

auto Negated(std::vector<float> embedding) -> std::vector<float> {
  for (auto& value : embedding) {
    value = -value;
  }
  return embedding;
}

/// A query embedding of the 512-dimensional test model with the neutral vector on kNeutralAxis.
auto QueryOf(std::vector<float> query) -> SemanticQueryEmbedding {
  return SemanticQueryEmbedding{.model_key_ = kModelKey,
                                .dimension_ = kSemanticEmbeddingDim,
                                .query_     = std::move(query),
                                .neutral_   = OneHot(kNeutralAxis)};
}

auto FileIdsOf(const SemanticRankedPage& page) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  ids.reserve(page.rows_.size());
  for (const auto& row : page.rows_) {
    ids.push_back(row.file_id_);
  }
  return ids;
}

void RunSql(duckdb_connection conn, const std::string& sql) {
  duckdb_result result;
  const auto    state = duckdb_query(conn, sql.c_str(), &result);
  EXPECT_EQ(state, DuckDBSuccess)
      << (duckdb_result_error(&result) ? duckdb_result_error(&result) : "") << " in " << sql;
  duckdb_destroy_result(&result);
}

auto CountHnswIndexes(duckdb_connection conn) -> int64_t {
  duckdb_result result;
  if (duckdb_query(conn,
                   "SELECT COUNT(*) FROM duckdb_indexes() "
                   "WHERE lower(COALESCE(sql, '')) LIKE '%using hnsw%';",
                   &result) != DuckDBSuccess) {
    ADD_FAILURE() << (duckdb_result_error(&result) ? duckdb_result_error(&result) : "");
    duckdb_destroy_result(&result);
    return -1;
  }
  const auto count = duckdb_value_int64(&result, 0, 0);
  duckdb_destroy_result(&result);
  return count;
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

void RegisterTestModel(const semantic_test::SemanticStores& semantic) {
  std::string error;
  ASSERT_TRUE(semantic.models_.UpsertModel(SemanticModelRecord{.model_key_     = kModelKey,
                                                       .model_id_      = "mobileclip-test",
                                                       .revision_      = "test-rev",
                                                       .embedding_dim_ = kSemanticEmbeddingDim,
                                                       .image_size_    = 256},
                                   &error))
      << error;
}

void RegisterSiglipTestModel(const semantic_test::SemanticStores& semantic) {
  std::string error;
  ASSERT_TRUE(semantic.models_.UpsertModel(SemanticModelRecord{.model_key_     = kSiglipModelKey,
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

  struct SyntheticFile {
    sl_element_id_t file_id_  = 0;
    image_id_t      image_id_ = 0;
  };

  /// Creates @p count library files named `<prefix>_<index>.raf` with one image pool write and
  /// one Sleeve write. Returns them in creation order.
  static auto CreateSyntheticFiles(ProjectService& project, const std::wstring& prefix,
                                   size_t count) -> std::vector<SyntheticFile> {
    auto                                             image_pool = project.GetImagePoolService();
    std::vector<ImagePoolManager::PinnedImageHandle> pinned;
    std::vector<file_name_t>                         names;
    std::vector<SyntheticFile>                       files(count);
    for (size_t i = 0; i < count; ++i) {
      names.push_back(std::format(L"{}_{:04}.raf", prefix, i));
      auto handle               = image_pool->CreateAndReturnPinnedEmpty();
      handle.Get()->image_name_ = names.back();
      handle.Get()->image_path_ = std::filesystem::path{names.back()};
      files[i].image_id_        = handle.Get()->image_id_;
      pinned.push_back(std::move(handle));
    }
    image_pool->SyncWithStorage();

    auto created = project.GetSleeveService()->Write<std::vector<sl_element_id_t>>(
        [&names, &files](FileSystem& fs) {
          std::vector<sl_element_id_t> ids;
          for (size_t i = 0; i < names.size(); ++i) {
            auto file       = fs.CreateFileInLibrary(names[i]);
            file->image_id_ = files[i].image_id_;
            ids.push_back(file->element_id_);
          }
          return ids;
        });
    EXPECT_TRUE(created.second.success_);
    EXPECT_EQ(created.first.size(), count);
    for (size_t i = 0; i < count && i < created.first.size(); ++i) {
      files[i].file_id_ = created.first[i];
    }
    return files;
  }

  static void StoreEmbedding(const semantic_test::SemanticStores& semantic, sl_element_id_t file_id,
                             image_id_t image_id, std::vector<float> embedding) {
    std::string error;
    ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbedding(
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

TEST_F(SemanticStoreTest, RelevanceRanksWithinRootAndFolderScope) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  const auto mountain_id = CreateSyntheticFile(project, L"mountain.raf");
  const auto beach_id    = CreateSyntheticFile(project, L"beach.raf");
  const auto portrait_id = CreateSyntheticFile(project, L"portrait.raf");

  const auto query       = ClosePairQuery(1, 2);
  const auto rows        = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 3U);
  for (const auto& row : rows) {
    if (row.file_id_ == mountain_id) {
      // Points away from the query: its similarity is below any similarity limit.
      StoreEmbedding(semantic, row.file_id_, row.image_id_, Negated(query));
    } else if (row.file_id_ == beach_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, OneHot(1));
    } else if (row.file_id_ == portrait_id) {
      StoreEmbedding(semantic, row.file_id_, row.image_id_, OneHot(2));
    }
  }

  std::string error;
  const auto  root_page = semantic.search_.SearchImageEmbeddings(0, QueryOf(query), 0, 3, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(FileIdsOf(root_page), (std::vector<sl_element_id_t>{beach_id, portrait_id}));
  EXPECT_EQ(root_page.total_, 2U);

  auto sleeve = project.GetSleeveService();
  auto album  = sleeve->CreateFolder(L"/", L"SemanticScope");
  ASSERT_TRUE(album.second.success_);
  ASSERT_NE(album.first, nullptr);
  ASSERT_TRUE(sleeve->LinkFileToFolder(portrait_id, album.first->element_id_).success_);

  const auto scoped_page = semantic.search_.SearchImageEmbeddings(album.first->element_id_,
                                                                  QueryOf(query), 0, 3, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(FileIdsOf(scoped_page), (std::vector<sl_element_id_t>{portrait_id}));
  EXPECT_EQ(scoped_page.total_, 1U);
}

TEST_F(SemanticStoreTest, PackedSnapshotSucceedsAfterRelevanceSearch) {
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    auto           semantic = semantic_test::StoresOf(*project.GetStorage());
    RegisterTestModel(semantic);

    const auto file_id = CreateSyntheticFile(project, L"indexed.raf");
    const auto rows    = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
    ASSERT_EQ(rows.size(), 1U);
    StoreEmbedding(semantic, file_id, rows.front().image_id_, OneHot(0));

    std::string error;
    const auto  page = semantic.search_.SearchImageEmbeddings(0, QueryOf(OneHot(0)), 0, 1, &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(FileIdsOf(page), (std::vector<sl_element_id_t>{file_id}));
    project.SaveProject(meta_path_);
  }

  std::filesystem::path snapshot_path;
  ASSERT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshot_path, nullptr));
  QString snapshot_error;
  auto    reopened =
      std::make_shared<ProjectService>(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
  EXPECT_TRUE(project_pack::CreateLiveDbSnapshot(reopened, snapshot_path, &snapshot_error))
      << snapshot_error.toStdString();

  std::error_code ec;
  std::filesystem::remove(snapshot_path, ec);
}

TEST_F(SemanticStoreTest, RelevanceKeepsOnlyFilesAboveTheZLimit) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  // 100 files: five aligned with the query (similarity 0.90 to 0.70) and 95 with a small
  // similarity (0 to 0.08). Over this scope only the five have a relevance z value of 2 or more.
  const auto                   files = CreateSyntheticFiles(project, L"z_limit", 100);
  std::vector<sl_element_id_t> aligned;
  for (size_t i = 0; i < files.size(); ++i) {
    const float similarity =
        i < 5 ? 0.90F - (0.05F * static_cast<float>(i)) : 0.02F * static_cast<float>(i % 5);
    StoreEmbedding(semantic, files[i].file_id_, files[i].image_id_,
                   UnitVectorWithSimilarity(0, 1 + i, similarity));
    if (i < 5) {
      aligned.push_back(files[i].file_id_);
    }
  }

  std::string error;
  const auto  page = semantic.search_.SearchImageEmbeddings(0, QueryOf(OneHot(0)), 0, 100, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(FileIdsOf(page), aligned);
  EXPECT_EQ(page.total_, 5U);
  for (size_t i = 1; i < page.rows_.size(); ++i) {
    EXPECT_GT(page.rows_[i - 1].relevance_, page.rows_[i].relevance_);
  }

  const auto scores = semantic.search_.ReadRelevanceScores(0, QueryOf(OneHot(0)), &error);
  ASSERT_EQ(scores.size(), 100U) << error;
  for (const auto& score : scores) {
    const bool is_aligned =
        std::find(aligned.begin(), aligned.end(), score.file_id_) != aligned.end();
    EXPECT_EQ(score.kept_, is_aligned) << score.file_name_;
    ASSERT_TRUE(score.relevance_z_.has_value());
    EXPECT_EQ(*score.relevance_z_ >= SemanticVectorSearch::kMinRelevanceZ, is_aligned)
        << score.file_name_;
  }
}

TEST_F(SemanticStoreTest, UnrelatedQueryKeepsNoFiles) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  const auto files = CreateSyntheticFiles(project, L"unrelated", 40);
  for (size_t i = 0; i < files.size(); ++i) {
    StoreEmbedding(semantic, files[i].file_id_, files[i].image_id_, OneHot(1 + i));
  }

  std::string error;
  const auto  page = semantic.search_.SearchImageEmbeddings(0, QueryOf(OneHot(0)), 0, 24, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_TRUE(page.rows_.empty());
  EXPECT_EQ(page.total_, 0U);

  const auto total_only =
      semantic.search_.SearchImageEmbeddings(0, QueryOf(OneHot(0)), 0, 0, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(total_only.total_, 0U);
}

TEST_F(SemanticStoreTest, NeutralSimilarityDemotesFilesThatMatchEveryText) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  const auto generic_id  = CreateSyntheticFile(project, L"generic.raf");
  const auto specific_id = CreateSyntheticFile(project, L"specific.raf");
  const auto rows        = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 2U);
  for (const auto& row : rows) {
    if (row.file_id_ == generic_id) {
      // c = 0.7 and b = 0.7: it matches the neutral text as well as the query.
      StoreEmbedding(semantic, row.file_id_, row.image_id_,
                     WeightedQuery({{0, 0.7F}, {kNeutralAxis, 0.7F}, {2, 0.141421F}}));
    } else {
      // c = 0.5 and b = 0: a weaker but specific match.
      StoreEmbedding(semantic, row.file_id_, row.image_id_, UnitVectorWithSimilarity(0, 3, 0.5F));
    }
  }

  std::string error;
  const auto  page = semantic.search_.SearchImageEmbeddings(0, QueryOf(OneHot(0)), 0, 10, &error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_EQ(FileIdsOf(page), (std::vector<sl_element_id_t>{specific_id, generic_id}));
  EXPECT_NEAR(page.rows_[0].relevance_, 0.5, 1e-4);
  EXPECT_NEAR(page.rows_[1].relevance_, 0.0, 1e-4);
}

TEST_F(SemanticStoreTest, FolderScopeKeepsChildrenThatRankLowInTheProject) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  // 600 files outside the folder match the query more strongly than the three folder files.
  // A project-wide read of the 512 nearest rows followed by a folder filter loses the three.
  const auto outside = CreateSyntheticFiles(project, L"outside", 600);
  for (size_t i = 0; i < outside.size(); ++i) {
    StoreEmbedding(semantic, outside[i].file_id_, outside[i].image_id_,
                   UnitVectorWithSimilarity(0, 1 + (i % 400), 0.95F));
  }
  const auto inside = CreateSyntheticFiles(project, L"inside", 3);
  for (size_t i = 0; i < inside.size(); ++i) {
    StoreEmbedding(semantic, inside[i].file_id_, inside[i].image_id_,
                   UnitVectorWithSimilarity(0, 401 + i, 0.60F - (0.05F * static_cast<float>(i))));
  }

  auto sleeve = project.GetSleeveService();
  auto folder = sleeve->CreateFolder(L"/", L"SmallFolder");
  ASSERT_TRUE(folder.second.success_);
  ASSERT_NE(folder.first, nullptr);
  for (const auto& file : inside) {
    ASSERT_TRUE(sleeve->LinkFileToFolder(file.file_id_, folder.first->element_id_).success_);
  }

  std::string error;
  const auto  page = semantic.search_.SearchImageEmbeddings(folder.first->element_id_,
                                                            QueryOf(OneHot(0)), 0, 24, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(FileIdsOf(page), (std::vector<sl_element_id_t>{
                                 inside[0].file_id_, inside[1].file_id_, inside[2].file_id_}));
  EXPECT_EQ(page.total_, 3U);
}

TEST_F(SemanticStoreTest, SmallScopeUsesOnlyTheSimilarityLimit) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);
  static_assert(10 < SemanticVectorSearch::kMinScopeStatisticsCount);

  // Ten files around the similarity limit. Most of the files above the limit have a z value
  // below kMinRelevanceZ, so a kept set equal to the above-limit set shows that the scope
  // statistics are not used.
  constexpr std::array<float, 10> kOffsets = {0.30F,   0.20F,  0.12F,  0.08F,  0.04F,
                                              0.015F, -0.015F, -0.04F, -0.08F, -0.12F};
  const auto limit = static_cast<float>(SemanticVectorSearch::kMinQuerySimilarity);
  const auto files = CreateSyntheticFiles(project, L"small_scope", 10);
  std::vector<sl_element_id_t> expected;
  for (size_t i = 0; i < files.size(); ++i) {
    StoreEmbedding(semantic, files[i].file_id_, files[i].image_id_,
                   UnitVectorWithSimilarity(0, 1 + i, limit + kOffsets[i]));
    if (kOffsets[i] > 0.0F) {
      expected.push_back(files[i].file_id_);
    }
  }

  std::string error;
  const auto  page = semantic.search_.SearchImageEmbeddings(0, QueryOf(OneHot(0)), 0, 10, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(FileIdsOf(page), expected);
  EXPECT_EQ(page.total_, expected.size());
}

TEST_F(SemanticStoreTest, PagesOfOneQueryJoinToTheFullOrderedResult) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  // 200 files: 30 aligned files share three similarity values (ties in relevance), and 170
  // files are orthogonal to the query. The 30 aligned files pass the z limit.
  constexpr std::array<float, 3> kAlignedSimilarities = {0.90F, 0.85F, 0.80F};
  const auto                     files = CreateSyntheticFiles(project, L"paging", 200);
  for (size_t i = 0; i < files.size(); ++i) {
    const auto embedding =
        i % 20 < 3 ? UnitVectorWithSimilarity(0, 1, kAlignedSimilarities[(i / 20 + i) % 3])
                   : OneHot(2 + i);
    StoreEmbedding(semantic, files[i].file_id_, files[i].image_id_, embedding);
  }

  std::string error;
  const auto  query = QueryOf(OneHot(0));
  const auto  full  = semantic.search_.SearchImageEmbeddings(0, query, 0, 1000, &error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_EQ(full.rows_.size(), 30U);
  EXPECT_EQ(full.total_, 30U);
  for (size_t i = 1; i < full.rows_.size(); ++i) {
    const auto& previous = full.rows_[i - 1];
    const auto& current  = full.rows_[i];
    EXPECT_TRUE(previous.relevance_ > current.relevance_ ||
                (previous.relevance_ == current.relevance_ && previous.file_id_ < current.file_id_))
        << "row " << i;
  }

  std::vector<sl_element_id_t> joined;
  for (size_t offset = 0;; offset += 7) {
    const auto page = semantic.search_.SearchImageEmbeddings(0, query, offset, 7, &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(page.total_, 30U) << "offset " << offset;
    if (page.rows_.empty()) {
      break;
    }
    for (const auto& row : page.rows_) {
      joined.push_back(row.file_id_);
    }
  }
  EXPECT_EQ(joined, FileIdsOf(full));
}

TEST_F(SemanticStoreTest, FilesWithoutReadyEmbeddingAreNotResults) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  const auto files = CreateSyntheticFiles(project, L"status", 2);
  StoreEmbedding(semantic, files[0].file_id_, files[0].image_id_, OneHot(0));
  StoreEmbedding(semantic, files[1].file_id_, files[1].image_id_, OneHot(0));
  {
    auto guard   = project.GetStorage()->GetDatabase().GetConnectionGuard();
    auto db_lock = guard.Lock();
    RunSql(guard.conn_, std::format("UPDATE SemanticImageEmbedding SET status = 'pending' "
                                    "WHERE file_id = {};",
                                    files[1].file_id_));
  }

  std::string error;
  const auto  page = semantic.search_.SearchImageEmbeddings(0, QueryOf(OneHot(0)), 0, 10, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(FileIdsOf(page), (std::vector<sl_element_id_t>{files[0].file_id_}));
  EXPECT_EQ(page.total_, 1U);
}

TEST_F(SemanticStoreTest, OlderProjectHnswIndexIsDroppedOnOpen) {
  sl_element_id_t file_id = 0;
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    auto           semantic = semantic_test::StoresOf(*project.GetStorage());
    RegisterTestModel(semantic);
    file_id         = CreateSyntheticFile(project, L"older_index.raf");
    const auto rows = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
    ASSERT_EQ(rows.size(), 1U);
    StoreEmbedding(semantic, file_id, rows.front().image_id_, OneHot(0));

    // The index that older versions created on the first natural-language search.
    {
      auto        guard   = project.GetStorage()->GetDatabase().GetConnectionGuard();
      auto        db_lock = guard.Lock();
      std::string load_error;
      ASSERT_TRUE(LoadPackagedDuckDbExtension(guard.conn_, "vss", &load_error)) << load_error;
      RunSql(guard.conn_, "SET hnsw_enable_experimental_persistence = true;");
      RunSql(guard.conn_,
             "CREATE INDEX idx_semantic_image_embedding_hnsw ON SemanticImageEmbedding "
             "USING HNSW (embedding);");
      ASSERT_EQ(CountHnswIndexes(guard.conn_), 1);
    }
    project.SaveProject(meta_path_);
  }

  {
    ProjectService reopened(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
    {
      auto guard   = reopened.GetStorage()->GetDatabase().GetConnectionGuard();
      auto db_lock = guard.Lock();
      EXPECT_EQ(CountHnswIndexes(guard.conn_), 0);
    }
    auto        semantic = semantic_test::StoresOf(*reopened.GetStorage());
    std::string error;
    const auto  page = semantic.search_.SearchImageEmbeddings(0, QueryOf(OneHot(0)), 0, 10, &error);
    ASSERT_TRUE(error.empty()) << error;
    EXPECT_EQ(FileIdsOf(page), (std::vector<sl_element_id_t>{file_id}));
    reopened.SaveProject(meta_path_);
  }

  // The drop is stored in the file: a second open finds no index.
  ProjectService second_open(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
  auto           guard   = second_open.GetStorage()->GetDatabase().GetConnectionGuard();
  auto           db_lock = guard.Lock();
  EXPECT_EQ(CountHnswIndexes(guard.conn_), 0);
}

TEST_F(SemanticStoreTest, RejectsInvalidVectorsBeforeStorageOrSearch) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());

  std::string    error;
  EXPECT_FALSE(semantic.models_.UpsertModel(SemanticModelRecord{.model_key_     = "wrong-dim",
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
  EXPECT_FALSE(semantic.embeddings_.UpsertImageEmbedding(SemanticImageEmbeddingRecord{.file_id_   = file_id,
                                                                          .image_id_  = image_id,
                                                                          .model_key_ = kModelKey,
                                                                          .embedding_ = wrong_dim},
                                             &error));

  auto non_finite = OneHot(0);
  non_finite[3]   = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(semantic.embeddings_.UpsertImageEmbedding(SemanticImageEmbeddingRecord{.file_id_   = file_id,
                                                                          .image_id_  = image_id,
                                                                          .model_key_ = kModelKey,
                                                                          .embedding_ = non_finite},
                                             &error));

  std::vector<float> zero(kSemanticEmbeddingDim, 0.0F);
  EXPECT_FALSE(semantic.embeddings_.UpsertImageEmbedding(
      SemanticImageEmbeddingRecord{
          .file_id_ = file_id, .image_id_ = image_id, .model_key_ = kModelKey, .embedding_ = zero},
      &error));

  StoreEmbedding(semantic, file_id, image_id, OneHot(0));
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddings(kModelKey), 1U);

  error.clear();
  auto wrong_dim_query       = QueryOf(wrong_dim);
  wrong_dim_query.dimension_ = static_cast<int>(wrong_dim.size());
  const auto results = semantic.search_.SearchImageEmbeddings(0, wrong_dim_query, 0, 10, &error);
  EXPECT_TRUE(results.rows_.empty());
  EXPECT_FALSE(error.empty());

  error.clear();
  auto non_finite_query     = QueryOf(OneHot(0));
  non_finite_query.neutral_ = non_finite;
  const auto non_finite_page =
      semantic.search_.SearchImageEmbeddings(0, non_finite_query, 0, 10, &error);
  EXPECT_TRUE(non_finite_page.rows_.empty());
  EXPECT_FALSE(error.empty());
}

TEST_F(SemanticStoreTest, Supports768DimensionalModelStorageAndSearch) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
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
  ASSERT_TRUE(semantic.labels_.UpsertLabelPrototypes(prototypes, &error)) << error;
  EXPECT_EQ(semantic.labels_.CountLabelPrototypes(kSiglipModelKey, "siglip-prompts"), 2U);

  const auto loaded_prototypes =
      semantic.labels_.LoadLabelPrototypes(kSiglipModelKey, "siglip-prompts", &error);
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
      ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbeddingAndAssignLabel(record, assignment_options,
                                                              &assigned_label, &error))
          << error;
      EXPECT_EQ(assigned_label.label_, "landscape");
    } else {
      ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbedding(record, &error)) << error;
    }
  }
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddings(kSiglipModelKey), 2U);
  EXPECT_EQ(semantic.labels_.CountImageLabelsForFile(landscape_id, kSiglipModelKey), 1U);

  const SemanticQueryEmbedding query{.model_key_ = kSiglipModelKey,
                                     .dimension_ = kSemanticEmbeddingDim768,
                                     .query_     = OneHot768(12),
                                     .neutral_   = OneHot768(kSemanticEmbeddingDim768 - 1)};
  error.clear();
  const auto results = semantic.search_.SearchImageEmbeddings(0, query, 0, 2, &error);
  ASSERT_TRUE(error.empty()) << error;
  // Two scored files: only the similarity limit applies, and the portrait file is orthogonal.
  ASSERT_FALSE(results.rows_.empty());
  EXPECT_EQ(results.rows_.front().file_id_, landscape_id);
  EXPECT_NEAR(results.rows_.front().relevance_, 1.0, 1e-5);
}

TEST_F(SemanticStoreTest, NewProjectPopulatesDefaultLabelQueries) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());

  EXPECT_EQ(semantic.labels_.CountLabelQueries(kDefaultSemanticPhotographyPromptConfigHash),
            DefaultSemanticPhotographyLabelQueries().size());
  EXPECT_EQ(semantic.labels_.CountLabelQueries(kDefaultSemanticPhotographyZhPromptConfigHash),
            DefaultSemanticPhotographyLabelQueries(SemanticLabelLanguage::kChinese).size());

  std::string error;
  const auto  queries =
      semantic.labels_.ListLabelQueries(kDefaultSemanticPhotographyPromptConfigHash, &error);
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
      semantic.labels_.ListLabelQueries(kDefaultSemanticPhotographyZhPromptConfigHash, &error);
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

// Qt runs the app under the user's UTF-8 locale on macOS, where <cctype> treats 0xA0 as
// whitespace. 沙漠 ends in byte 0xA0, so a <cctype> trim cut the label in half and the label SQL
// that opening a project runs failed with "Invalid unicode".
TEST(SemanticLabelConfigTest, NormalizeKeepsUtf8LabelsIntactUnderUtf8Locale) {
  const char* previous = std::setlocale(LC_ALL, nullptr);
  const std::string saved_locale = previous != nullptr ? previous : "C";
  for (const char* name : {"en_US.UTF-8", "C.UTF-8", "zh_CN.UTF-8", ".UTF8"}) {
    if (std::setlocale(LC_ALL, name) != nullptr) {
      break;
    }
  }

  const std::string interior = "\xE5\xAE\xA4\xE5\x86\x85";  // 室内
  const std::string desert   = "\xE6\xB2\x99\xE6\xBC\xA0";  // 沙漠
  EXPECT_EQ(NormalizeSemanticLabelKey(interior), interior);
  EXPECT_EQ(NormalizeSemanticLabelKey(desert), desert);
  EXPECT_EQ(NormalizeSemanticLabelKey(" \tLandscape \n"), "landscape");
  for (const auto& entry : DefaultSemanticPhotographyLabelDefinitions()) {
    EXPECT_EQ(NormalizeSemanticLabelKey(entry.chinese_label), entry.chinese_label);
  }

  std::setlocale(LC_ALL, saved_locale.c_str());
  EXPECT_EQ(CanonicalSemanticLabel(interior).value_or(""), "interior");
  EXPECT_EQ(CanonicalSemanticLabel(desert).value_or(""), "desert");
}

TEST_F(SemanticStoreTest, ActiveModelKeyAndLanguageMetadataAreStoredPerModel) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());

  std::string    error;
  ASSERT_TRUE(
      semantic.models_.UpsertModel(SemanticModelRecord{.model_key_     = "mobileclip-en",
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
  EXPECT_EQ(semantic.models_.ActiveModelKey(), "mobileclip-en");
  EXPECT_EQ(semantic.models_.GetModelSupportedTextLanguagesJson("mobileclip-en"), R"(["en"])");

  ASSERT_TRUE(
      semantic.models_.UpsertModel(SemanticModelRecord{.model_key_     = "multilingual-clip",
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
  EXPECT_EQ(semantic.models_.ActiveModelKey(), "multilingual-clip");
  EXPECT_EQ(semantic.models_.GetModelSupportedTextLanguagesJson("multilingual-clip"), R"(["en","zh"])");

  ASSERT_TRUE(semantic.models_.SetActiveModelKey("mobileclip-en", &error)) << error;
  EXPECT_EQ(semantic.models_.ActiveModelKey(), "mobileclip-en");
  EXPECT_FALSE(semantic.models_.SetActiveModelKey("missing-model", &error));
}

TEST_F(SemanticStoreTest, ClearActiveModelLeavesNoActiveModelAndKeepsRows) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  std::string    error;
  ASSERT_TRUE(
      semantic.models_.UpsertModel(SemanticModelRecord{.model_key_     = "mobileclip-en",
                                                       .model_id_      = "mobileclip-test",
                                                       .revision_      = "en-rev",
                                                       .embedding_dim_ = kSemanticEmbeddingDim,
                                                       .image_size_    = 256,
                                                       .active_        = true},
                                   &error))
      << error;
  ASSERT_EQ(semantic.models_.ActiveModelKey(), "mobileclip-en");

  ASSERT_TRUE(semantic.models_.ClearActiveModel(&error)) << error;
  EXPECT_TRUE(semantic.models_.ActiveModelKey().empty());
  EXPECT_FALSE(semantic.models_.ActiveModel(&error).has_value());
  EXPECT_TRUE(semantic.models_.HasModel("mobileclip-en"));
  ASSERT_TRUE(semantic.models_.SetActiveModelKey("mobileclip-en", &error)) << error;
  EXPECT_EQ(semantic.models_.ActiveModelKey(), "mobileclip-en");
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
  SemanticModelRegistry models(db_controller);
  EXPECT_EQ(models.ActiveModelKey(), "latest-model");
}

TEST_F(SemanticStoreTest, PersistsEmbeddingAndLabelTransactionally) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
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
  ASSERT_TRUE(semantic.labels_.UpsertLabelPrototypes(prototypes, &error)) << error;
  EXPECT_EQ(semantic.labels_.CountLabelPrototypes(kModelKey, "test-prompts"), 2U);

  SemanticImageEmbeddingRecord embedding{
      .file_id_ = file_id, .image_id_ = image_id, .model_key_ = kModelKey, .embedding_ = OneHot(4)};
  SemanticImageLabelRecord bad_label{.file_id_   = file_id + 1,
                                     .model_key_ = kModelKey,
                                     .label_     = "landscape",
                                     .score_     = 0.9,
                                     .confident_ = true};
  EXPECT_FALSE(semantic.embeddings_.UpsertImageEmbeddingWithLabel(embedding, &bad_label, &error));
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddingsForFile(file_id, kModelKey), 0U);
  EXPECT_EQ(semantic.labels_.CountImageLabelsForFile(file_id, kModelKey), 0U);

  SemanticImageLabelRecord label{.file_id_         = file_id,
                                 .model_key_       = kModelKey,
                                 .label_           = "landscape",
                                 .score_           = 0.91,
                                 .second_label_    = "portrait",
                                 .second_score_    = 0.12,
                                 .margin_          = 0.79,
                                 .confident_       = true,
                                 .top_scores_json_ = R"([{"label":"landscape","score":0.91}])"};
  ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbeddingWithLabel(embedding, &label, &error)) << error;
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddingsForFile(file_id, kModelKey), 1U);
  EXPECT_EQ(semantic.labels_.CountImageLabelsForFile(file_id, kModelKey), 1U);

  const auto stored_label = semantic.labels_.GetImageLabelForFile(file_id, kModelKey, &error);
  ASSERT_TRUE(stored_label.has_value()) << error;
  EXPECT_EQ(stored_label->label_, "landscape");
  EXPECT_EQ(stored_label->second_label_, "portrait");
  EXPECT_TRUE(stored_label->confident_);
  EXPECT_NEAR(stored_label->margin_, 0.79, 1.0e-6);
}

TEST_F(SemanticStoreTest, AssignsLabelInDatabaseTransaction) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
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
  ASSERT_TRUE(semantic.labels_.UpsertLabelPrototypes(prototypes, &error)) << error;

  SemanticImageEmbeddingRecord   embedding{.file_id_   = file_id,
                                           .image_id_  = image_id,
                                           .model_key_ = kModelKey,
                                           .embedding_ = MixedQuery(4, 5)};
  SemanticLabelAssignmentOptions missing_options;
  missing_options.prompt_config_hash_ = "missing-prompts";
  EXPECT_FALSE(
      semantic.embeddings_.UpsertImageEmbeddingAndAssignLabel(embedding, missing_options, nullptr, &error));
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddingsForFile(file_id, kModelKey), 0U);
  EXPECT_EQ(semantic.labels_.CountImageLabelsForFile(file_id, kModelKey), 0U);

  SemanticLabelAssignmentOptions assignment_options;
  assignment_options.prompt_config_hash_          = "test-prompts";
  assignment_options.confidence_score_threshold_  = 0.5;
  assignment_options.confidence_margin_threshold_ = 0.1;
  assignment_options.top_score_count_             = 8;

  SemanticImageLabelRecord assigned_label;
  ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbeddingAndAssignLabel(embedding, assignment_options,
                                                          &assigned_label, &error))
      << error;
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddingsForFile(file_id, kModelKey), 1U);
  EXPECT_EQ(semantic.labels_.CountImageLabelsForFile(file_id, kModelKey), 1U);
  EXPECT_EQ(assigned_label.label_, "landscape");
  // MixedQuery(4,5) scores [0.95, 0.05, 0, 0, 0]: a cliff after the top match. The elbow
  // keeps only one label, so the 0.05 runner-up is dropped rather than assigned as a
  // spurious second tag.
  EXPECT_TRUE(assigned_label.second_label_.empty());
  EXPECT_TRUE(assigned_label.confident_);
  EXPECT_NE(assigned_label.top_scores_json_.find("landscape"), std::string::npos);
  EXPECT_EQ(CountSubstring(assigned_label.top_scores_json_, "\"label\""), 1U);

  const auto stored_label = semantic.labels_.GetImageLabelForFile(file_id, kModelKey, &error);
  ASSERT_TRUE(stored_label.has_value()) << error;
  EXPECT_EQ(stored_label->label_, "landscape");
  EXPECT_TRUE(stored_label->second_label_.empty());
  EXPECT_TRUE(stored_label->confident_);
}

// A Chinese prompt profile ranks prototypes that carry Chinese label text. The label, second
// label, and top scores store the canonical keys; the UI translates them for display.
TEST_F(SemanticStoreTest, StoresCanonicalKeysForChinesePromptLabels) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  const std::string                         landscape_zh = "\xE9\xA3\x8E\xE6\x99\xAF";  // 风景
  const std::string                         portrait_zh  = "\xE4\xBA\xBA\xE5\x83\x8F";  // 人像
  std::vector<SemanticLabelPrototypeRecord> prototypes{
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = landscape_zh,
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(4)},
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = portrait_zh,
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(5)},
      SemanticLabelPrototypeRecord{.model_key_          = kModelKey,
                                   .label_              = "\xE5\xBB\xBA\xE7\xAD\x91",
                                   .prompt_config_hash_ = "test-prompts",
                                   .embedding_          = OneHot(6)}};
  std::string error;
  ASSERT_TRUE(semantic.labels_.UpsertLabelPrototypes(prototypes, &error)) << error;

  const auto file_id = CreateSyntheticFile(project, L"zh_pair.raf");
  const auto rows    = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 1U);
  SemanticLabelAssignmentOptions assignment_options;
  assignment_options.prompt_config_hash_ = "test-prompts";
  SemanticImageEmbeddingRecord embedding{.file_id_   = file_id,
                                         .image_id_  = rows.front().image_id_,
                                         .model_key_ = kModelKey,
                                         .embedding_ = ClosePairQuery(4, 5)};
  SemanticImageLabelRecord     assigned;
  ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbeddingAndAssignLabel(embedding, assignment_options,
                                                                      &assigned, &error))
      << error;

  const auto stored = semantic.labels_.GetImageLabelForFile(file_id, kModelKey, &error);
  ASSERT_TRUE(stored.has_value()) << error;
  EXPECT_EQ(stored->label_, "landscape");
  EXPECT_EQ(stored->second_label_, "portrait");
  EXPECT_EQ(CountSubstring(stored->top_scores_json_, "\"landscape\""), 1U);
  EXPECT_EQ(CountSubstring(stored->top_scores_json_, "\"portrait\""), 1U);
  EXPECT_EQ(stored->top_scores_json_.find(landscape_zh), std::string::npos);
  EXPECT_EQ(stored->top_scores_json_.find(portrait_zh), std::string::npos);
}

// Projects from older versions hold the label text of the model's prompt language. Opening
// the project rewrites taxonomy aliases in the label, second label, and top scores to the
// canonical keys, leaves other text alone, and a second run changes nothing.
TEST_F(SemanticStoreTest, OpeningOldProjectRewritesAliasLabelsToCanonicalKeys) {
  sl_element_id_t alias_file  = 0;
  sl_element_id_t custom_file = 0;
  {
    ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    RegisterTestModel(semantic_test::StoresOf(*project.GetStorage()));
    alias_file  = CreateSyntheticFile(project, L"old_alias.raf");
    custom_file = CreateSyntheticFile(project, L"old_custom.raf");
    project.SaveProject(meta_path_);
  }
  // Rows as an older version wrote them for a Chinese prompt profile: 沙漠 (desert) and
  // 风景 (landscape), plus a label that no taxonomy definition lists.
  const std::string insert =
      "INSERT INTO SemanticImageLabel (file_id, model_key, label, score, second_label, "
      "second_score, confident, top_scores) VALUES (" +
      std::to_string(alias_file) + ", '" + kModelKey +
      "', '\xE6\xB2\x99\xE6\xBC\xA0', 0.6, '\xE9\xA3\x8E\xE6\x99\xAF', 0.5, TRUE, "
      "'[{\"label\":\"\xE6\xB2\x99\xE6\xBC\xA0\",\"score\":0.6},{\"label\":"
      "\"\xE9\xA3\x8E\xE6\x99\xAF\",\"score\":0.5}]'), (" +
      std::to_string(custom_file) + ", '" + kModelKey +
      "', 'my custom', 0.9, NULL, NULL, TRUE, '[{\"label\":\"my custom\",\"score\":0.9}]');";
  RunRawSql(db_path_, insert.c_str());

  ProjectService reopened(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
  auto           semantic = semantic_test::StoresOf(*reopened.GetStorage());
  std::string    error;

  const auto     alias_label = semantic.labels_.GetImageLabelForFile(alias_file, kModelKey, &error);
  ASSERT_TRUE(alias_label.has_value()) << error;
  EXPECT_EQ(alias_label->label_, "desert");
  EXPECT_EQ(alias_label->second_label_, "landscape");
  EXPECT_EQ(CountSubstring(alias_label->top_scores_json_, "\"desert\""), 1U);
  EXPECT_EQ(CountSubstring(alias_label->top_scores_json_, "\"landscape\""), 1U);
  EXPECT_EQ(alias_label->top_scores_json_.find("\xE6\xB2\x99\xE6\xBC\xA0"), std::string::npos);
  EXPECT_EQ(alias_label->top_scores_json_.find("\xE9\xA3\x8E\xE6\x99\xAF"), std::string::npos);

  const auto custom_label = semantic.labels_.GetImageLabelForFile(custom_file, kModelKey, &error);
  ASSERT_TRUE(custom_label.has_value()) << error;
  EXPECT_EQ(custom_label->label_, "my custom");
  EXPECT_EQ(CountSubstring(custom_label->top_scores_json_, "\"my custom\""), 1U);

  const auto second_run = semantic.labels_.CanonicalizeImageLabels(&error);
  ASSERT_TRUE(second_run.has_value()) << error;
  EXPECT_EQ(*second_run, 0U);
}

TEST_F(SemanticStoreTest, AssignsElbowTruncatedLabelsByScoreDistribution) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
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
  ASSERT_TRUE(semantic.labels_.UpsertLabelPrototypes(prototypes, &error)) << error;

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
    ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbeddingAndAssignLabel(embedding, assignment_options,
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
    ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbeddingAndAssignLabel(embedding, assignment_options,
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
  auto           semantic = semantic_test::StoresOf(*project.GetStorage());
  RegisterTestModel(semantic);

  const auto delete_id = CreateSyntheticFile(project, L"delete_me.raf");
  const auto keep_id   = CreateSyntheticFile(project, L"keep_me.raf");

  const auto rows      = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(rows.size(), 2U);
  for (const auto& row : rows) {
    StoreEmbedding(semantic, row.file_id_, row.image_id_,
                   row.file_id_ == delete_id ? OneHot(0) : OneHot(1));
  }
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddings(kModelKey), 2U);

  ASSERT_TRUE(project.GetSleeveService()->DeleteFileEverywhere(delete_id).success_);

  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddingsForFile(delete_id, kModelKey), 0U);
  EXPECT_EQ(semantic.embeddings_.CountImageEmbeddingsForFile(keep_id, kModelKey), 1U);

  std::string error;
  const auto  results =
      semantic.search_.SearchImageEmbeddings(0, QueryOf(MixedQuery(1, 0)), 0, 10, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(FileIdsOf(results), (std::vector<sl_element_id_t>{keep_id}));
}
}  // namespace alcedo
