//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/import_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "edit/operators/operator_registeration.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"
#include "storage/store/ai/ai_store.hpp"
#include "storage/store/semantic/semantic_store.hpp"
#include "type/supported_file_type.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/string/convert.hpp"

#include "ai/ai_description.hpp"
#include "ai/ai_rating.hpp"

namespace alcedo {
namespace {
auto U8(const char8_t* text) -> std::string {
  const auto* bytes = reinterpret_cast<const char*>(text);
  return std::string(bytes);
}

auto OneHot(size_t index) -> std::vector<float> {
  std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
  embedding.at(index) = 1.0F;
  return embedding;
}

void RegisterSemanticSearchModel(SemanticStore& semantic, const std::string& model_key,
                                 bool active = true) {
  std::string error;
  ASSERT_TRUE(semantic.UpsertModel(SemanticModelRecord{.model_key_     = model_key,
                                                       .model_id_      = "mobileclip-test",
                                                       .revision_      = "test-rev",
                                                       .embedding_dim_ = kSemanticEmbeddingDim,
                                                       .image_size_    = 256,
                                                       .active_        = active},
                                   &error))
      << error;
}

auto FindImageId(ProjectService& project, sl_element_id_t file_id) -> image_id_t {
  const auto rows = project.GetStorage()->GetElementStore().ListFilesInFolder(0);
  const auto it   = std::find_if(rows.begin(), rows.end(), [file_id](const FileListEntry& row) {
    return row.file_id_ == file_id;
  });
  EXPECT_NE(it, rows.end());
  return it != rows.end() ? it->image_id_ : 0;
}

void StoreSemanticLabel(ProjectService& project, const std::string& model_key,
                        sl_element_id_t file_id, const std::string& label, size_t embedding_index) {
  auto&       semantic = project.GetStorage()->GetSemanticStore();
  const auto  image_id = FindImageId(project, file_id);
  std::string error;
  ASSERT_NE(image_id, 0u);
  SemanticImageLabelRecord record{.file_id_         = file_id,
                                  .model_key_       = model_key,
                                  .label_           = label,
                                  .score_           = 0.91,
                                  .second_label_    = "other",
                                  .second_score_    = 0.12,
                                  .margin_          = 0.79,
                                  .confident_       = true,
                                  .top_scores_json_ = R"([{"label":"test","score":0.91}])"};
  ASSERT_TRUE(semantic.UpsertImageEmbeddingWithLabel(
      SemanticImageEmbeddingRecord{.file_id_   = file_id,
                                   .image_id_  = image_id,
                                   .model_key_ = model_key,
                                   .embedding_ = OneHot(embedding_index)},
      &record, &error))
      << error;
}

// Persist a remote image-understanding result (Phase 5f) for a file via the
// AiStore, so a search test can exercise the AI caption/tags/scene
// contribution to the search document.
void StoreAiUnderstanding(ProjectService& project, sl_element_id_t file_id,
                          const std::string& task_id, const std::string& caption,
                          const std::vector<std::string>& tags, const std::string& scene) {
  auto&          ai = project.GetStorage()->GetAiStore();
  AiDescription d;
  d.file_id_           = file_id;
  d.task_id_           = task_id;
  d.provider_id_       = "openrouter";
  d.model_id_          = "test-model";
  d.prompt_profile_id_ = "profile-v1";
  d.rendition_kind_    = "thumbnail_k1024";
  d.caption_           = caption;
  d.scene_             = scene;
  d.confidence_        = 0.8;
  d.active_            = true;
  d.SetTags(tags);
  ASSERT_TRUE(ai.UpsertUnderstanding(d));
}

// Persist a remote image-rating result (Phase 5f). Rating is intentionally NOT part of
// full-text search; these tests use it to prove that exclusion.
void StoreAiRating(ProjectService& project, sl_element_id_t file_id, const std::string& task_id,
                   int rating, const std::string& reasons) {
  auto&       ai = project.GetStorage()->GetAiStore();
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
  ASSERT_TRUE(ai.UpsertRating(r));
}

// Phase 7a: persist a rating's *reasons* only (rating = 0 sentinel) through the new
// UpsertRatingReasons path — the path AlbumImageAnalysisSink uses. The reasons must still
// be excluded from full-text search.
void StoreAiRatingReasons(ProjectService& project, sl_element_id_t file_id,
                          const std::string& task_id, const std::string& reasons) {
  auto&       ai = project.GetStorage()->GetAiStore();
  AiRating r;
  r.file_id_           = file_id;
  r.task_id_           = task_id;
  r.provider_id_       = "openrouter";
  r.model_id_          = "test-model";
  r.prompt_profile_id_ = "profile-v1";
  r.rendition_kind_    = "thumbnail_k1024";
  r.rating_            = 0;  // 7a sentinel — the real star is the EXIF Rating column
  r.rubric_id_         = "default-rubric";
  r.rubric_version_    = "v1";
  r.reasons_           = reasons;
  r.active_            = true;
  ASSERT_TRUE(ai.UpsertRatingReasons(r));
}

struct SyntheticFileSpec {
  file_name_t           file_name_{};
  std::filesystem::path image_path_{};
  std::string           make_{};
  std::string           camera_model_{};
  std::string           lens_{};
  std::string           lens_make_{};
  std::string           date_time_ = "2025-01-02 03:04:05";
  int                   rating_    = 0;
  uint64_t              iso_       = 200;
  float                 aperture_  = 5.6f;
  float                 focal_     = 50.0f;
};
}  // namespace

class FilterServiceTests : public ::testing::Test {
 protected:
  std::filesystem::path db_path_;
  std::filesystem::path meta_path_;

  void                  SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    RegisterAllOperators();

    db_path_ = std::filesystem::temp_directory_path() / "filter_service_test.db";
    meta_path_ = std::filesystem::temp_directory_path() / "filter_service_test.json";

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

  static auto BatchFixturesAvailable() -> bool {
    const auto batch_dir = std::filesystem::path(std::string(TEST_IMG_PATH)) / "raw" / "batch";
    if (!std::filesystem::exists(batch_dir)) {
      return false;
    }
    for (const auto& img : std::filesystem::directory_iterator(batch_dir)) {
      if (!img.is_directory() && is_supported_file(img.path())) {
        return true;
      }
    }
    return false;
  }

  static auto LoadBatchToRoot(ProjectService& project) -> uint32_t {
    auto                           fs_service       = project.GetSleeveService();
    auto                           img_pool_service = project.GetImagePoolService();

    std::unique_ptr<ImportService> import_service =
        std::make_unique<ImportServiceImpl>(fs_service, img_pool_service);

    const image_path_t        batch_dir = std::string(TEST_IMG_PATH) + "/raw/batch";

    std::vector<image_path_t> paths;
    for (const auto& img : std::filesystem::directory_iterator(batch_dir)) {
      if (!img.is_directory() && is_supported_file(img.path())) {
        paths.push_back(img.path().string());
      }
    }

    std::shared_ptr<ImportJob> import_job = std::make_shared<ImportJob>();

    std::promise<ImportResult> final_result;
    auto                       final_result_future = final_result.get_future();

    import_job->on_finished_                       = [&final_result](const ImportResult& result) {
      final_result.set_value(result);
    };

    import_job = import_service->ImportToFolder(paths, L"", {}, import_job);
    EXPECT_NE(import_job, nullptr);

    final_result_future.wait();
    ImportResult result = final_result_future.get();

    EXPECT_EQ(result.requested_, static_cast<uint32_t>(paths.size()));
    EXPECT_EQ(result.failed_, 0u);

    EXPECT_NE(import_job->import_log_, nullptr);
    auto snapshot = import_job->import_log_->Snapshot();
    import_service->SyncImports(snapshot, L"");

    return result.imported_;
  }

  static auto CreateSyntheticFile(ProjectService& project, const file_name_t& file_name,
                                  const std::string& camera_model) -> sl_element_id_t {
    return CreateSyntheticFile(project,
                               SyntheticFileSpec{.file_name_    = file_name,
                                                 .image_path_   = std::filesystem::path{file_name},
                                                 .camera_model_ = camera_model,
                                                 .lens_         = "Synthetic 50mm"});
  }

  static auto CreateSyntheticFile(ProjectService& project, const SyntheticFileSpec& spec)
      -> sl_element_id_t {
    auto image_pool          = project.GetImagePoolService();
    auto image               = image_pool->CreateAndReturnPinnedEmpty();
    auto image_id            = image.Get()->image_id_;
    image.Get()->image_name_ = spec.file_name_;
    image.Get()->image_path_ =
        spec.image_path_.empty() ? std::filesystem::path{spec.file_name_} : spec.image_path_;
    image.Get()->image_type_ = ImageType::DNG;

    ExifDisplayMetaData metadata;
    metadata.make_          = spec.make_;
    metadata.model_         = spec.camera_model_;
    metadata.lens_          = spec.lens_;
    metadata.lens_make_     = spec.lens_make_;
    metadata.date_time_str_ = spec.date_time_;
    metadata.aperture_      = spec.aperture_;
    metadata.iso_           = spec.iso_;
    metadata.focal_         = spec.focal_;
    metadata.rating_        = spec.rating_;
    image.Get()->SetExifDisplayMetaData(std::move(metadata));
    image_pool->SyncWithStorage();

    auto sleeve = project.GetSleeveService();
    auto file   = sleeve->Write<std::shared_ptr<SleeveFile>>(
        [&spec, image_id](FileSystem& fs) -> std::shared_ptr<SleeveFile> {
          auto created       = fs.CreateFileInLibrary(spec.file_name_);
          created->image_id_ = image_id;
          return created;
        });
    EXPECT_TRUE(file.second.success_);
    EXPECT_NE(file.first, nullptr);
    return file.first ? file.first->element_id_ : 0;
  }
};

TEST_F(FilterServiceTests, ASTCreationTest) {
  try {
    FieldCondition cond{
        .field_ = FilterField::ExifCameraModel,
        .op_    = CompareOp::EQUALS,
        .value_ = std::wstring(L"Canon EOS 5D Mark IV"),
    };
    FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};
    (void)root;
  } catch (const std::exception& e) {
    FAIL() << "Exception during AST creation: " << e.what();
  }
}

TEST_F(FilterServiceTests, SQLCompilationTest) {
  FieldCondition cond{
      .field_ = FilterField::ExifCameraModel,
      .op_    = CompareOp::EQUALS,
      .value_ = std::wstring(L"Canon EOS 5D Mark IV"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "(json_extract(i.metadata, '$.Model') = ?)");
  ASSERT_EQ(sql.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(sql.binds_[0]), "Canon EOS 5D Mark IV");
}

TEST_F(FilterServiceTests, ComplexFilterSQLTest) {
  FieldCondition cond1{
      .field_ = FilterField::ExifCameraModel,
      .op_    = CompareOp::EQUALS,
      .value_ = std::wstring(L"Nikon D850"),
  };
  FilterNode node1{FilterNode::Type::Condition, {}, {}, std::move(cond1), std::nullopt};

  FieldCondition cond2{
      .field_ = FilterField::FileExtension,
      .op_    = CompareOp::ENDS_WITH,
      .value_ = std::wstring(L".NEF"),
  };
  FilterNode node2{FilterNode::Type::Condition, {}, {}, std::move(cond2), std::nullopt};

  FilterNode root{FilterNode::Type::Logical, FilterOp::AND, {node1, node2}, {}, std::nullopt};

  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_,
            "((json_extract(i.metadata, '$.Model') = ?) AND "
            "(UPPER(i.file_name) LIKE ?))");
  ASSERT_EQ(sql.binds_.size(), 2u);
  EXPECT_EQ(std::get<std::string>(sql.binds_[0]), "Nikon D850");
  EXPECT_EQ(std::get<std::string>(sql.binds_[1]), "%.NEF");
}

TEST_F(FilterServiceTests, BetweenConditionSQLTest) {
  FieldCondition cond{
      .field_        = FilterField::ExifISO,
      .op_           = CompareOp::BETWEEN,
      .value_        = int64_t(100),
      .second_value_ = int64_t(800),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "(json_extract(i.metadata, '$.ISO')::INT BETWEEN ? AND ?)");
  ASSERT_EQ(sql.binds_.size(), 2u);
  EXPECT_EQ(std::get<int64_t>(sql.binds_[0]), 100);
  EXPECT_EQ(std::get<int64_t>(sql.binds_[1]), 800);
}

TEST_F(FilterServiceTests, BindsStringFilterValueWithEmbeddedQuote) {
  FieldCondition cond{
      .field_ = FilterField::ExifCameraModel,
      .op_    = CompareOp::EQUALS,
      .value_ = std::wstring(L"O'Brien"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "(json_extract(i.metadata, '$.Model') = ?)");
  ASSERT_EQ(sql.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(sql.binds_[0]), "O'Brien");
}

TEST_F(FilterServiceTests, RawSQLBridgeNodeCompilesAsTrustedText) {
  FilterNode root{FilterNode::Type::RawSQL, {}, {}, std::nullopt, std::wstring(L"e.id > 0")};
  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "e.id > 0");
  EXPECT_TRUE(sql.binds_.empty());
}

TEST_F(FilterServiceTests, FolderIndexTest_Model) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond{
           .field_ = FilterField::ExifCameraModel,
           .op_    = CompareOp::CONTAINS,
           .value_ = std::wstring(L"D850"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto filter_id  = filter_service.CreateFilterCombo(root);
  auto       result_opt = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_opt.has_value());

  EXPECT_EQ(result_opt->size(), 5u);
}

TEST_F(FilterServiceTests, AlbumScopeFilterUsesMembershipOnly) {
  ProjectService project(db_path_, meta_path_);
  const auto     d850_file_id  = CreateSyntheticFile(project, L"d850.dng", "Nikon D850");
  const auto     other_file_id = CreateSyntheticFile(project, L"other.dng", "Sony A7");
  ASSERT_NE(d850_file_id, 0u);
  ASSERT_NE(other_file_id, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond{
           .field_ = FilterField::ExifCameraModel,
           .op_    = CompareOp::CONTAINS,
           .value_ = std::wstring(L"D850"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto filter_id   = filter_service.CreateFilterCombo(root);
  auto       root_result = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(root_result.has_value());
  ASSERT_EQ(root_result->size(), 1u);
  ASSERT_EQ(root_result->front(), d850_file_id);

  auto created_album = sleeve_service->CreateFolder(L"/", L"AlbumScope");
  ASSERT_TRUE(created_album.second.success_);
  ASSERT_NE(created_album.first, nullptr);
  const auto album_id = created_album.first->element_id_;

  ASSERT_TRUE(sleeve_service->LinkFileToFolder(d850_file_id, album_id).success_);

  auto album_result = filter_service.ApplyFilterOn(filter_id, album_id);
  ASSERT_TRUE(album_result.has_value());

  ASSERT_EQ(album_result->size(), 1u);
  EXPECT_EQ(album_result->front(), d850_file_id);

  const auto other_album = sleeve_service->CreateFolder(L"/", L"OtherAlbum");
  ASSERT_TRUE(other_album.second.success_);
  ASSERT_TRUE(
      sleeve_service->LinkFileToFolder(other_file_id, other_album.first->element_id_).success_);
  auto other_album_result = filter_service.ApplyFilterOn(filter_id, other_album.first->element_id_);
  ASSERT_TRUE(other_album_result.has_value());
  EXPECT_TRUE(other_album_result->empty());
}

TEST_F(FilterServiceTests, FuzzySearchMatchesMetadataFilenamePathAndDatesWithWideInput) {
  ProjectService project(db_path_, meta_path_);
  const auto     huangshan_id = CreateSyntheticFile(
      project, SyntheticFileSpec{
                       .file_name_    = L"\u9EC4\u5C71\u65E5\u51FA_20260529.RAF",
                       .image_path_   = std::filesystem::path{L"D:/\u7167\u7247\u5E93/\u5B89\u5FBD/"
                                                        L"\u9EC4\u5C71\u65E5\u51FA_20260529.RAF"},
                       .make_         = U8(u8"\u5BCC\u58EB"),
                       .camera_model_ = U8(u8"\u5BCC\u58EB X-T5"),
                       .lens_         = U8(u8"\u9F99\u955C 23mm"),
                       .lens_make_    = U8(u8"\u4E2D\u56FD\u955C\u5934\u5382"),
                       .date_time_    = "2026-05-29 08:09:10",
                       .rating_       = 4,
                       .iso_          = 400,
                       .aperture_     = 2.8f,
                       .focal_        = 23.0f});
  const auto city_id = CreateSyntheticFile(
      project,
      SyntheticFileSpec{
          .file_name_    = L"city_walk.dng",
          .image_path_   = std::filesystem::path{L"D:/photos/city/\u591C\u666F/city_walk.dng"},
          .make_         = "Nikon",
          .camera_model_ = "Nikon Z8",
          .lens_         = "NIKKOR Z 35mm",
          .date_time_    = "2025-12-01 20:00:00",
          .rating_       = 2,
          .iso_          = 800,
          .aperture_     = 1.8f,
          .focal_        = 35.0f});
  ASSERT_NE(huangshan_id, 0u);
  ASSERT_NE(city_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());

  const auto          expect_only = [&](const std::wstring& query, sl_element_id_t expected_id) {
    const auto rows = filter_service.SearchFolder(0, query, 0, 10);
    ASSERT_EQ(rows.size(), 1u) << conv::ToBytes(query);
    EXPECT_EQ(rows.front().file_id_, expected_id);
    EXPECT_EQ(filter_service.CountSearchResults(0, query), 1u);
  };

  expect_only(L"\u5BCC\u58EB", huangshan_id);
  expect_only(L"\u9EC4\u5C71", huangshan_id);
  expect_only(L"\u5B89\u5FBD", huangshan_id);
  expect_only(L"\u9F99\u955C", huangshan_id);
  expect_only(L"\u5BCC\u58EB \u9EC4\u5C71", huangshan_id);
  expect_only(L"2026-05-29", huangshan_id);
  expect_only(L"20260529", huangshan_id);
  expect_only(L"2026/05", huangshan_id);
  expect_only(L"2026\u5E745\u670829\u65E5", huangshan_id);
  expect_only(L"2026", huangshan_id);
  expect_only(L"\u591C\u666F", city_id);
  expect_only(L"Nikon 2025", city_id);
}

TEST_F(FilterServiceTests, FuzzySearchMatchesSeparatorFoldedPhotoTerms) {
  ProjectService project(db_path_, meta_path_);
  const auto     target_id = CreateSyntheticFile(
      project,
      SyntheticFileSpec{.file_name_ = L"DSC_01523-X-T5.RAF",
                            .image_path_ = std::filesystem::path{L"D:/archive/fuji/DSC_01523-X-T5.RAF"},
                            .make_         = "FUJIFILM",
                            .camera_model_ = "FUJIFILM X-T5",
                            .lens_         = "XF 23mm F/1.4 R LM WR",
                            .lens_make_    = "FUJIFILM",
                            .date_time_    = "2026-05-29 11:22:33",
                            .rating_       = 5,
                            .iso_          = 125,
                            .aperture_     = 1.4f,
                            .focal_        = 23.0f});
  const auto decoy_id = CreateSyntheticFile(
      project,
      SyntheticFileSpec{.file_name_  = L"DSC_01524-X-T5.RAF",
                        .image_path_ = std::filesystem::path{L"D:/archive/fuji/DSC_01524-X-T5.RAF"},
                        .make_       = "FUJIFILM",
                        .camera_model_ = "FUJIFILM X-T5",
                        .lens_         = "XF 35mm F/2 R WR",
                        .lens_make_    = "FUJIFILM",
                        .date_time_    = "2026-05-30 11:22:33",
                        .rating_       = 3,
                        .iso_          = 200,
                        .aperture_     = 2.0f,
                        .focal_        = 35.0f});
  ASSERT_NE(target_id, 0u);
  ASSERT_NE(decoy_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());

  const auto          expect_only_target = [&](const std::wstring& query) {
    const auto rows = filter_service.SearchFolder(0, query, 0, 10);
    ASSERT_EQ(rows.size(), 1u) << conv::ToBytes(query);
    EXPECT_EQ(rows.front().file_id_, target_id);
    EXPECT_EQ(filter_service.CountSearchResults(0, query), 1u);
  };

  expect_only_target(L"DSC_01523-X-T5");
  expect_only_target(L"xt5 23mmf14");
  expect_only_target(L"xf23mmf14rlmwr");
}

TEST_F(FilterServiceTests, FuzzySearchMatchesRealImportedRawFilenameWithoutSeparators) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          rows = filter_service.SearchFolder(0, L"_DSC2296ARW", 0, 10);

  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().file_name_, "_DSC2296.ARW");
  EXPECT_EQ(filter_service.CountSearchResults(0, L"_DSC2296ARW"), 1u);
}

TEST_F(FilterServiceTests, FuzzySearchMatchesGeneratedSemanticLabelsAsOrdinaryText) {
  ProjectService project(db_path_, meta_path_);
  const auto     landscape_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"semantic_alpha.dng",
                                     .image_path_   = std::filesystem::path{L"D:/photos/a.dng"},
                                     .camera_model_ = "Neutral Body",
                                     .lens_         = "Plain Lens"});
  const auto portrait_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"semantic_beta.dng",
                                 .image_path_   = std::filesystem::path{L"D:/photos/b.dng"},
                                 .camera_model_ = "Neutral Body",
                                 .lens_         = "Plain Lens"});
  ASSERT_NE(landscape_id, 0u);
  ASSERT_NE(portrait_id, 0u);

  auto& semantic = project.GetStorage()->GetSemanticStore();
  RegisterSemanticSearchModel(semantic, "mobileclip-test-a");
  RegisterSemanticSearchModel(semantic, "mobileclip-test-b");
  StoreSemanticLabel(project, "mobileclip-test-a", landscape_id, "landscape", 4);
  StoreSemanticLabel(project, "mobileclip-test-b", landscape_id, "landscape", 5);
  StoreSemanticLabel(project, "mobileclip-test-a", portrait_id, "portrait", 6);

  SleeveFilterService filter_service(project.GetStorage());

  const auto          landscape_rows = filter_service.SearchFolder(0, L"landscape", 0, 10);
  ASSERT_EQ(landscape_rows.size(), 1u);
  EXPECT_EQ(landscape_rows.front().file_id_, landscape_id);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"landscape"), 1u);
  const auto zh_landscape_rows = filter_service.SearchFolder(0, L"\u98CE\u666F", 0, 10);
  ASSERT_EQ(zh_landscape_rows.size(), 1u);
  EXPECT_EQ(zh_landscape_rows.front().file_id_, landscape_id);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"\u98CE\u666F"), 1u);
  EXPECT_TRUE(filter_service.SearchFolder(0, L"portrait", 0, 10).empty());
  EXPECT_EQ(filter_service.CountSearchResults(0, L"portrait"), 0u);

  const auto combined_rows = filter_service.SearchFolder(0, L"landscape Neutral", 0, 10);
  ASSERT_EQ(combined_rows.size(), 1u);
  EXPECT_EQ(combined_rows.front().file_id_, landscape_id);

  auto sleeve_service = project.GetSleeveService();
  auto album          = sleeve_service->CreateFolder(L"/", L"SemanticLabels");
  ASSERT_TRUE(album.second.success_);
  ASSERT_NE(album.first, nullptr);
  ASSERT_TRUE(sleeve_service->LinkFileToFolder(landscape_id, album.first->element_id_).success_);

  const auto album_landscape =
      filter_service.SearchFolder(album.first->element_id_, L"landscape", 0, 10);
  ASSERT_EQ(album_landscape.size(), 1u);
  EXPECT_EQ(album_landscape.front().file_id_, landscape_id);
  EXPECT_TRUE(filter_service.SearchFolder(album.first->element_id_, L"portrait", 0, 10).empty());

  const auto stats            = filter_service.BuildFolderStats(0);
  const auto has_label_bucket = std::find_if(
      stats.label_stats_.begin(), stats.label_stats_.end(),
      [](const StatsBucket& row) { return row.label_ == "landscape" && row.count_ == 1; });
  EXPECT_NE(has_label_bucket, stats.label_stats_.end());

  RegisterSemanticSearchModel(semantic, "jina-multilingual-test");
  StoreSemanticLabel(project, "jina-multilingual-test", portrait_id, "portrait", 7);
  EXPECT_TRUE(filter_service.SearchFolder(0, L"landscape", 0, 10).empty());
  const auto en_portrait_rows = filter_service.SearchFolder(0, L"portrait", 0, 10);
  ASSERT_EQ(en_portrait_rows.size(), 1u);
  EXPECT_EQ(en_portrait_rows.front().file_id_, portrait_id);
  const auto zh_portrait_rows = filter_service.SearchFolder(0, L"\u4EBA\u50CF", 0, 10);
  ASSERT_EQ(zh_portrait_rows.size(), 1u);
  EXPECT_EQ(zh_portrait_rows.front().file_id_, portrait_id);
}

TEST_F(FilterServiceTests, FuzzySearchIgnoresSemanticLabelsWhenNoModelIsActive) {
  ProjectService project(db_path_, meta_path_);
  const auto     landscape_id =
      CreateSyntheticFile(project, SyntheticFileSpec{.file_name_ = L"inactive_semantic_alpha.dng",
                                                     .camera_model_ = "Neutral Camera"});
  ASSERT_NE(landscape_id, 0u);

  auto& semantic = project.GetStorage()->GetSemanticStore();
  RegisterSemanticSearchModel(semantic, "inactive-mobileclip-test", false);
  StoreSemanticLabel(project, "inactive-mobileclip-test", landscape_id, "landscape", 4);
  ASSERT_TRUE(semantic.ActiveModelKey().empty());

  SleeveFilterService filter_service(project.GetStorage());
  EXPECT_TRUE(filter_service.SearchFolder(0, L"landscape", 0, 10).empty());
  EXPECT_EQ(filter_service.CountSearchResults(0, L"landscape"), 0u);
  EXPECT_TRUE(filter_service.BuildFolderStats(0).label_stats_.empty());
}

// Phase 5f: an active AI understanding's caption + tags + scene participate in full-text
// search, but only once an active row is actually persisted. The filename and metadata
// below deliberately do not contain the caption/tag tokens, so a match can only come from
// the AiImageUnderstanding row.
TEST_F(FilterServiceTests, AiUnderstandingCaptionAndTagsSearchableOnlyAfterActivePersistence) {
  ProjectService project(db_path_, meta_path_);
  const auto     alpha_id =
      CreateSyntheticFile(project, SyntheticFileSpec{.file_name_    = L"ai_search_alpha.dng",
                                                     .camera_model_ = "Neutral Body",
                                                     .lens_         = "Plain Lens"});
  const auto beta_id =
      CreateSyntheticFile(project, SyntheticFileSpec{.file_name_    = L"ai_search_beta.dng",
                                                     .camera_model_ = "Neutral Body",
                                                     .lens_         = "Plain Lens"});
  ASSERT_NE(alpha_id, 0u);
  ASSERT_NE(beta_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());
  // Before any AI understanding is persisted, neither caption nor tag token is searchable.
  EXPECT_TRUE(filter_service.SearchFolder(0, L"sahara", 0, 10).empty());
  EXPECT_EQ(filter_service.CountSearchResults(0, L"sahara"), 0u);
  EXPECT_TRUE(filter_service.SearchFolder(0, L"desert", 0, 10).empty());

  // Persist an active understanding for alpha with a distinctive caption + tag + scene.
  StoreAiUnderstanding(project, alpha_id, "describe", "sahara dunes at sunset", {"desert"},
                       "saharasunset");

  // The caption word is now searchable and returns only alpha (beta has no such caption).
  const auto caption_rows = filter_service.SearchFolder(0, L"sahara", 0, 10);
  ASSERT_EQ(caption_rows.size(), 1u);
  EXPECT_EQ(caption_rows.front().file_id_, alpha_id);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"sahara"), 1u);

  // The tag word is searchable too (the JSON syntax around the tag is stripped by folding).
  const auto tag_rows = filter_service.SearchFolder(0, L"desert", 0, 10);
  ASSERT_EQ(tag_rows.size(), 1u);
  EXPECT_EQ(tag_rows.front().file_id_, alpha_id);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"desert"), 1u);

  // The scene word is searchable as well.
  ASSERT_EQ(filter_service.SearchFolder(0, L"saharasunset", 0, 10).size(), 1u);
}

// The search-settings drawer field mask scopes which field groups contribute to the
// WHERE. The AiDescription bit gates the new per-field LIKE on caption+scene; the
// AiTags bit gates the per-field LIKE on tags_json (plus the local SemanticImageLabel
// clauses); the combined BM25 FTS clause only runs when BOTH AI bits are set (its
// body merges caption + tags_json + scene and cannot attribute a hit to one sub-field).
TEST_F(FilterServiceTests, FieldMaskScopesResultsByFieldGroup) {
  ProjectService project(db_path_, meta_path_);
  const auto     alpha_id =
      CreateSyntheticFile(project, SyntheticFileSpec{.file_name_    = L"mask_scope_alpha.dng",
                                                     .camera_model_ = "Neutral Body",
                                                     .lens_         = "Plain Lens"});
  ASSERT_NE(alpha_id, 0u);

  // Distinctive caption + tag + scene tokens that do NOT appear in the filename or
  // EXIF, so a match can only come from the AiImageUnderstanding row.
  StoreAiUnderstanding(project, alpha_id, "describe", "sahara dunes at sunset", {"desert"},
                       "saharasunset");

  SleeveFilterService filter_service(project.GetStorage());

  // Single-bit masks used below; SearchField is an enum class so the bits must
  // be cast to SearchFieldMask before they can be passed as the mask argument.
  constexpr SearchFieldMask kDescOnly =
      static_cast<SearchFieldMask>(SearchField::AiDescription);
  constexpr SearchFieldMask kTagsOnly = static_cast<SearchFieldMask>(SearchField::AiTags);
  constexpr SearchFieldMask kFileOnly = static_cast<SearchFieldMask>(SearchField::Filename);
  constexpr SearchFieldMask kNoFields  = static_cast<SearchFieldMask>(0);

  // Default (all fields): caption, tag, and filename tokens all match.
  EXPECT_EQ(filter_service.CountSearchResults(0, L"sahara", kAllSearchFields), 1u);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"desert", kAllSearchFields), 1u);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"mask_scope_alpha", kAllSearchFields), 1u);

  // AI-description only: caption + scene match; the tag token does NOT (it lives
  // only in tags_json, gated to the AiTags bit).
  EXPECT_EQ(filter_service.CountSearchResults(0, L"sahara", kDescOnly), 1u);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"desert", kDescOnly), 0u);

  // AI-tags only: the tag token matches; the caption token does NOT.
  EXPECT_EQ(filter_service.CountSearchResults(0, L"desert", kTagsOnly), 1u);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"sahara", kTagsOnly), 0u);

  // Filename-only: neither AI token matches (they are not in the filename); the
  // filename token matches under the Filename bit and not under an AI-only mask.
  EXPECT_EQ(filter_service.CountSearchResults(0, L"sahara", kFileOnly), 0u);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"desert", kFileOnly), 0u);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"mask_scope_alpha", kFileOnly), 1u);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"mask_scope_alpha", kDescOnly), 0u);

  // No field selected → match nothing (the WHERE becomes the literal FALSE).
  EXPECT_EQ(filter_service.CountSearchResults(0, L"sahara", kNoFields), 0u);
  EXPECT_EQ(filter_service.CountSearchResults(0, L"mask_scope_alpha", kNoFields), 0u);
}

// Phase 5f: the remote LLM rating is NOT part of full-text search. A distinctive word that
// appears only in the rating's reasons must not be searchable, even though the rating row
// is persisted and the word exists in the database.
TEST_F(FilterServiceTests, AiRatingReasonsAreNotInFullScreenSearch) {
  ProjectService project(db_path_, meta_path_);
  const auto     alpha_id =
      CreateSyntheticFile(project, SyntheticFileSpec{.file_name_    = L"ai_rating_search_alpha.dng",
                                                     .camera_model_ = "Neutral Body",
                                                     .lens_         = "Plain Lens"});
  ASSERT_NE(alpha_id, 0u);

  StoreAiUnderstanding(project, alpha_id, "describe", "sahara caption", {"desert"}, "");
  StoreAiRating(project, alpha_id, "rate", 5, "flibbertigibbet golden ratio");

  SleeveFilterService filter_service(project.GetStorage());
  // The understanding caption is searchable.
  const auto caption_rows = filter_service.SearchFolder(0, L"sahara", 0, 10);
  ASSERT_EQ(caption_rows.size(), 1u);
  EXPECT_EQ(caption_rows.front().file_id_, alpha_id);

  // The rating's distinctive reasons word must NOT be searchable.
  EXPECT_TRUE(filter_service.SearchFolder(0, L"flibbertigibbet", 0, 10).empty());
  EXPECT_EQ(filter_service.CountSearchResults(0, L"flibbertigibbet"), 0u);

  // Sanity: the rating was actually persisted, so the empty search result is meaningful —
  // the word exists in the DB, it is simply excluded from the search document.
  const auto rating =
      project.GetStorage()->GetAiStore().GetRating(alpha_id, "rate");
  ASSERT_TRUE(rating.has_value());
  EXPECT_EQ(rating->rating_, 5);
  EXPECT_NE(rating->reasons_.find("flibbertigibbet"), std::string::npos);
}

// Phase 7a: the reasons-only path (UpsertRatingReasons, rating = 0 sentinel — the path
// AlbumImageAnalysisSink uses) still keeps rating reasons out of full-text search. A
// distinctive word that appears only in the reasons must not be searchable.
TEST_F(FilterServiceTests, AiRatingReasonsOnlyRowIsNotInFullScreenSearch) {
  ProjectService project(db_path_, meta_path_);
  const auto     alpha_id =
      CreateSyntheticFile(project, SyntheticFileSpec{.file_name_    = L"ai_reasons_search_alpha.dng",
                                                     .camera_model_ = "Neutral Body",
                                                     .lens_         = "Plain Lens"});
  ASSERT_NE(alpha_id, 0u);

  StoreAiUnderstanding(project, alpha_id, "describe", "sahara caption", {"desert"}, "");
  StoreAiRatingReasons(project, alpha_id, "rate", "flibbertigibbet golden ratio");

  SleeveFilterService filter_service(project.GetStorage());
  // The understanding caption is searchable.
  ASSERT_EQ(filter_service.SearchFolder(0, L"sahara", 0, 10).size(), 1u);

  // The reasons-only row's distinctive word must NOT be searchable, even though the row
  // is persisted and active and the word exists in the DB.
  EXPECT_TRUE(filter_service.SearchFolder(0, L"flibbertigibbet", 0, 10).empty());
  EXPECT_EQ(filter_service.CountSearchResults(0, L"flibbertigibbet"), 0u);

  // Sanity: the reasons row was persisted as an active rating row (rating = 0 sentinel).
  const auto rating =
      project.GetStorage()->GetAiStore().GetActiveRating(alpha_id);
  ASSERT_TRUE(rating.has_value());
  EXPECT_EQ(rating->rating_, 0);
  EXPECT_NE(rating->reasons_.find("flibbertigibbet"), std::string::npos);
}

TEST_F(FilterServiceTests, FuzzySearchEscapesSqlLikeWildcardsAndQuotesInWideInput) {
  ProjectService project(db_path_, meta_path_);
  const auto     literal_id = CreateSyntheticFile(
      project, SyntheticFileSpec{
                       .file_name_    = L"100%_\u62A5\u4EF7'\u9EC4\u5C71'.dng",
                       .image_path_   = std::filesystem::path{L"D:/\u7167\u7247\u5E93/\u62A5\u4EF7/"
                                                        L"100%_\u62A5\u4EF7'\u9EC4\u5C71'.dng"},
                       .camera_model_ = U8(u8"\u62A5\u4EF7\u673A"),
                       .lens_         = U8(u8"\u62A5\u4EF7\u955C\u5934"),
                       .date_time_    = "2026-06-01 10:00:00"});
  const auto wildcard_decoy_id = CreateSyntheticFile(
      project,
      SyntheticFileSpec{.file_name_  = L"1000A\u62A5\u4EF7\u9EC4\u5C71.dng",
                        .image_path_ = std::filesystem::path{L"D:/\u7167\u7247\u5E93/\u62A5\u4EF7/"
                                                             L"1000A\u62A5\u4EF7\u9EC4\u5C71.dng"},
                        .camera_model_ = U8(u8"\u62A5\u4EF7\u673A"),
                        .lens_         = U8(u8"\u62A5\u4EF7\u955C\u5934"),
                        .date_time_    = "2026-06-02 10:00:00"});
  ASSERT_NE(literal_id, 0u);
  ASSERT_NE(wildcard_decoy_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());

  const auto          expect_only_literal = [&](const std::wstring& query) {
    const auto rows = filter_service.SearchFolder(0, query, 0, 10);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows.front().file_id_, literal_id);
  };

  expect_only_literal(L"100%_");
  expect_only_literal(L"100%_\u62A5\u4EF7");
  expect_only_literal(L"\u62A5\u4EF7'\u9EC4\u5C71");
}

TEST_F(FilterServiceTests, FolderIndexTest_FileExtension) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond{
           .field_ = FilterField::FileExtension,
           .op_    = CompareOp::ENDS_WITH,
           .value_ = std::wstring(L".NEF"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto filter_id  = filter_service.CreateFilterCombo(root);
  auto       result_opt = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_opt.has_value());

  EXPECT_EQ(result_opt->size(), 5u);
}

TEST_F(FilterServiceTests, FolderIndexTest_Aperature) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond{
           .field_ = FilterField::ExifAperture,
           .op_    = CompareOp::GREATER_THAN,
           .value_ = double(5.6),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto filter_id  = filter_service.CreateFilterCombo(root);
  auto       result_opt = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_opt.has_value());

  // raw/batch also contains JPEG siblings for the Sony ARWs. The import path keeps the RAW
  // identities only, so filter counts must match the 11 imported RAW files rather than all 17
  // files on disk.
  EXPECT_EQ(result_opt->size(), 6u);
}

TEST_F(FilterServiceTests, FolderIndexTest_ISO) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond{
           .field_        = FilterField::ExifISO,
           .op_           = CompareOp::BETWEEN,
           .value_        = int64_t(100),
           .second_value_ = int64_t(400),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto filter_id  = filter_service.CreateFilterCombo(root);
  auto       result_opt = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_opt.has_value());

  // JPEG siblings are not imported as independent library files in this fixture.
  EXPECT_EQ(result_opt->size(), 6u);
}

TEST_F(FilterServiceTests, FolderIndexTest_FocalLength) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond{
           .field_ = FilterField::ExifFocalLength,
           .op_    = CompareOp::LESS_THAN,
           .value_ = double(150.0),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto filter_id  = filter_service.CreateFilterCombo(root);
  auto       result_opt = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_opt.has_value());

  // Six Sony ARWs plus one Nikon D850 NEF are under 150mm after import.
  EXPECT_EQ(result_opt->size(), 7u);
}

TEST_F(FilterServiceTests, FolderIndexTest_Combined) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond1{
           .field_ = FilterField::ExifCameraModel,
           .op_    = CompareOp::CONTAINS,
           .value_ = std::wstring(L"D850"),
  };
  FilterNode     node1{FilterNode::Type::Condition, {}, {}, std::move(cond1), std::nullopt};

  FieldCondition cond2{
      .field_ = FilterField::ExifFocalLength,
      .op_    = CompareOp::LESS_THAN,
      .value_ = double(150.0),
  };
  FilterNode node2{FilterNode::Type::Condition, {}, {}, std::move(cond2), std::nullopt};

  FilterNode root{FilterNode::Type::Logical, FilterOp::AND, {node1, node2}, {}, std::nullopt};

  const auto filter_id  = filter_service.CreateFilterCombo(root);
  auto       result_opt = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_opt.has_value());

  EXPECT_EQ(result_opt->size(), 1u);
}

TEST_F(FilterServiceTests, FolderIndexTest_NoMatch) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond{
           .field_ = FilterField::ExifCameraModel,
           .op_    = CompareOp::CONTAINS,
           .value_ = std::wstring(L"A7"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto filter_id  = filter_service.CreateFilterCombo(root);
  auto       result_opt = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_opt.has_value());

  EXPECT_EQ(result_opt->size(), 0u);
}

TEST_F(FilterServiceTests, FolderIndexTest_DateRange) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());

  FieldCondition      cond{
           .field_        = FilterField::CaptureDate,
           .op_           = CompareOp::BETWEEN,
           .value_        = std::tm{0, 0, 0, 1, 0, 125, 0, 0, -1},    // Jan 1, 2025
           .second_value_ = std::tm{0, 0, 0, 31, 11, 125, 0, 0, -1},  // Dec 31, 2025
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto filter_id  = filter_service.CreateFilterCombo(root);
  auto       result_opt = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_opt.has_value());

  // Six Sony ARWs plus one Nikon D850 NEF fall in the 2025 capture-date range.
  EXPECT_EQ(result_opt->size(), 7u);
}

TEST_F(FilterServiceTests, ListFilesInFolderByIdMatchesPathBasedList) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto browse         = project.GetAlbumBrowseService();
  ASSERT_NE(browse, nullptr);

  auto root_folder = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  const auto path_based = browse->ListFilesInFolder(std::filesystem::path(L"/"));
  const auto id_based   = browse->ListFilesInFolderById(root_folder->element_id_);
  ASSERT_EQ(path_based.size(), id_based.size());

  std::unordered_set<sl_element_id_t> path_ids;
  for (const auto& f : path_based) {
    path_ids.insert(f.file_id_);
  }
  for (const auto& f : id_based) {
    EXPECT_TRUE(path_ids.contains(f.file_id_))
        << "DB-first result file_id " << f.file_id_ << " not in path-based list";
  }
}

TEST_F(FilterServiceTests, ListCountMatchesStatsCount) {
  if (!BatchFixturesAvailable()) {
    GTEST_SKIP() << "No filter RAW fixtures available in raw/batch/";
  }
  ProjectService project(db_path_, meta_path_);
  const uint32_t imported = LoadBatchToRoot(project);
  ASSERT_GT(imported, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          stats = filter_service.BuildFolderStats(root_folder->element_id_);
  const auto          list  = project.GetStorage()->GetElementStore().ListFilesInFolder(
      root_folder->element_id_);
  EXPECT_EQ(static_cast<size_t>(stats.total_photo_count_), list.size());
}

TEST_F(FilterServiceTests, RootScopeUsesVirtualFileView) {
  ProjectService project(db_path_, meta_path_);
  const auto     file_id = CreateSyntheticFile(project, L"virtual_root.dng", "Nikon D850");
  ASSERT_NE(file_id, 0u);

  auto storage = project.GetStorage();
  storage->GetElementStore().RemoveFolderContent(0, file_id);

  const auto list = storage->GetElementStore().ListFilesInFolder(0);
  ASSERT_EQ(list.size(), 1u);
  EXPECT_EQ(list.front().file_id_, file_id);

  SleeveFilterService filter_service(storage);
  const auto          stats = filter_service.BuildFolderStats(0);
  EXPECT_EQ(stats.total_photo_count_, 1);
}

TEST_F(FilterServiceTests, PagedScopeListUsesStableOrderAndCount) {
  ProjectService project(db_path_, meta_path_);
  const auto     first_id  = CreateSyntheticFile(project, L"page_1.dng", "Nikon D850");
  const auto     second_id = CreateSyntheticFile(project, L"page_2.dng", "Nikon D850");
  const auto     third_id  = CreateSyntheticFile(project, L"page_3.dng", "Nikon D850");
  ASSERT_NE(first_id, 0u);
  ASSERT_NE(second_id, 0u);
  ASSERT_NE(third_id, 0u);

  auto browse = project.GetAlbumBrowseService();
  ASSERT_NE(browse, nullptr);
  EXPECT_EQ(browse->CountFilesInFolderById(0), 3u);

  const auto first_page = browse->ListFilesInFolderById(0, 0, 2);
  const auto next_page  = browse->ListFilesInFolderById(0, 2, 2);

  ASSERT_EQ(first_page.size(), 2u);
  ASSERT_EQ(next_page.size(), 1u);
  EXPECT_EQ(first_page[0].file_id_, first_id);
  EXPECT_EQ(first_page[1].file_id_, second_id);
  EXPECT_EQ(next_page[0].file_id_, third_id);
}

TEST_F(FilterServiceTests, FilterCacheInvalidationAfterLink) {
  ProjectService project(db_path_, meta_path_);
  const auto     file_id = CreateSyntheticFile(project, L"cache_test.dng", "Nikon D850");
  ASSERT_NE(file_id, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto album          = sleeve_service->CreateFolder(L"/", L"CacheTestAlbum");
  ASSERT_TRUE(album.second.success_);
  ASSERT_NE(album.first, nullptr);
  const auto          album_id = album.first->element_id_;

  SleeveFilterService filter_service(project.GetStorage());
  FieldCondition      cond{
           .field_ = FilterField::ExifCameraModel,
           .op_    = CompareOp::CONTAINS,
           .value_ = std::wstring(L"D850"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};
  const auto filter_id     = filter_service.CreateFilterCombo(root);

  // Apply filter on the empty album - should return empty.
  auto       result_before = filter_service.ApplyFilterOn(filter_id, album_id);
  ASSERT_TRUE(result_before.has_value());
  EXPECT_TRUE(result_before->empty());

  // Link file to album, invalidate, then re-apply - should now find the file.
  ASSERT_TRUE(sleeve_service->LinkFileToFolder(file_id, album_id).success_);
  filter_service.InvalidateResultCache(album_id);

  auto result_after = filter_service.ApplyFilterOn(filter_id, album_id);
  ASSERT_TRUE(result_after.has_value());
  ASSERT_EQ(result_after->size(), 1u);
  EXPECT_EQ(result_after->front(), file_id);
}

TEST_F(FilterServiceTests, FilterCacheInvalidationAfterUnlink) {
  ProjectService project(db_path_, meta_path_);
  const auto     file_id = CreateSyntheticFile(project, L"unlink_test.dng", "Nikon D850");
  ASSERT_NE(file_id, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto album          = sleeve_service->CreateFolder(L"/", L"UnlinkTestAlbum");
  ASSERT_TRUE(album.second.success_);
  ASSERT_NE(album.first, nullptr);
  const auto album_id = album.first->element_id_;

  ASSERT_TRUE(sleeve_service->LinkFileToFolder(file_id, album_id).success_);

  SleeveFilterService filter_service(project.GetStorage());
  FieldCondition      cond{
           .field_ = FilterField::ExifCameraModel,
           .op_    = CompareOp::CONTAINS,
           .value_ = std::wstring(L"D850"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};
  const auto filter_id     = filter_service.CreateFilterCombo(root);

  // Apply filter on the album - should find the file.
  auto       result_before = filter_service.ApplyFilterOn(filter_id, album_id);
  ASSERT_TRUE(result_before.has_value());
  ASSERT_EQ(result_before->size(), 1u);

  // Unlink file from album, invalidate, then re-apply - should be empty.
  ASSERT_TRUE(sleeve_service->DeleteFileFromFolder(file_id, album_id).success_);
  filter_service.InvalidateResultCache(album_id);

  auto result_after = filter_service.ApplyFilterOn(filter_id, album_id);
  ASSERT_TRUE(result_after.has_value());
  EXPECT_TRUE(result_after->empty());
}

TEST_F(FilterServiceTests, FilterCacheInvalidationAfterDeleteEverywhere) {
  ProjectService project(db_path_, meta_path_);
  const auto     file_id = CreateSyntheticFile(project, L"del_test.dng", "Nikon D850");
  ASSERT_NE(file_id, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto root_folder    = sleeve_service->Read<std::shared_ptr<SleeveElement>>(
      [](FileSystem& fs) { return fs.Get(L"/", false); });
  ASSERT_NE(root_folder, nullptr);

  SleeveFilterService filter_service(project.GetStorage());
  FieldCondition      cond{
           .field_ = FilterField::ExifCameraModel,
           .op_    = CompareOp::CONTAINS,
           .value_ = std::wstring(L"D850"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};
  const auto filter_id     = filter_service.CreateFilterCombo(root);

  // Apply filter on Root - should find the file.
  auto       result_before = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_before.has_value());
  ASSERT_EQ(result_before->size(), 1u);

  // Delete everywhere, invalidate entire cache, re-apply - should be empty.
  ASSERT_TRUE(sleeve_service->DeleteFileEverywhere(file_id).success_);
  filter_service.InvalidateResultCache();

  auto result_after = filter_service.ApplyFilterOn(filter_id, root_folder->element_id_);
  ASSERT_TRUE(result_after.has_value());
  EXPECT_TRUE(result_after->empty());
}

TEST_F(FilterServiceTests, AlbumScopeListAndStatsAreConsistent) {
  ProjectService project(db_path_, meta_path_);
  const auto     file1_id = CreateSyntheticFile(project, L"consist1.dng", "Nikon D850");
  const auto     file2_id = CreateSyntheticFile(project, L"consist2.dng", "Sony A7");
  ASSERT_NE(file1_id, 0u);
  ASSERT_NE(file2_id, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto album          = sleeve_service->CreateFolder(L"/", L"ConsistencyAlbum");
  ASSERT_TRUE(album.second.success_);
  ASSERT_NE(album.first, nullptr);
  const auto album_id = album.first->element_id_;

  ASSERT_TRUE(sleeve_service->LinkFileToFolder(file1_id, album_id).success_);
  ASSERT_TRUE(sleeve_service->LinkFileToFolder(file2_id, album_id).success_);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          stats = filter_service.BuildFolderStats(album_id);
  const auto list = project.GetStorage()->GetElementStore().ListFilesInFolder(album_id);
  EXPECT_EQ(static_cast<size_t>(stats.total_photo_count_), list.size());
  EXPECT_EQ(list.size(), 2u);
}

TEST_F(FilterServiceTests, AutoInvalidationOnLink) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     file_id = CreateSyntheticFile(project, L"auto_inval_test.dng", "Nikon D850");
  ASSERT_NE(file_id, 0u);

  auto sleeve_service = project.GetSleeveService();
  auto album          = sleeve_service->CreateFolder(L"/", L"AutoInvalAlbum");
  ASSERT_TRUE(album.second.success_);
  ASSERT_NE(album.first, nullptr);
  const auto album_id       = album.first->element_id_;

  auto       filter_service = project.GetSleeveFilterService();
  ASSERT_NE(filter_service, nullptr);
  auto browse_service = project.GetAlbumBrowseService();
  ASSERT_NE(browse_service, nullptr);

  FieldCondition cond{
      .field_ = FilterField::ExifCameraModel,
      .op_    = CompareOp::CONTAINS,
      .value_ = std::wstring(L"D850"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};
  const auto filter_id     = filter_service->CreateFilterCombo(root);

  // Apply filter on the empty album - should return empty.
  auto       result_before = filter_service->ApplyFilterOn(filter_id, album_id);
  ASSERT_TRUE(result_before.has_value());
  EXPECT_TRUE(result_before->empty());

  // Link via AlbumBrowseService - this MUST auto-invalidate the filter cache.
  const auto link_result = browse_service->LinkFilesToFolder({file_id}, album_id);
  EXPECT_EQ(link_result.deleted_files_.size(), 1u);

  // Re-apply filter WITHOUT manual InvalidateResultCache.
  // The AlbumBrowseService should have auto-invalidated the cache.
  auto result_after = filter_service->ApplyFilterOn(filter_id, album_id);
  ASSERT_TRUE(result_after.has_value());
  ASSERT_EQ(result_after->size(), 1u);
  EXPECT_EQ(result_after->front(), file_id);
}

// A fake SemanticSearchProvider that records its calls and returns a fixed
// result set, used to verify the semantic-search seam (5B/5C) without starting
// the Rust runtime. The concrete provider is 5D.
class FakeSemanticSearchProvider : public SemanticSearchProvider {
 public:
  std::vector<FuzzySearchMatch> matches_{};
  mutable bool                  was_called_           = false;
  mutable std::wstring          last_query_           = {};
  mutable sl_element_id_t       last_folder_id_       = 0;
  mutable size_t                last_offset_          = 0;
  mutable size_t                last_limit_           = 0;

  [[nodiscard]] auto Search(sl_element_id_t folder_id, const std::wstring& query, size_t offset,
                            size_t limit) const -> std::vector<FuzzySearchMatch> override {
    was_called_     = true;
    last_query_     = query;
    last_folder_id_ = folder_id;
    last_offset_    = offset;
    last_limit_     = limit;
    return matches_;
  }
};

TEST_F(FilterServiceTests, SemanticSearchProviderDelegatesWhenRegistered) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  ASSERT_NE(CreateSyntheticFile(project, L"semantic_seam.dng", "Nikon D850"), 0u);

  SleeveFilterService filter_service(project.GetStorage());

  // No provider registered: the seam reports unavailable and returns nothing.
  EXPECT_FALSE(filter_service.HasSemanticSearchProvider());
  EXPECT_TRUE(filter_service.SearchFolderSemantic(0, L"sunset over mountains", 0, 10).empty());

  auto fake = std::make_shared<FakeSemanticSearchProvider>();
  fake->matches_.push_back(FuzzySearchMatch{.file_id_   = 4242,
                                            .image_id_  = 7,
                                            .file_name_ = "semantic_hit.dng"});
  filter_service.SetSemanticSearchProvider(fake);

  EXPECT_TRUE(filter_service.HasSemanticSearchProvider());
  const auto rows = filter_service.SearchFolderSemantic(0, L"sunset over mountains", 0, 10);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().file_id_, 4242u);
  EXPECT_EQ(rows.front().image_id_, 7u);
  EXPECT_EQ(rows.front().file_name_, "semantic_hit.dng");
  EXPECT_TRUE(fake->was_called_);
  EXPECT_EQ(fake->last_query_, L"sunset over mountains");
  EXPECT_EQ(fake->last_offset_, 0u);
  EXPECT_EQ(fake->last_limit_, 10u);

  // Clearing the provider restores the unavailable state.
  filter_service.SetSemanticSearchProvider(nullptr);
  EXPECT_FALSE(filter_service.HasSemanticSearchProvider());
  EXPECT_TRUE(filter_service.SearchFolderSemantic(0, L"sunset over mountains", 0, 10).empty());
}

TEST_F(FilterServiceTests, LabelQueryUsesOrdinaryPathNotSemanticProvider) {
  ProjectService project(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
  const auto     landscape_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"semantic_route.dng",
                                     .image_path_   = std::filesystem::path{L"D:/photos/r.dng"},
                                     .camera_model_ = "Neutral Body",
                                     .lens_         = "Plain Lens"});
  ASSERT_NE(landscape_id, 0u);

  auto& semantic = project.GetStorage()->GetSemanticStore();
  RegisterSemanticSearchModel(semantic, "mobileclip-route-test");
  StoreSemanticLabel(project, "mobileclip-route-test", landscape_id, "landscape", 4);

  SleeveFilterService filter_service(project.GetStorage());

  // Register a provider that would record any accidental semantic call.
  auto fake = std::make_shared<FakeSemanticSearchProvider>();
  filter_service.SetSemanticSearchProvider(fake);

  // A label query must resolve through the ordinary SQL path (5A) and reach the
  // stored label, without invoking the semantic provider.
  const auto rows = filter_service.SearchFolder(0, L"landscape", 0, 10);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().file_id_, landscape_id);
  EXPECT_FALSE(fake->was_called_);

  // The Chinese surface form of the same label also uses the ordinary path.
  const auto zh_rows = filter_service.SearchFolder(0, L"风景", 0, 10);
  ASSERT_EQ(zh_rows.size(), 1u);
  EXPECT_EQ(zh_rows.front().file_id_, landscape_id);
  EXPECT_FALSE(fake->was_called_);
}

TEST_F(FilterServiceTests, StatsBarAndSearchMergeUnderOneCompiledPredicate) {
  ProjectService project(db_path_, meta_path_);
  const auto     d850_24 = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"merge_a.dng",
                                 .image_path_   = std::filesystem::path{L"D:/merge/a.dng"},
                                 .camera_model_ = "Nikon D850",
                                 .lens_         = "NIKKOR 24mm"});
  const auto d850_50 = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"merge_b.dng",
                                 .image_path_   = std::filesystem::path{L"D:/merge/b.dng"},
                                 .camera_model_ = "Nikon D850",
                                 .lens_         = "NIKKOR 50mm"});
  const auto sony_50 = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"merge_c.dng",
                                 .image_path_   = std::filesystem::path{L"D:/merge/c.dng"},
                                 .camera_model_ = "Sony A7",
                                 .lens_         = "SEL 50mm"});
  ASSERT_NE(d850_24, 0u);
  ASSERT_NE(d850_50, 0u);
  ASSERT_NE(sony_50, 0u);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          stats_node = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");
  const auto          search_node = filter_service.BuildFuzzySearchWhere(L"50mm");
  ASSERT_TRUE(search_node.has_value());

  // Search alone: two files carry "50mm" in the lens field.
  const auto search_where = CompileFilterPredicate(search_node);
  ASSERT_TRUE(search_where.has_value());
  auto search_rows = project.GetStorage()->GetElementStore().ListFilesInFolderPage(
      0, 0, 0, search_where);
  ASSERT_EQ(search_rows.size(), 2u);
  EXPECT_EQ(filter_service.BuildFolderStats(0, search_node).total_photo_count_, 2);

  // Stats-bar alone: two Nikon D850 files.
  EXPECT_EQ(filter_service.BuildFolderStats(0, stats_node).total_photo_count_, 2);

  // Combined: only the Nikon D850 file whose lens contains "50mm".
  const auto merged = MergeFilterNodes(stats_node, search_node);
  ASSERT_TRUE(merged.has_value());
  const auto merged_where = CompileFilterPredicate(merged);
  ASSERT_TRUE(merged_where.has_value());
  const auto merged_rows =
      project.GetStorage()->GetElementStore().ListFilesInFolderPage(0, 0, 0,
                                                                                merged_where);
  ASSERT_EQ(merged_rows.size(), 1u);
  EXPECT_EQ(merged_rows.front().file_id_, d850_50);

  const auto merged_stats = filter_service.BuildFolderStats(0, merged);
  EXPECT_EQ(merged_stats.total_photo_count_, 1);
  (void)d850_24;
  (void)sony_50;
}

TEST_F(FilterServiceTests, StatsCameraBucketFilterRestrictsFolderStatsAndGrid) {
  ProjectService project(db_path_, meta_path_);
  const auto     nikon_id = CreateSyntheticFile(project, L"bucket_nikon.dng", "Nikon D850");
  const auto     sony_id  = CreateSyntheticFile(project, L"bucket_sony.dng", "Sony A7");
  ASSERT_NE(nikon_id, 0u);
  ASSERT_NE(sony_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          stats_node = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");

  const auto stats = filter_service.BuildFolderStats(0, stats_node);
  EXPECT_EQ(stats.total_photo_count_, 1);
  const auto camera_bucket = std::find_if(
      stats.camera_stats_.begin(), stats.camera_stats_.end(),
      [](const StatsBucket& row) { return row.label_ == "Nikon D850"; });
  ASSERT_NE(camera_bucket, stats.camera_stats_.end());
  EXPECT_EQ(camera_bucket->count_, 1);

  const auto where = CompileFilterPredicate(stats_node);
  ASSERT_TRUE(where.has_value());
  const auto rows = project.GetStorage()->GetElementStore().ListFilesInFolderPage(
      0, 0, 0, where);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().file_id_, nikon_id);
}

TEST_F(FilterServiceTests, StatsSemanticLabelExistsFilterRestrictsFolderStats) {
  ProjectService project(db_path_, meta_path_);
  const auto     landscape_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"stats_label_a.dng",
                                 .image_path_   = std::filesystem::path{L"D:/stats/a.dng"},
                                 .camera_model_ = "Neutral Body",
                                 .lens_         = "Plain Lens"});
  const auto portrait_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"stats_label_b.dng",
                                 .image_path_   = std::filesystem::path{L"D:/stats/b.dng"},
                                 .camera_model_ = "Neutral Body",
                                 .lens_         = "Plain Lens"});
  ASSERT_NE(landscape_id, 0u);
  ASSERT_NE(portrait_id, 0u);

  auto& semantic = project.GetStorage()->GetSemanticStore();
  RegisterSemanticSearchModel(semantic, "mobileclip-stats-filter");
  StoreSemanticLabel(project, "mobileclip-stats-filter", landscape_id, "landscape", 4);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          stats_node = sleeve_filter::BuildSemanticLabelExistsFilter(
      "mobileclip-stats-filter", SemanticLabelAliases("landscape"));

  const auto stats = filter_service.BuildFolderStats(0, stats_node);
  EXPECT_EQ(stats.total_photo_count_, 1);
  const auto label_bucket = std::find_if(
      stats.label_stats_.begin(), stats.label_stats_.end(),
      [](const StatsBucket& row) { return row.label_ == "landscape" && row.count_ == 1; });
  EXPECT_NE(label_bucket, stats.label_stats_.end());

  const auto where = CompileFilterPredicate(stats_node);
  ASSERT_TRUE(where.has_value());
  const auto rows = project.GetStorage()->GetElementStore().ListFilesInFolderPage(
      0, 0, 0, where);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().file_id_, landscape_id);
}

TEST_F(FilterServiceTests, StatsRatingBucketFilterRestrictsFolderStats) {
  ProjectService project(db_path_, meta_path_);
  const auto     four_star = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"stats_rating_a.dng",
                                 .image_path_   = std::filesystem::path{L"D:/stats/a.dng"},
                                 .camera_model_ = "Neutral Body",
                                 .rating_       = 4});
  const auto two_star = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"stats_rating_b.dng",
                                 .image_path_   = std::filesystem::path{L"D:/stats/b.dng"},
                                 .camera_model_ = "Neutral Body",
                                 .rating_       = 2});
  ASSERT_NE(four_star, 0u);
  ASSERT_NE(two_star, 0u);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          stats_node = sleeve_filter::BuildRatingBucketFilter(L"4");

  const auto stats = filter_service.BuildFolderStats(0, stats_node);
  EXPECT_EQ(stats.total_photo_count_, 1);
  const auto rating_bucket = std::find_if(
      stats.rating_stats_.begin(), stats.rating_stats_.end(),
      [](const StatsBucket& row) { return row.label_ == "4" && row.count_ == 1; });
  EXPECT_NE(rating_bucket, stats.rating_stats_.end());

  const auto where = CompileFilterPredicate(stats_node);
  ASSERT_TRUE(where.has_value());
  const auto rows = project.GetStorage()->GetElementStore().ListFilesInFolderPage(
      0, 0, 0, where);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().file_id_, four_star);
}

TEST_F(FilterServiceTests, StatsCaptureDateUnknownFilterMatchesFilesWithoutDate) {
  ProjectService project(db_path_, meta_path_);
  const auto     dated_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"stats_date_a.dng",
                                 .image_path_   = std::filesystem::path{L"D:/stats/a.dng"},
                                 .camera_model_ = "Neutral Body",
                                 .date_time_    = "2026-01-01 10:00:00"});
  const auto undated_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"stats_date_b.dng",
                                 .image_path_   = std::filesystem::path{L"D:/stats/b.dng"},
                                 .camera_model_ = "Neutral Body",
                                 .date_time_    = ""});
  ASSERT_NE(dated_id, 0u);
  ASSERT_NE(undated_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());

  const auto unknown_node = sleeve_filter::BuildCaptureDateUnknownFilter();
  const auto unknown_where = CompileFilterPredicate(unknown_node);
  ASSERT_TRUE(unknown_where.has_value());
  const auto unknown_rows =
      project.GetStorage()->GetElementStore().ListFilesInFolderPage(0, 0, 0,
                                                                                unknown_where);
  ASSERT_EQ(unknown_rows.size(), 1u);
  EXPECT_EQ(unknown_rows.front().file_id_, undated_id);
  EXPECT_EQ(filter_service.BuildFolderStats(0, unknown_node).total_photo_count_, 1);

  const auto known_node = sleeve_filter::BuildCaptureDateBucketFilter(L"2026-01-01");
  const auto known_where = CompileFilterPredicate(known_node);
  ASSERT_TRUE(known_where.has_value());
  const auto known_rows =
      project.GetStorage()->GetElementStore().ListFilesInFolderPage(0, 0, 0,
                                                                                known_where);
  ASSERT_EQ(known_rows.size(), 1u);
  EXPECT_EQ(known_rows.front().file_id_, dated_id);
  EXPECT_EQ(filter_service.BuildFolderStats(0, known_node).total_photo_count_, 1);
}

TEST_F(FilterServiceTests,
       StatsCameraBucketWithEmbeddedQuoteUsesPreparedBindsOnAlbumScopeQuery) {
  ProjectService project(db_path_, meta_path_);
  const auto     quoted_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"quote_cam_a.dng",
                                 .image_path_   = std::filesystem::path{L"D:/quote/a.dng"},
                                 .camera_model_ = "O'Brien Body"});
  const auto plain_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"quote_cam_b.dng",
                                 .image_path_   = std::filesystem::path{L"D:/quote/b.dng"},
                                 .camera_model_ = "Plain Body"});
  ASSERT_NE(quoted_id, 0u);
  ASSERT_NE(plain_id, 0u);

  const auto stats_node = sleeve_filter::BuildCameraModelBucketFilter(L"O'Brien Body");
  const auto predicate  = CompileFilterPredicate(stats_node);
  ASSERT_TRUE(predicate.has_value());
  ASSERT_FALSE(predicate->binds_.empty());
  EXPECT_EQ(std::get<std::string>(predicate->binds_.front()), "O'Brien Body");
  EXPECT_NE(predicate->sql_.find('?'), std::string::npos);

  const auto rows = project.GetStorage()->GetElementStore().ListFilesInFolderPage(
      0, 0, 0, predicate);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().file_id_, quoted_id);

  SleeveFilterService filter_service(project.GetStorage());
  EXPECT_EQ(filter_service.BuildFolderStats(0, stats_node).total_photo_count_, 1);
  EXPECT_EQ(project.GetStorage()->GetElementStore().CountFilesInFolder(0, predicate), 1u);
}

TEST_F(FilterServiceTests, FuzzySearchInjectPayloadDoesNotWidenFolderResults) {
  ProjectService project(db_path_, meta_path_);
  const auto     d850_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"inject_a.dng",
                                 .image_path_   = std::filesystem::path{L"D:/inject/a.dng"},
                                 .camera_model_ = "Nikon D850"});
  const auto     sony_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"inject_b.dng",
                                 .image_path_   = std::filesystem::path{L"D:/inject/b.dng"},
                                 .camera_model_ = "Sony A7"});
  ASSERT_NE(d850_id, 0u);
  ASSERT_NE(sony_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());

  const auto control = filter_service.SearchFolder(0, L"Nikon D850", 0, 10);
  ASSERT_EQ(control.size(), 1u);
  EXPECT_EQ(control.front().file_id_, d850_id);

  const auto inject_rows = filter_service.SearchFolder(0, L"' OR 1=1 --", 0, 10);
  EXPECT_TRUE(inject_rows.empty());
  EXPECT_EQ(filter_service.CountSearchResults(0, L"' OR 1=1 --"), 0u);

  const auto node = filter_service.BuildFuzzySearchWhere(L"' OR 1=1 --");
  ASSERT_TRUE(node.has_value());
  const auto predicate = CompileFilterPredicate(node);
  ASSERT_TRUE(predicate.has_value());
  EXPECT_NE(predicate->sql_.find('?'), std::string::npos);
  EXPECT_EQ(predicate->sql_.find("OR 1=1"), std::string::npos);
  ASSERT_FALSE(predicate->binds_.empty());
}

TEST_F(FilterServiceTests, FuzzySearchZeroFieldMaskCompilesToFalseRawSql) {
  ProjectService project(db_path_, meta_path_);
  const auto     file_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"mask_zero.dng",
                                 .image_path_   = std::filesystem::path{L"D:/mask/zero.dng"},
                                 .camera_model_ = "Nikon D850"});
  ASSERT_NE(file_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          node = filter_service.BuildFuzzySearchWhere(L"Nikon", SearchFieldMask{0});
  ASSERT_TRUE(node.has_value());
  EXPECT_EQ(node->type_, FilterNode::Type::RawSQL);
  ASSERT_TRUE(node->raw_sql_.has_value());
  EXPECT_EQ(*node->raw_sql_, L"FALSE");
  EXPECT_TRUE(node->raw_binds_.empty());

  EXPECT_TRUE(filter_service.SearchFolder(0, L"Nikon", 0, 10, SearchFieldMask{0}).empty());
  EXPECT_EQ(filter_service.CountSearchResults(0, L"Nikon", SearchFieldMask{0}), 0u);
}

TEST_F(FilterServiceTests, ExactFileWhereBindsFileIdAsParam) {
  ProjectService project(db_path_, meta_path_);
  const auto     file_id = CreateSyntheticFile(
      project, SyntheticFileSpec{.file_name_    = L"exact_id.dng",
                                 .image_path_   = std::filesystem::path{L"D:/exact/id.dng"},
                                 .camera_model_ = "Nikon D850"});
  ASSERT_NE(file_id, 0u);

  SleeveFilterService filter_service(project.GetStorage());
  const auto          node = filter_service.BuildExactFileWhere(file_id);
  EXPECT_EQ(node.type_, FilterNode::Type::RawSQL);

  const auto frag = FilterSQLCompiler::Compile(node);
  EXPECT_EQ(frag.sql_, "(e.id = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(frag.binds_[0]), static_cast<int64_t>(file_id));

  const auto predicate = CompileFilterPredicate(node);
  ASSERT_TRUE(predicate.has_value());
  const auto rows =
      project.GetStorage()->GetElementStore().ListFilesInFolderPage(0, 0, 0, predicate);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows.front().file_id_, file_id);
}

}  // namespace alcedo
