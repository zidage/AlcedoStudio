//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/ai/ai_store.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "ai/ai_description.hpp"
#include "ai/ai_rating.hpp"
#include "app/project_service.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "storage/store/database.hpp"

namespace alcedo {
namespace {

// Count rows directly on the project's own Database connection (the same DuckDB
// instance the controller under test uses), so there is no second duckdb_open contending
// for the file. This is test verification, not production ser/deser, so a raw COUNT query
// is appropriate here.
auto CountUnderstandingRows(Database& db) -> size_t {
  auto          guard = db.GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_, "SELECT COUNT(*) FROM AiImageUnderstanding;", &result) !=
      DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return 0;
  }
  const auto n = static_cast<size_t>(duckdb_value_int64(&result, 0, 0));
  duckdb_destroy_result(&result);
  return n;
}

auto CountFtsDocumentRows(Database& db) -> size_t {
  auto          guard = db.GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_, "SELECT COUNT(*) FROM AiImageFtsDocument;", &result) !=
      DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return 0;
  }
  const auto n = static_cast<size_t>(duckdb_value_int64(&result, 0, 0));
  duckdb_destroy_result(&result);
  return n;
}

void DropFtsDocumentTable(Database& db) {
  auto          guard = db.GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  duckdb_query(guard.conn_, "DROP TABLE IF EXISTS AiImageFtsDocument;", &result);
  duckdb_destroy_result(&result);
}

auto FtsDocumentBody(Database& db, sl_element_id_t file_id) -> std::string {
  auto          guard = db.GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  const auto    sql = std::string("SELECT body FROM AiImageFtsDocument WHERE file_id = ") +
                   std::to_string(file_id) + ";";
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    duckdb_destroy_result(&result);
    return {};
  }
  std::string body;
  if (duckdb_row_count(&result) > 0 && !duckdb_value_is_null(&result, 0, 0)) {
    char* raw = duckdb_value_varchar(&result, 0, 0);
    if (raw) {
      body = raw;
      duckdb_free(raw);
    }
  }
  duckdb_destroy_result(&result);
  return body;
}

auto MakeUnderstanding(sl_element_id_t file_id, const std::string& task_id,
                       const std::string& caption, std::vector<std::string> tags) -> AiDescription {
  AiDescription d;
  d.file_id_           = file_id;
  d.task_id_           = task_id;
  d.provider_id_       = "openrouter";
  d.model_id_          = "test-model";
  d.prompt_profile_id_ = "profile-v1";
  d.rendition_kind_    = "thumbnail_k1024";
  d.caption_           = caption;
  d.scene_             = "outdoor";
  d.confidence_        = 0.81;
  d.active_            = true;
  d.SetTags(tags);
  return d;
}

auto MakeRating(sl_element_id_t file_id, const std::string& task_id, int rating,
                const std::string& reasons) -> AiRating {
  AiRating r;
  r.file_id_           = file_id;
  r.task_id_           = task_id;
  r.provider_id_       = "openrouter";
  r.model_id_          = "test-model";
  r.prompt_profile_id_ = "profile-v1";
  r.rendition_kind_    = "thumbnail_k1024";
  r.rating_            = rating;
  r.rubric_id_         = "default-rubric";
  r.rubric_version_    = "v1";
  r.reasons_           = reasons;
  r.active_            = true;
  return r;
}
}  // namespace

class AiStoreTest : public ::testing::Test {
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
};

// Insert + retrieve an understanding; every identity, content, and confidence field
// round-trips through the duckorm layer, and tags survive the JSON serialization.
TEST_F(AiStoreTest, UpsertAndRetrieveUnderstanding) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_understanding_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();
  ASSERT_TRUE(ai.UpsertUnderstanding(
      MakeUnderstanding(file_id, "describe", "Sahara dunes at sunset", {"desert", "sand"})));

  const auto got = ai.GetUnderstanding(file_id, "describe");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->file_id_, file_id);
  EXPECT_EQ(got->task_id_, "describe");
  EXPECT_EQ(got->provider_id_, "openrouter");
  EXPECT_EQ(got->model_id_, "test-model");
  EXPECT_EQ(got->prompt_profile_id_, "profile-v1");
  EXPECT_EQ(got->rendition_kind_, "thumbnail_k1024");
  EXPECT_EQ(got->caption_, "Sahara dunes at sunset");
  EXPECT_EQ(got->scene_, "outdoor");
  EXPECT_DOUBLE_EQ(got->confidence_, 0.81);
  EXPECT_TRUE(got->active_);
  const auto tags = got->Tags();
  ASSERT_EQ(tags.size(), 2u);
  EXPECT_EQ(tags[0], "desert");
  EXPECT_EQ(tags[1], "sand");
}

// insert_or_replace on PRIMARY KEY (file_id, task_id) must replace, not append, so there
// is at most one row — hence at most one active-for-search understanding — per pair.
TEST_F(AiStoreTest, ReplaceUnderstandingForSamePairKeepsOneRow) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_replace_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();

  ASSERT_TRUE(
      ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe", "old caption", {"old"})));
  ASSERT_EQ(CountUnderstandingRows(project.GetStorage()->GetDatabase()), 1u);
  ASSERT_TRUE(
      ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe", "new caption", {"new"})));
  EXPECT_EQ(CountUnderstandingRows(project.GetStorage()->GetDatabase()), 1u)
      << "insert_or_replace must not append a second row for the same (file_id, task_id)";

  const auto got = ai.GetUnderstanding(file_id, "describe");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->caption_, "new caption");
  ASSERT_EQ(got->Tags().size(), 1u);
  EXPECT_EQ(got->Tags()[0], "new");
}

// Distinct task_ids for the same file are independent rows (e.g. two prompt-profile runs
// kept as history). The "at most one per (file_id, task_id)" invariant constrains each
// pair, not the file as a whole.
TEST_F(AiStoreTest, DifferentTaskIdsForSameFileCoexist) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_tasks_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();

  ASSERT_TRUE(
      ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe-v1", "caption v1", {"a"})));
  ASSERT_TRUE(
      ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe-v2", "caption v2", {"b"})));
  EXPECT_EQ(CountUnderstandingRows(project.GetStorage()->GetDatabase()), 2u);
  EXPECT_EQ(ai.GetUnderstanding(file_id, "describe-v1")->caption_, "caption v1");
  EXPECT_EQ(ai.GetUnderstanding(file_id, "describe-v2")->caption_, "caption v2");
}

TEST_F(AiStoreTest, BatchUpsertUnderstandingUpdatesFtsDocumentsOnce) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     alpha_id = CreateSyntheticFile(project, L"ai_batch_alpha.dng");
  const auto     beta_id  = CreateSyntheticFile(project, L"ai_batch_beta.dng");
  ASSERT_NE(alpha_id, 0u);
  ASSERT_NE(beta_id, 0u);
  auto&                            ai = project.GetStorage()->GetAiStore();

  const std::vector<AiDescription> descriptions{
      MakeUnderstanding(alpha_id, "describe", "sahara dunes at sunset", {"desert", "sand"}),
      MakeUnderstanding(beta_id, "describe", "city street at night", {"urban"})};
  EXPECT_EQ(ai.UpsertUnderstandings(descriptions), 2u);
  EXPECT_EQ(CountUnderstandingRows(project.GetStorage()->GetDatabase()), 2u);
  EXPECT_EQ(CountFtsDocumentRows(project.GetStorage()->GetDatabase()), 2u);

  const auto alpha_body = FtsDocumentBody(project.GetStorage()->GetDatabase(), alpha_id);
  EXPECT_NE(alpha_body.find("sahara dunes"), std::string::npos);
  EXPECT_NE(alpha_body.find("desert"), std::string::npos);

  const auto beta_body = FtsDocumentBody(project.GetStorage()->GetDatabase(), beta_id);
  EXPECT_NE(beta_body.find("city street"), std::string::npos);
  EXPECT_NE(beta_body.find("urban"), std::string::npos);
}

TEST_F(AiStoreTest, UpsertUnderstandingCreatesMissingFtsDocumentTable) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_missing_fts_doc_alpha.dng");
  ASSERT_NE(file_id, 0u);

  DropFtsDocumentTable(project.GetStorage()->GetDatabase());
  auto& ai = project.GetStorage()->GetAiStore();
  ASSERT_TRUE(ai.UpsertUnderstanding(
      MakeUnderstanding(file_id, "describe", "sahara dunes at sunset", {"desert"})));

  EXPECT_EQ(CountFtsDocumentRows(project.GetStorage()->GetDatabase()), 1u);
  const auto body = FtsDocumentBody(project.GetStorage()->GetDatabase(), file_id);
  EXPECT_NE(body.find("sahara dunes"), std::string::npos);
  EXPECT_NE(body.find("desert"), std::string::npos);
}

// A partial/invalid understanding (missing provider/model identity) is rejected so a
// failed remote call never leaves a partial active search document.
TEST_F(AiStoreTest, InvalidUnderstandingNotPersisted) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_invalid_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai         = project.GetStorage()->GetAiStore();

  auto  bad        = MakeUnderstanding(file_id, "describe", "partial", {});
  bad.provider_id_ = "";  // IsValid() == false
  EXPECT_FALSE(ai.UpsertUnderstanding(bad));
  EXPECT_FALSE(ai.GetUnderstanding(file_id, "describe").has_value());
  EXPECT_EQ(CountUnderstandingRows(project.GetStorage()->GetDatabase()), 0u);
}

TEST_F(AiStoreTest, GetActiveUnderstandingReturnsPersistedRow) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_active_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();
  ASSERT_TRUE(ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe", "active caption", {})));
  const auto got = ai.GetActiveUnderstanding(file_id);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->caption_, "active caption");
}

// Rating persists and round-trips; the 1..5 integer and rubric identity survive.
TEST_F(AiStoreTest, UpsertAndRetrieveRating) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_rating_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();

  ASSERT_TRUE(ai.UpsertRating(MakeRating(file_id, "rate", 4, "strong composition")));
  const auto got = ai.GetRating(file_id, "rate");
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->file_id_, file_id);
  EXPECT_EQ(got->rating_, 4);
  EXPECT_EQ(got->rubric_id_, "default-rubric");
  EXPECT_EQ(got->rubric_version_, "v1");
  EXPECT_EQ(got->reasons_, "strong composition");
  EXPECT_EQ(got->provider_id_, "openrouter");
  EXPECT_EQ(got->model_id_, "test-model");
  EXPECT_TRUE(got->active_);
}

// A rating of 0 ("unset") is rejected so a scored image is never confused with an
// unrated one.
TEST_F(AiStoreTest, UnsetRatingNotPersisted) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_rating_unset_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();
  EXPECT_FALSE(ai.UpsertRating(MakeRating(file_id, "rate", 0, "unset")));
  EXPECT_FALSE(ai.GetRating(file_id, "rate").has_value());
}

// ── Phase 7a: reasons-only persistence path ─────────────────────────────────

// The 7a gate ignores the rating value and requires file key + provider/model identity +
// non-empty reasons. A rating=0 row (the 7a sentinel) is storable through this path even
// though `IsValid()` rejects it.
TEST_F(AiStoreTest, IsValidReasonsOnlyAcceptsZeroSentinelAndRejectsEmptyReasons) {
  const auto file_id  = static_cast<sl_element_id_t>(42);
  // rating=0 sentinel with reasons — reasons-only valid, but IsValid (strict) rejects it.
  const auto sentinel = MakeRating(file_id, "rate", 0, "rationale");
  EXPECT_FALSE(sentinel.IsValid());            // 5f strict gate: rating must be 1..5
  EXPECT_TRUE(sentinel.IsValidReasonsOnly());  // 7a gate: rating ignored, reasons present
  // Empty reasons — rejected by the reasons-only gate.
  const auto no_reasons = MakeRating(file_id, "rate", 0, "");
  EXPECT_FALSE(no_reasons.IsValidReasonsOnly());
  // Missing identity — rejected even with reasons.
  auto no_identity = MakeRating(file_id, "rate", 0, "rationale");
  no_identity.provider_id_.clear();
  EXPECT_FALSE(no_identity.IsValidReasonsOnly());
  // A real 1..5 rating is also reasons-only-valid (rating ignored).
  EXPECT_TRUE(MakeRating(file_id, "rate", 5, "rationale").IsValidReasonsOnly());
}

// UpsertRatingReasons writes a row with rating = 0 (sentinel) + reasons + identity; the
// active row is retrievable via GetActiveRating. The stored rating is the 0 sentinel, NOT
// the real score (the truth is the EXIF/metadata `Rating` value).
TEST_F(AiStoreTest, UpsertRatingReasonsInsertsZeroSentinelRow) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_reasons_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();

  ASSERT_TRUE(ai.UpsertRatingReasons(MakeRating(file_id, "rate", 0, "strong composition")));
  const auto got = ai.GetActiveRating(file_id);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->file_id_, file_id);
  EXPECT_EQ(got->rating_, 0);  // sentinel — NOT the real score
  EXPECT_EQ(got->reasons_, "strong composition");
  EXPECT_EQ(got->rubric_id_, "default-rubric");
  EXPECT_EQ(got->rubric_version_, "v1");
  EXPECT_EQ(got->provider_id_, "openrouter");
  EXPECT_EQ(got->model_id_, "test-model");
  EXPECT_TRUE(got->active_);
}

// The (file_id, task_id) PK replaces the prior reasons row in place — at most one active
// reasons row per pair.
TEST_F(AiStoreTest, UpsertRatingReasonsReplacesPriorRowForSamePair) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_reasons_replace_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();

  ASSERT_TRUE(ai.UpsertRatingReasons(MakeRating(file_id, "rate", 0, "old rationale")));
  ASSERT_TRUE(ai.UpsertRatingReasons(MakeRating(file_id, "rate", 0, "new rationale")));
  const auto got = ai.GetActiveRating(file_id);
  ASSERT_TRUE(got.has_value());
  EXPECT_EQ(got->reasons_, "new rationale");
  EXPECT_EQ(got->rating_, 0);
}

// Empty reasons are rejected at the storage boundary — no row is written.
TEST_F(AiStoreTest, UpsertRatingReasonsRejectsEmptyReasons) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_reasons_empty_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();
  EXPECT_FALSE(ai.UpsertRatingReasons(MakeRating(file_id, "rate", 0, "")));
  EXPECT_FALSE(ai.GetActiveRating(file_id).has_value());
}

// A reasons-only row is dropped by the file-deletion cascade (same as a full rating row).
TEST_F(AiStoreTest, DeleteForFilesRemovesReasonsOnlyRow) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_reasons_delete_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();
  ASSERT_TRUE(ai.UpsertRatingReasons(MakeRating(file_id, "rate", 0, "rationale")));
  ASSERT_TRUE(ai.GetActiveRating(file_id).has_value());

  ai.DeleteForFiles(std::span<const sl_element_id_t>(&file_id, 1));
  EXPECT_FALSE(ai.GetActiveRating(file_id).has_value());
}

// Understanding and rating live in separate tables and must not bleed into each other's
// retrieval: the rating's reasons never appear on the understanding object, and the
// understanding's caption never appears on the rating object.
TEST_F(AiStoreTest, RatingAndUnderstandingAreIsolated) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_iso_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();

  ASSERT_TRUE(ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe", "sahara caption", {})));
  ASSERT_TRUE(ai.UpsertRating(MakeRating(file_id, "rate", 5, "golden ratio composition")));

  const auto u = ai.GetUnderstanding(file_id, "describe");
  ASSERT_TRUE(u.has_value());
  EXPECT_EQ(u->caption_, "sahara caption");
  EXPECT_EQ(u->caption_.find("golden"), std::string::npos);
  EXPECT_EQ(u->tags_json_.find("golden"), std::string::npos);

  const auto r = ai.GetRating(file_id, "rate");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->reasons_, "golden ratio composition");
  EXPECT_EQ(r->rating_, 5);
  EXPECT_EQ(r->reasons_.find("sahara"), std::string::npos);
}

// Provider/model/prompt identity is preserved per row, so a prompt/profile change on a
// later run cannot be misread as the old row's identity.
TEST_F(AiStoreTest, ProviderModelPromptIdentityPreserved) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_identity_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai             = project.GetStorage()->GetAiStore();

  auto  u              = MakeUnderstanding(file_id, "describe", "caption", {"tag"});
  u.provider_id_       = "volcengine_ark";
  u.model_id_          = "doubao-vision";
  u.prompt_profile_id_ = "profile-7";
  ASSERT_TRUE(ai.UpsertUnderstanding(u));
  const auto gu = ai.GetUnderstanding(file_id, "describe");
  ASSERT_TRUE(gu.has_value());
  EXPECT_EQ(gu->provider_id_, "volcengine_ark");
  EXPECT_EQ(gu->model_id_, "doubao-vision");
  EXPECT_EQ(gu->prompt_profile_id_, "profile-7");

  auto r            = MakeRating(file_id, "rate", 3, "ok");
  r.rubric_id_      = "curated-rubric";
  r.rubric_version_ = "v2";
  ASSERT_TRUE(ai.UpsertRating(r));
  const auto gr = ai.GetRating(file_id, "rate");
  ASSERT_TRUE(gr.has_value());
  EXPECT_EQ(gr->rubric_id_, "curated-rubric");
  EXPECT_EQ(gr->rubric_version_, "v2");
}

// The controller's DeleteForFiles drops both understanding and rating rows for the file.
TEST_F(AiStoreTest, DeleteForFilesRemovesBothKinds) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_delete_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();
  ASSERT_TRUE(ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe", "caption", {})));
  ASSERT_TRUE(ai.UpsertRating(MakeRating(file_id, "rate", 3, "ok")));
  ASSERT_TRUE(ai.GetUnderstanding(file_id, "describe").has_value());
  ASSERT_TRUE(ai.GetRating(file_id, "rate").has_value());

  ai.DeleteForFiles(std::span<const sl_element_id_t>(&file_id, 1));
  EXPECT_FALSE(ai.GetUnderstanding(file_id, "describe").has_value());
  EXPECT_FALSE(ai.GetActiveUnderstanding(file_id).has_value());
  EXPECT_FALSE(ai.GetRating(file_id, "rate").has_value());
  EXPECT_FALSE(ai.GetActiveRating(file_id).has_value());
}

// file_id is a foreign key into Element(id). An upsert for a file_id with no
// matching Element row is refused so no orphan AI annotation can ever be
// written (the FK is enforced at the write boundary, not via a DDL constraint).
TEST_F(AiStoreTest, OrphanFileIdNotPersisted) {
  ProjectService            project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  auto&                     ai            = project.GetStorage()->GetAiStore();
  // A file_id that was never created — no Element row exists for it.
  constexpr sl_element_id_t kOrphanFileId = 999'999'999;

  EXPECT_FALSE(ai.UpsertUnderstanding(
      MakeUnderstanding(kOrphanFileId, "describe", "orphan caption", {"orphan"})));
  EXPECT_FALSE(ai.UpsertRating(MakeRating(kOrphanFileId, "rate", 3, "orphan reasons")));

  EXPECT_FALSE(ai.GetUnderstanding(kOrphanFileId, "describe").has_value());
  EXPECT_FALSE(ai.GetRating(kOrphanFileId, "rate").has_value());
  EXPECT_EQ(CountUnderstandingRows(project.GetStorage()->GetDatabase()), 0u);
}

// The file-deletion cascade (ElementStore::RemoveElements) drops AI annotation rows
// on the same connection as element deletion, so the cleanup is atomic and a re-import
// cannot resurrect an old AI annotation under a new image id.
TEST_F(AiStoreTest, ElementDeletionCascadesAiRows) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_cascade_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();
  ASSERT_TRUE(
      ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe", "cascade caption", {})));
  ASSERT_TRUE(ai.UpsertRating(MakeRating(file_id, "rate", 2, "cascade reasons")));
  ASSERT_TRUE(ai.GetUnderstanding(file_id, "describe").has_value());

  auto&      el_ctrl = project.GetStorage()->GetElementStore();
  const auto element = el_ctrl.GetElementById(file_id);
  ASSERT_NE(element, nullptr);
  const std::vector<std::shared_ptr<SleeveElement>> elements = {element};
  el_ctrl.RemoveElements(elements);

  EXPECT_FALSE(ai.GetUnderstanding(file_id, "describe").has_value());
  EXPECT_FALSE(ai.GetRating(file_id, "rate").has_value());
}

// The public façade delete-by-id API — SleeveServiceImpl::DeleteElement(id) —
// must cascade AI rows. DeleteElement flows through Write<void>, which marks the
// element DELETED in the in-memory FS tree and then calls Sync(); Sync collects
// the deleted element and runs ElementStore::RemoveElements, which calls
// DeleteSemanticAndAiRowsForFiles on the same connection. The cascade lives at
// the service layer, NOT in the low-level ElementStore::RemoveElement(id)
// row-delete primitive (which intentionally removes only the Element row).
TEST_F(AiStoreTest, SleeveServiceDeleteElementCascadesAiRows) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"ai_cascade_id_alpha.dng");
  ASSERT_NE(file_id, 0u);
  auto& ai = project.GetStorage()->GetAiStore();
  ASSERT_TRUE(
      ai.UpsertUnderstanding(MakeUnderstanding(file_id, "describe", "cascade id caption", {})));
  ASSERT_TRUE(ai.UpsertRating(MakeRating(file_id, "rate", 4, "cascade id reasons")));
  ASSERT_TRUE(ai.GetUnderstanding(file_id, "describe").has_value());
  ASSERT_TRUE(ai.GetRating(file_id, "rate").has_value());

  // Façade API: marks DELETED + Syncs (Write<void> auto-calls Sync), flushing
  // the element + its AI rows out of the DB.
  const auto result = project.GetSleeveService()->DeleteElement(file_id);
  EXPECT_TRUE(result.success_) << result.message_;

  EXPECT_FALSE(ai.GetUnderstanding(file_id, "describe").has_value());
  EXPECT_FALSE(ai.GetActiveUnderstanding(file_id).has_value());
  EXPECT_FALSE(ai.GetRating(file_id, "rate").has_value());
  EXPECT_FALSE(ai.GetActiveRating(file_id).has_value());
}

}  // namespace alcedo
