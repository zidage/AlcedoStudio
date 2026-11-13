//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Writes the marked project of the alcedo-cli read comparison (#807,
// scripts/storage_cli_compare/compare_storage_cli_reads.py). This program is not a ctest test:
// the script runs it once and gives the path of the new .alcd package in
// ALCEDO_STORAGE_COMPARE_PROJECT. Without that variable the test is skipped.
//
// Every value goes through a production write path, as in LibraryReferenceModelTest: files and
// history roots as the import writes them, ratings and marks through the image pool, albums and
// library deletion through AlbumBrowseService with the cleanup steps of the ImageController
// delete batch, edits through the library Paste, semantic labels through the semantic embedding
// store, and mask assets through PipelineMgmtService. The values that are hard for a query are
// part of the library: file names with `_`, `%`, quotes, and Chinese text; camera and lens text
// in two spellings and empty; capture dates that are valid, date-only, and invalid; NULL ISO;
// import times at the New York daylight-saving changes and at the Shanghai midnight; semantic
// labels in mixed case, with spaces, as Chinese label text, and outside the label taxonomy.
// One fixed random seed makes the project the same on each run.

#include <gtest/gtest.h>

#include <QString>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <format>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <system_error>
#include <vector>

#include "app/adjustment_transfer_service.hpp"
#include "app/album_browse_service.hpp"
#include "app/image_pool_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_package_backend.hpp"
#include "app/project_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/mask/mask_asset.hpp"
#include "image/color_label.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"
#include "library_search_test_support.hpp"
#include "storage/store/semantic/semantic_records.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/semantic_stores_test_support.hpp"
#include "support/storage_invariants.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

constexpr uint64_t               kSeed           = 807;
constexpr int                    kFileCount      = 72;
constexpr const char*            kModelKey       = "storage-compare-model";
constexpr const char*            kProjectName    = "storage_compare_marked";
constexpr const char*            kOutputVariable = "ALCEDO_STORAGE_COMPARE_PROJECT";

const std::vector<std::wstring>& NamePrefixes() {
  static const std::vector<std::wstring> prefixes{
      L"IMG_", L"DSC", L"photo%", L"风景_", L"a_b", L"Ab", L"quote\"d ", L"it's ", L"100% "};
  return prefixes;
}

const std::vector<std::wstring>& NameExtensions() {
  static const std::vector<std::wstring> extensions{L".CR3", L".jpg", L".NEF",
                                                    L".dng", L".JPG", L".ARW"};
  return extensions;
}

/// EXIF date texts: valid, date-only, empty, and not a real time or day.
const std::vector<std::string>& CaptureTexts() {
  static const std::vector<std::string> texts{"",
                                              "2026:06:07 10:11:12",
                                              "2026-06-07 23:59:59",
                                              "2026:06:07 00:00:00",
                                              "2025:12:31 08:00:00",
                                              "2026:03:08",
                                              "2024:02:29 08:30:00",
                                              "0000:00:00 00:00:00",
                                              "2026:02:29 10:00:00"};
  return texts;
}

/// Camera models: two spellings of one camera, the empty text, and Chinese text.
const std::vector<std::string>& CameraModels() {
  static const std::vector<std::string> models{"Canon EOS R5", "NIKON Z 8",    "",
                                               "canon eos r5", "X100V",        "ILCE-7RM5",
                                               "相机 100%",    "Canon EOS R5 "};
  return models;
}

const std::vector<std::string>& Lenses() {
  static const std::vector<std::string> lenses{"RF50mm F1.2 L USM",     "",
                                               "XF23mmF2 R WR",         "NIKKOR Z 24-70mm f/4 S",
                                               "FE 24-70mm F2.8 GM II", "rf50mm f1.2 l usm"};
  return lenses;
}

const std::vector<uint64_t>& IsoValues() {
  // 0 is stored as NULL.
  static const std::vector<uint64_t> values{0, 100, 400, 3200, 12800};
  return values;
}

/// Import times (Unix seconds, UTC) at the New York daylight-saving changes of 2026 and at the
/// Shanghai and New York midnights; the other files get a time in 2026.
const std::vector<int64_t>& SpecialImportTimes() {
  static const std::vector<int64_t> times{
      1772952000,  // 2026-03-08 06:40:00 UTC, 01:40 EST
      1772953199,  // 2026-03-08 06:59:59 UTC, 01:59:59 EST
      1772953200,  // 2026-03-08 07:00:00 UTC, 03:00 EDT
      1793512800,  // 2026-11-01 05:00:00 UTC, 01:00 EDT
      1793516400,  // 2026-11-01 06:00:00 UTC, 01:00 EST
      1780761600,  // 2026-06-06 16:00:00 UTC, 2026-06-07 00:00 Shanghai
      1780761599,  // 2026-06-06 15:59:59 UTC, 23:59:59 Shanghai
      1780804800,  // 2026-06-07 04:00:00 UTC, 00:00 EDT
  };
  return times;
}

/// Stored semantic labels. The open of a project rewrites a label text of the taxonomy to its
/// key; the stats and the label filter compare LOWER and TRIM of the stored label.
const std::vector<std::string>& SemanticLabels() {
  static const std::vector<std::string> labels{"sky",       "Sky",      "SKY ",   "beach",
                                               "Beach",     "沙漠",     "desert", "my_label%",
                                               "My_Label%", "城市 夜景"};
  return labels;
}

auto MakeMaskAsset(int variant) -> MaskAsset {
  MaskAsset asset;
  asset.descriptor.extent           = {32, 24};
  asset.descriptor.reference_bounds = NormalizedRect{0.125f, 0.25f, 0.5f, 0.5f};
  asset.pixels.assign(32 * 24, 0);
  for (std::size_t y = 2; y < 10 + static_cast<std::size_t>(variant); ++y) {
    for (std::size_t x = 3; x < 12 + static_cast<std::size_t>(variant); ++x) {
      asset.pixels[y * 32 + x] = 255;
    }
  }
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

class StorageCliCompareProjectWriter : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* out = std::getenv(kOutputVariable);
    if (out == nullptr || *out == '\0') {
      GTEST_SKIP() << kOutputVariable << " names no output file";
    }
    out_path_ = std::filesystem::u8path(out);
    TimeProvider::Refresh();
    rng_.seed(kSeed);
    workspace_ = std::filesystem::temp_directory_path() /
                 std::format("alcedo_StorageCliCompareProjectWriter_{}", std::random_device{}());
    std::filesystem::create_directories(workspace_);
    std::tie(db_path_, meta_path_) =
        project_pack::BuildRuntimeProjectPair(workspace_, QString::fromLatin1(kProjectName));
    project_ = std::make_shared<ProjectService>(db_path_, meta_path_, ProjectOpenMode::kCreateNew);
    pipelines_ = std::make_unique<PipelineMgmtService>(project_->GetStorage());
  }

  void TearDown() override {
    pipelines_.reset();
    project_.reset();
    std::error_code ec;
    if (!workspace_.empty()) {
      std::filesystem::remove_all(workspace_, ec);
    }
  }

  auto Uniform(int64_t low, int64_t high) -> int64_t {
    return std::uniform_int_distribution<int64_t>(low, high)(rng_);
  }
  auto Chance(double probability) -> bool { return std::bernoulli_distribution(probability)(rng_); }
  template <typename T>
  auto Pick(const std::vector<T>& values) -> const T& {
    return values[static_cast<size_t>(Uniform(0, static_cast<int64_t>(values.size()) - 1))];
  }

  auto Browse() -> AlbumBrowseService& { return *project_->GetAlbumBrowseService(); }

  /// Files as the import writes them: Image row, file element, history root.
  void AddFiles() {
    std::vector<SyntheticImageSpec> specs;
    for (int i = 0; i < kFileCount; ++i) {
      const auto    name = Pick(NamePrefixes()) + std::format(L"{:04}", i) + Pick(NameExtensions());
      const int64_t added_time =
          Chance(0.35) ? Pick(SpecialImportTimes()) : 1767225600 + Uniform(0, 365 * 86400);
      specs.push_back(SyntheticImageSpec{.file_name_   = name,
                                         .image_path_  = L"D:/storage_compare_library/" + name,
                                         .make_        = Chance(0.8) ? "Make" : "",
                                         .model_       = Pick(CameraModels()),
                                         .lens_        = Pick(Lenses()),
                                         .date_time_   = Pick(CaptureTexts()),
                                         .iso_         = Pick(IsoValues()),
                                         .aperture_    = Chance(0.7) ? 2.8f : 0.0f,
                                         .focal_       = Chance(0.7) ? 35.0f : 0.0f,
                                         .rating_      = static_cast<int>(Uniform(0, 5)),
                                         .is_raw_file_ = Chance(0.6),
                                         .added_time_  = static_cast<std::time_t>(added_time)});
    }
    SyntheticLibraryBuilder builder(*project_);
    file_ids_ = builder.AddFiles(specs);
    ASSERT_EQ(file_ids_.size(), specs.size());
    for (const auto id : file_ids_) {
      pipelines_->InitializeImageRoot(id, CreateDefaultPipelineDocument(), nullptr);
    }
    const auto rows = Browse().ReadAlbumFileRows(file_ids_);
    ASSERT_EQ(rows.size(), file_ids_.size());
    for (const auto& row : rows) {
      image_ids_.push_back(row.image_id_);
    }
  }

  /// Change one image in the pool and write it, as ImageController does.
  void WriteImage(size_t index, std::function<void(std::shared_ptr<Image>)> change) {
    auto image_pool = project_->GetImagePoolService();
    image_pool->Write_NoSync<void>(image_ids_[index], std::move(change));
    const auto status = image_pool->SyncWithStorage();
    ASSERT_TRUE(status.failed_images_.empty()) << "image " << image_ids_[index];
  }

  void WriteMarks() {
    for (size_t i = 0; i < file_ids_.size(); ++i) {
      if (Chance(0.4)) {
        const int rating = static_cast<int>(Uniform(0, 5));
        WriteImage(i, [rating](const std::shared_ptr<Image>& image) {
          // The write of ImageController::SetImageRating.
          ExifDisplayMetaData metadata;
          if (image->has_exif_display_.load()) {
            metadata = image->exif_display_;
          } else if (image->has_exif_json_.load()) {
            metadata.FromJson(image->exif_json_);
          }
          metadata.rating_ = ExifDisplayMetaData::NormalizeRating(rating);
          image->SetExifDisplayMetaData(std::move(metadata));
        });
      }
      const auto label_count = Uniform(0, 3);
      for (int64_t l = 0; l < label_count; ++l) {
        const auto code = static_cast<ColorLabel>(Uniform(0, kColorLabelCount - 1));
        WriteImage(
            i, [code](const std::shared_ptr<Image>& image) { image->SetColorLabel(code, true); });
      }
      if (Chance(0.2)) {
        WriteImage(i, [](const std::shared_ptr<Image>& image) { image->SetDeleteMark(true); });
      }
    }
  }

  void WriteSemanticLabels() {
    auto        semantic = semantic_test::StoresOf(*project_->GetStorage());
    std::string error;
    ASSERT_TRUE(
        semantic.models_.UpsertModel(SemanticModelRecord{.model_key_     = kModelKey,
                                                         .model_id_      = "storage-compare-model",
                                                         .revision_      = "rev-807",
                                                         .embedding_dim_ = kSemanticEmbeddingDim,
                                                         .image_size_    = 256,
                                                         .active_        = true},
                                     &error))
        << error;
    ASSERT_TRUE(semantic.models_.SetActiveModelKey(kModelKey, &error)) << error;
    for (size_t i = 0; i < file_ids_.size(); ++i) {
      if (Chance(0.25)) {
        continue;
      }
      std::vector<float> embedding(kSemanticEmbeddingDim, 0.0F);
      embedding.at(i % kSemanticEmbeddingDim) = 1.0F;
      const auto&              label          = Pick(SemanticLabels());
      SemanticImageLabelRecord record{.file_id_         = file_ids_[i],
                                      .model_key_       = kModelKey,
                                      .label_           = label,
                                      .score_           = 0.5 + 0.004 * static_cast<double>(i),
                                      .second_label_    = Pick(SemanticLabels()),
                                      .second_score_    = 0.1,
                                      .margin_          = 0.3,
                                      .confident_       = Chance(0.7),
                                      .top_scores_json_ = R"([{"label":"sky","score":0.5}])"};
      ASSERT_TRUE(semantic.embeddings_.UpsertImageEmbeddingWithLabel(
          SemanticImageEmbeddingRecord{.file_id_   = file_ids_[i],
                                       .image_id_  = image_ids_[i],
                                       .model_key_ = kModelKey,
                                       .embedding_ = std::move(embedding)},
          &record, &error))
          << error;
    }
  }

  /// Paste an exposure adjustment as a new Version: the library Paste of
  /// AdjustmentTransferApplyCoordinator.
  void PasteEdit(sl_element_id_t id) {
    const auto base = pipelines_->LoadHistorySnapshot(id);
    ASSERT_TRUE(base.graph_ && base.root_);
    CommitGraph graph  = *base.graph_;
    const auto  pasted = AdjustmentTransferService::PasteAsRootRelativeVersion(
        graph, base.root_->document,
        test::MakeExposureTransferPackage(0.25 * static_cast<double>(Uniform(1, 8))), "Pasted");
    ASSERT_TRUE(pasted.pasted) << pasted.error;
    ASSERT_NE(pipelines_->PersistHistory(base, graph), nullptr);
  }

  void WriteEdits() {
    for (size_t i = 0; i < file_ids_.size(); ++i) {
      const auto commits = Chance(0.35) ? Uniform(1, 3) : 0;
      for (int64_t c = 0; c < commits; ++c) {
        PasteEdit(file_ids_[i]);
      }
    }
    for (int variant = 0; variant < 3; ++variant) {
      pipelines_->PutMaskAsset(MakeMaskAsset(variant));
    }
  }

  void WriteAlbums() {
    const auto first  = Browse().CreateFolder(L"/", L"Album 1");
    const auto second = Browse().CreateFolder(L"/", L"相册 \"二\"");
    ASSERT_TRUE(first.has_value() && second.has_value());
    const auto nested = Browse().CreateFolder(L"/Album 1", L"Sub_%");
    ASSERT_TRUE(nested.has_value());
    std::vector<sl_element_id_t> first_ids;
    std::vector<sl_element_id_t> second_ids;
    std::vector<sl_element_id_t> nested_ids;
    for (const auto id : file_ids_) {
      if (Chance(0.4)) {
        first_ids.push_back(id);
      }
      if (Chance(0.3)) {
        second_ids.push_back(id);
      }
      if (Chance(0.15)) {
        nested_ids.push_back(id);
      }
    }
    for (const auto& [folder, ids] :
         {std::pair{first->folder_id_, first_ids}, std::pair{second->folder_id_, second_ids},
          std::pair{nested->folder_id_, nested_ids}}) {
      const auto result = Browse().LinkFilesToFolder(ids, folder);
      ASSERT_TRUE(result.failed_element_ids_.empty());
    }
  }

  /// Delete files from the library with the steps of ImageController's delete batch.
  void DeleteSomeFiles() {
    std::vector<sl_element_id_t> ids;
    std::vector<image_id_t>      image_ids;
    for (size_t i = 3; i < file_ids_.size(); i += 17) {
      ids.push_back(file_ids_[i]);
      image_ids.push_back(image_ids_[i]);
    }
    const auto result = Browse().DeleteFilesInFolderByElementIds(0, ids);
    ASSERT_TRUE(result.failed_element_ids_.empty());
    pipelines_->DeletePipelines(ids);
    auto image_pool = project_->GetImagePoolService();
    image_pool->RemoveBatch(image_ids);
    image_pool->SyncWithStorage();
  }

  /// The save of ProjectHandler: sync the services, write the metadata, and pack a snapshot of
  /// the live database.
  void Pack() {
    project_->GetSleeveService()->Sync();
    project_->GetImagePoolService()->SyncWithStorage();
    project_->SaveProject(meta_path_);
    QString               error;
    std::filesystem::path snapshot_path;
    ASSERT_TRUE(project_pack::BuildTempDbSnapshotPath(&snapshot_path, &error))
        << error.toStdString();
    ASSERT_TRUE(project_pack::CreateLiveDbSnapshot(project_, snapshot_path, &error))
        << error.toStdString();
    std::filesystem::create_directories(out_path_.parent_path());
    ASSERT_TRUE(project_pack::WritePackedProject(out_path_, meta_path_, snapshot_path, &error))
        << error.toStdString();
    std::error_code ec;
    std::filesystem::remove(snapshot_path, ec);
  }

  std::filesystem::path                out_path_;
  std::filesystem::path                workspace_;
  std::filesystem::path                db_path_;
  std::filesystem::path                meta_path_;
  std::mt19937_64                      rng_;
  std::shared_ptr<ProjectService>      project_;
  std::unique_ptr<PipelineMgmtService> pipelines_;
  std::vector<sl_element_id_t>         file_ids_;
  std::vector<image_id_t>              image_ids_;
};

TEST_F(StorageCliCompareProjectWriter, WritesMarkedLabeledEditedProject) {
  ASSERT_NO_FATAL_FAILURE(AddFiles());
  ASSERT_NO_FATAL_FAILURE(WriteMarks());
  ASSERT_NO_FATAL_FAILURE(WriteSemanticLabels());
  ASSERT_NO_FATAL_FAILURE(WriteEdits());
  ASSERT_NO_FATAL_FAILURE(WriteAlbums());
  ASSERT_NO_FATAL_FAILURE(DeleteSomeFiles());
  const auto violations = test::FindStorageInvariantViolations(*project_->GetStorage());
  ASSERT_TRUE(violations.empty()) << test::FormatStorageInvariantViolations(violations);
  ASSERT_NO_FATAL_FAILURE(Pack());
  EXPECT_TRUE(std::filesystem::exists(out_path_));
}

}  // namespace
}  // namespace alcedo
