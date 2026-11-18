// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include <gtest/gtest.h>

#include <QTemporaryDir>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include "app/import_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "edit/history/edit_commit.hpp"
#include "edit/history/pipeline_edit_batch.hpp"
#include "edit/mask/mask_asset.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/image/source_file_store.hpp"
#include "support/storage_test_access.hpp"

namespace alcedo {
namespace {
/// Keep test files under the writable build workspace, including hard-link measurements.
auto ImportTestDirectoryPattern() -> QString {
  const auto root =
      std::filesystem::path(__FILE__).parent_path().parent_path().parent_path().parent_path() /
      "build/tmp/import_duplicate_skipping";
  std::filesystem::create_directories(root);
  return QString::fromStdWString((root / "case-XXXXXX").wstring());
}
class ImportDuplicateSkippingTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(directory_.isValid());
    root_ = std::filesystem::path(directory_.path().toStdWString());
    Reopen();
    source_ = Copy("first/image.jpg");
  }
  void Reopen() {
    importer_.reset();
    if (project_) project_->SaveProject(root_ / "project.json");
    project_.reset();
    project_  = std::make_unique<ProjectService>(root_ / "project.db", root_ / "project.json");
    importer_ = std::make_unique<ImportServiceImpl>(
        project_->GetSleeveService(), project_->GetImagePoolService(),
        std::make_shared<PipelineMgmtService>(project_->GetStorage()));
  }
  auto Copy(const char* name) -> image_path_t {
    auto path = root_ / name;
    std::filesystem::create_directories(path.parent_path());
    std::filesystem::copy_file(
        std::filesystem::path(ALCEDO_RASTER_FIXTURE_DIR) / "adobe_rgb_exif_r03_no_icc.jpg", path);
    return path;
  }
  auto Run(const std::vector<image_path_t>& paths, bool skip = true,
           const std::shared_ptr<ImportJob>& supplied = {}, const image_path_t& destination = {})
      -> ImportResult {
    auto                       job = supplied ? supplied : std::make_shared<ImportJob>();
    std::promise<ImportResult> done;
    auto                       ready = done.get_future();
    job->on_finished_                = [&](const ImportResult& result) { done.set_value(result); };
    ImportOptions options;
    options.skip_duplicates_ = skip;
    last_job_                = importer_->ImportToFolder(paths, destination, options, job);
    if (ready.wait_for(std::chrono::seconds(90)) != std::future_status::ready) {
      ADD_FAILURE() << "Import did not complete";
      return {};
    }
    auto result = ready.get();
    EXPECT_EQ(result.requested_,
              result.imported_ + result.updated_ + result.duplicates_ + result.failed_);
    EXPECT_EQ(result.raw_requested_, result.raw_imported_ + result.raw_updated_ +
                                         result.raw_duplicates_ + result.raw_failed_);
    return result;
  }
  void Execute(const std::string& sql) {
    auto guard = StorageTestAccess::GetDatabase(*project_->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    duckorm::execute(guard.conn_, duckorm::expr::raw(sql));
  }
  auto Count(const std::string& table) -> int64_t {
    auto guard = StorageTestAccess::GetDatabase(*project_->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    return duckorm::select_int64(guard.conn_, duckorm::expr::raw("SELECT COUNT(*) FROM " + table))
        .value();
  }
  auto ImageId() -> image_id_t {
    return last_job_->import_log_->Snapshot().created_.front().image_id_;
  }
  auto StoredRows(const std::string& table) -> std::string {
    auto guard = StorageTestAccess::GetDatabase(*project_->GetStorage()).GetConnectionGuard();
    auto lock  = guard.Lock();
    return duckorm::select_string(
               guard.conn_, duckorm::expr::raw("SELECT to_json(list(t)) FROM (SELECT * FROM " +
                                               table + " ORDER BY ALL) t"))
        .value_or("");
  }
  QTemporaryDir                      directory_{ImportTestDirectoryPattern()};
  image_path_t                       root_;
  image_path_t                       source_;
  std::unique_ptr<ProjectService>    project_;
  std::unique_ptr<ImportServiceImpl> importer_;
  std::shared_ptr<ImportJob>         last_job_;
};

TEST_F(ImportDuplicateSkippingTest, CompletionRunsAfterImageLibraryRootAndSourceSizePersistence) {
  const auto result = Run({source_});
  ASSERT_EQ(result.imported_, 1);
  EXPECT_EQ(Count("Image"), 1);
  EXPECT_EQ(Count("FileImage"), 1);
  EXPECT_EQ(Count("PipelineRoot"), 1);
  EXPECT_EQ(Count("ImageSourceSize"), 1);
  EXPECT_EQ(Count("ImageSourceSize WHERE byte_size = " +
                  std::to_string(std::filesystem::file_size(source_))),
            1);
  EXPECT_EQ(Count("information_schema.tables WHERE table_name='ImageSourceFingerprint'"), 0);
}
TEST_F(ImportDuplicateSkippingTest, RepeatedImportSkipsAndPreservesExistingRatingAndRoot) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto id = ImageId();
  project_->GetImagePoolService()->Write<void>(
      id, [](const std::shared_ptr<Image>& image) { image->exif_display_.rating_ = 4; });
  const auto repeated = Run({source_});
  EXPECT_EQ(repeated.duplicates_, 1);
  EXPECT_EQ(repeated.imported_, 0);
  EXPECT_EQ(repeated.failed_, 0);
  EXPECT_EQ(Count("Image WHERE rating = 4"), 1);
  EXPECT_EQ(Count("PipelineRoot"), 1);
  EXPECT_EQ(Count("VersionRef"), 1);
  EXPECT_TRUE(last_job_->import_log_->Snapshot().created_.empty());
}
TEST_F(ImportDuplicateSkippingTest, MissingSizeOwnerRejectsTheWholeUpdateWithoutPartialRows) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto                                         id     = ImageId();
  const auto                                         before = StoredRows("ImageSourceSize");
  auto& store = project_->GetStorage()->GetSourceFileStore();
  const std::vector<std::pair<image_id_t, uint64_t>> sizes{{id, 17}, {id + 1000, 18}};
  EXPECT_THROW(store.RecordSizes(sizes), std::runtime_error);
  EXPECT_EQ(StoredRows("ImageSourceSize"), before);
}
TEST_F(ImportDuplicateSkippingTest, IdenticalBytesInDifferentFoldersCreateSeparateEntries) {
  const auto other  = Copy("second/image.jpg");
  const auto result = Run({source_, other});
  EXPECT_EQ(result.imported_, 2);
  EXPECT_EQ(result.duplicates_, 0);
  EXPECT_EQ(Count("ImageSourceSize"), 2);
  EXPECT_EQ(Count("PipelineRoot"), 2);
}
TEST_F(ImportDuplicateSkippingTest, SameFilenameWithDifferentFolderAndContentImportsSeparately) {
  const auto other = Copy("different/image.jpg");
  {
    std::ofstream(other, std::ios::binary | std::ios::app) << 'x';
  }
  const auto result = Run({source_, other});
  EXPECT_EQ(result.imported_, 2);
  EXPECT_EQ(result.duplicates_, 0);
  EXPECT_EQ(Count("ImageSourceSize"), 2);
}
TEST_F(ImportDuplicateSkippingTest, DuplicateAndFailureCountsIncludeTheirCorrectRawSubsets) {
  const auto raw_extension = Copy("raw/image.NEF");
  const auto result = Run({source_, raw_extension, source_, raw_extension, root_ / "missing.NEF"});
  EXPECT_EQ(result.imported_, 2);
  EXPECT_EQ(result.duplicates_, 2);
  EXPECT_EQ(result.failed_, 1);
  EXPECT_EQ(result.raw_requested_, 3);
  EXPECT_EQ(result.raw_imported_, 1);
  EXPECT_EQ(result.raw_duplicates_, 1);
  EXPECT_EQ(result.raw_failed_, 1);
}

TEST_F(ImportDuplicateSkippingTest, ChangedRawSourceCountsOneUpdateAndOneLaterDuplicate) {
  const auto source = Copy("raw/source.NEF");
  ASSERT_EQ(Run({source}).raw_imported_, 1);
  std::ofstream(source, std::ios::binary | std::ios::app) << 'x';
  const auto result = Run({source, source});
  EXPECT_EQ(result.raw_requested_, 2);
  EXPECT_EQ(result.updated_, 1);
  EXPECT_EQ(result.raw_updated_, 1);
  EXPECT_EQ(result.duplicates_, 1);
  EXPECT_EQ(result.raw_duplicates_, 1);
  EXPECT_EQ(result.raw_imported_, 0);
  EXPECT_EQ(result.failed_, 0);
}
TEST_F(ImportDuplicateSkippingTest, SameJobEquivalentPathSpellingsCreateOnePersistedEntry) {
  const auto equivalent = source_.parent_path() / "." / source_.filename();
  const auto result     = Run({source_, equivalent, source_});
  EXPECT_EQ(result.imported_, 1);
  EXPECT_EQ(result.duplicates_, 2);
  EXPECT_EQ(result.failed_, 0);
  EXPECT_EQ(Count("FileImage"), 1);
  EXPECT_EQ(Count("ImageSourceSize"), 1);
}
TEST_F(ImportDuplicateSkippingTest, ChangedSizeUpdatesTheExistingSourceAndPreservesItsRating) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto id = ImageId();
  project_->GetImagePoolService()->Write<void>(id, [](const auto& image) {
    image->exif_display_.rating_ = 4;
    image->SetColorLabel(ColorLabel::kRed, true);
    image->SetDeleteMark(true);
  });
  const auto image    = StoredRows("Image");
  const auto roots    = StoredRows("PipelineRoot");
  const auto versions = StoredRows("VersionRef");
  std::ofstream(source_, std::ios::binary | std::ios::app) << 'x';
  const auto result = Run({source_, source_});
  EXPECT_EQ(result.updated_, 1);
  EXPECT_EQ(result.imported_, 0);
  EXPECT_EQ(result.failed_, 0);
  EXPECT_EQ(result.duplicates_, 1);
  ASSERT_EQ(result.source_changes_.size(), 1);
  EXPECT_EQ(result.source_changes_.front().path, source_);
  ASSERT_EQ(result.source_changes_.front().entries.size(), 1);
  EXPECT_EQ(result.source_changes_.front().entries.front().second, id);
  Reopen();
  EXPECT_EQ(StoredRows("Image"), image);
  EXPECT_EQ(StoredRows("PipelineRoot"), roots);
  EXPECT_EQ(StoredRows("VersionRef"), versions);
  EXPECT_EQ(Count("FileImage"), 1);
  EXPECT_EQ(Count("ImageSourceSize WHERE byte_size = " +
                  std::to_string(std::filesystem::file_size(source_))),
            1);
  EXPECT_EQ(Run({source_}).duplicates_, 1);
}
TEST_F(ImportDuplicateSkippingTest, SamePathAndSizeSkipsWithoutInspectingChangedContents) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto before = StoredRows("ImageSourceSize");
  {
    std::fstream file(source_, std::ios::binary | std::ios::in | std::ios::out);
    file.put('x');
  }
  const auto result = Run({source_});
  EXPECT_EQ(result.failed_, 0);
  EXPECT_EQ(result.duplicates_, 1);
  EXPECT_EQ(result.updated_, 0);
  EXPECT_TRUE(result.source_changes_.empty());
  EXPECT_EQ(StoredRows("ImageSourceSize"), before);
}
TEST_F(ImportDuplicateSkippingTest, SkipDisabledCreatesIndependentEntriesAndRecordsEverySize) {
  const auto result = Run({source_, source_}, false);
  EXPECT_EQ(result.imported_, 2);
  EXPECT_EQ(result.duplicates_, 0);
  EXPECT_EQ(Count("FileImage"), 2);
  EXPECT_EQ(Count("PipelineRoot"), 2);
  EXPECT_EQ(Count("ImageSourceSize"), 2);
}
TEST_F(ImportDuplicateSkippingTest,
       EntryWithoutRecordedSizeSkipsAndRecordsCurrentSizeAcrossReopen) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto images = StoredRows("Image");
  const auto roots  = StoredRows("PipelineRoot");
  Execute("DELETE FROM ImageSourceSize");
  Reopen();
  const auto result = Run({source_});
  EXPECT_EQ(result.duplicates_, 1);
  EXPECT_EQ(result.updated_, 0);
  Reopen();
  EXPECT_EQ(StoredRows("Image"), images);
  EXPECT_EQ(StoredRows("PipelineRoot"), roots);
  EXPECT_EQ(Count("ImageSourceSize"), 1);
}
TEST_F(ImportDuplicateSkippingTest, SavedDuplicateImportReopensWithOneSamePathEntry) {
  EXPECT_EQ(Run({source_, source_}).duplicates_, 1);
  Reopen();
  EXPECT_EQ(Run({source_}).duplicates_, 1);
  EXPECT_EQ(Count("FileImage"), 1);
  EXPECT_EQ(Count("PipelineRoot"), 1);
}
TEST_F(ImportDuplicateSkippingTest, UnrelatedUnindexedOfflineEntryDoesNotBlockNewPathImport) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  Execute("DELETE FROM ImageSourceSize");
  std::filesystem::remove(source_);
  EXPECT_EQ(Run({Copy("second/image.jpg")}).imported_, 1);
  EXPECT_EQ(Count("FileImage"), 2);
}
TEST_F(ImportDuplicateSkippingTest, FailedRepresentativeCannotProduceSuccessfulFollowerSkips) {
  const auto invalid = root_ / "invalid.NEF";
  {
    std::ofstream(invalid) << "invalid image";
  }
  const auto result = Run({invalid, invalid});
  EXPECT_EQ(result.failed_, 2);
  EXPECT_EQ(result.duplicates_, 0);
  EXPECT_EQ(Count("Image"), 0);
  EXPECT_EQ(Count("FileImage"), 0);
  EXPECT_EQ(Count("ImageSourceSize"), 0);
}
TEST_F(ImportDuplicateSkippingTest, SizeWriteFailureRemovesProvisionalBindingAndRoot) {
  Execute(
      "CREATE OR REPLACE TABLE ImageSourceSize (image_id BIGINT PRIMARY KEY, "
      "byte_size UBIGINT NOT NULL CHECK(byte_size = 0))");
  const auto result = Run({source_, source_});
  EXPECT_EQ(result.imported_, 0);
  EXPECT_EQ(result.duplicates_, 0);
  EXPECT_EQ(result.failed_, 2);
  EXPECT_EQ(Count("Image"), 0);
  EXPECT_EQ(Count("FileImage"), 0);
  EXPECT_EQ(Count("PipelineRoot"), 0);
}
TEST_F(ImportDuplicateSkippingTest, OverlappingJobIsRejectedUntilFinalizationCompletes) {
  auto               paused = std::make_shared<ImportJob>();
  std::promise<void> entered, release;
  auto               proceed = release.get_future().share();
  std::atomic<bool>  once    = false;
  paused->on_progress_       = [&](const ImportProgress&) {
    if (!once.exchange(true)) {
      entered.set_value();
      proceed.wait();
    }
  };
  std::promise<ImportResult> first;
  auto                       finished = first.get_future();
  paused->on_finished_ = [&](const ImportResult& result) { first.set_value(result); };
  importer_->ImportToFolder({source_}, L"", {}, paused);
  entered.get_future().wait();
  const auto busy = Run({source_});
  EXPECT_EQ(busy.failed_, 1);
  ASSERT_EQ(busy.errors_.size(), 1);
  EXPECT_EQ(busy.errors_[0].code_, ImportErrorCode::BUSY);
  release.set_value();
  EXPECT_EQ(finished.get().imported_, 1);
}
TEST_F(ImportDuplicateSkippingTest, PreCanceledJobReportsEveryOccurrenceExactlyOnce) {
  auto canceled       = std::make_shared<ImportJob>();
  canceled->canceled_ = true;
  const auto result   = Run({source_, source_, source_}, true, canceled);
  EXPECT_EQ(result.canceled_, 3);
  EXPECT_EQ(result.failed_, 3);
  EXPECT_EQ(Count("FileImage"), 0);
}

TEST_F(ImportDuplicateSkippingTest, CanceledJobFinalizesCompletedEntriesExactlyOnce) {
  auto job          = std::make_shared<ImportJob>();
  job->on_progress_ = [job_ptr = job.get()](const ImportProgress& progress) {
    if (progress.metadata_done_.load() == 1) job_ptr->canceled_ = true;
  };
  const auto result = Run({source_, source_, Copy("later/image.jpg")}, true, job);
  EXPECT_EQ(result.imported_, 1);
  EXPECT_EQ(result.duplicates_, 0);
  EXPECT_EQ(result.canceled_, 2);
  EXPECT_EQ(result.failed_, 2);
  EXPECT_EQ(Count("Image"), 1);
  EXPECT_EQ(Count("FileImage"), 1);
  EXPECT_EQ(Count("PipelineRoot"), 1);
  EXPECT_EQ(Count("ImageSourceSize"), 1);
  EXPECT_EQ(job->metadata_tasks_finished_.load(), job->metadata_tasks_submitted_.load());
}

TEST_F(ImportDuplicateSkippingTest, ImageWriteFailureCannotBeReportedAsImported) {
  Execute("CREATE UNIQUE INDEX reject_image_name ON Image(file_name)");
  const auto result = Run({source_, Copy("second/image.jpg")});
  EXPECT_EQ(result.imported_, 0);
  EXPECT_EQ(result.failed_, 2);
  EXPECT_EQ(Count("Image"), 0);
  EXPECT_EQ(Count("FileImage"), 0);
  EXPECT_EQ(Count("ImageSourceSize"), 0);
}
TEST_F(ImportDuplicateSkippingTest, RootWriteFailureRemovesProvisionalBinding) {
  Execute(
      "CREATE OR REPLACE TABLE PipelineRoot (root_id VARCHAR PRIMARY KEY, "
      "element_id BIGINT UNIQUE NOT NULL CHECK(element_id < 0), serialized_pipeline_state JSON NOT "
      "NULL)");
  const auto result = Run({source_});
  EXPECT_EQ(result.imported_, 0);
  EXPECT_EQ(result.failed_, 1);
  EXPECT_TRUE(result.restoration_error_.empty());
  EXPECT_EQ(Count("Image"), 0);
  EXPECT_EQ(Count("FileImage"), 0);
  EXPECT_EQ(Count("VersionRef"), 0);
}
TEST_F(ImportDuplicateSkippingTest, LibraryWriteFailureRestoresProvisionalImages) {
  Execute(
      "CREATE OR REPLACE TABLE FileImage (file_id BIGINT, image_id BIGINT CHECK(image_id < 0))");
  const auto result = Run({source_});
  EXPECT_EQ(result.imported_, 0);
  EXPECT_EQ(result.failed_, 1);
  EXPECT_TRUE(result.restoration_error_.empty());
  EXPECT_EQ(Count("Image"), 0);
  EXPECT_EQ(Count("FileImage"), 0);
}
TEST_F(ImportDuplicateSkippingTest, RestorationFailureIsReturnedAndStopsLaterAdmission) {
  Execute("DROP TABLE ImageSourceSize");
  const auto result = Run({source_, source_}, false);
  EXPECT_EQ(result.imported_, 0);
  EXPECT_EQ(result.failed_, 2);
  EXPECT_FALSE(result.restoration_error_.empty());
  EXPECT_TRUE(std::any_of(
      result.errors_.begin(), result.errors_.end(),
      [](const ImportError& error) { return error.code_ == ImportErrorCode::RESTORATION_FAILED; }));
  const auto next = Run({source_});
  EXPECT_EQ(next.failed_, 1);
  EXPECT_EQ(next.imported_, 0);
  EXPECT_FALSE(next.restoration_error_.empty());
}
TEST_F(ImportDuplicateSkippingTest, AllDuplicateJobCompletesWithZeroImportsAndFailures) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto result = Run(std::vector<image_path_t>(100, source_));
  EXPECT_EQ(result.imported_, 0);
  EXPECT_EQ(result.duplicates_, 100);
  EXPECT_EQ(result.failed_, 0);
  EXPECT_EQ(Count("FileImage"), 1);
}
TEST_F(ImportDuplicateSkippingTest, SourceBindingReplacementInvalidatesOnlyItsRecordedSize) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto id     = ImageId();
  const auto other  = Copy("other/image.jpg");
  const auto status = project_->GetImagePoolService()->Write<void>(
      id, [&](const auto& image) { image->image_path_ = other; });
  EXPECT_TRUE(status.failed_images_.empty());
  EXPECT_EQ(Count("ImageSourceSize"), 0);
  EXPECT_EQ(Run({other}).duplicates_, 1);
  EXPECT_EQ(Count("ImageSourceSize"), 1);
}
TEST_F(ImportDuplicateSkippingTest, ChangedSizeRefreshesEveryExistingSamePathEntry) {
  ASSERT_EQ(Run({source_, source_}, false).imported_, 2);
  Execute(
      "UPDATE ImageSourceSize SET byte_size = 1 WHERE image_id = "
      "(SELECT min(image_id) FROM ImageSourceSize)");
  const auto result = Run({source_});
  EXPECT_EQ(result.failed_, 0);
  EXPECT_EQ(result.updated_, 1);
  EXPECT_EQ(result.duplicates_, 0);
  ASSERT_EQ(result.source_changes_.size(), 1);
  EXPECT_EQ(result.source_changes_.front().entries.size(), 2);
  EXPECT_EQ(Count("FileImage"), 2);
  EXPECT_EQ(Count("ImageSourceSize WHERE byte_size = " +
                  std::to_string(std::filesystem::file_size(source_))),
            2);
}
TEST_F(ImportDuplicateSkippingTest,
       ChangedSourcePreservesCompleteHistoryMarksAndMembershipsAcrossReopen) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto element = last_job_->import_log_->Snapshot().created_.front().element_id_;
  {
    PipelineMgmtService pipelines(project_->GetStorage());
    const auto          history  = pipelines.LoadHistorySnapshot(element);
    auto                versions = *history.graph_;
    const auto          current  = pipelines.AcquireCommittedSnapshot(element);
    const auto          before =
        current->Document().PrimaryGrade()->FindAdjustmentByType(type_ids::Exposure())->ToJson();
    auto after           = before;
    after["exposure_ev"] = 1.0;
    SetParameterChange change;
    change.target.owner_kind             = PipelineParameterOwnerKind::ColorGrade;
    change.target.node_id                = NodeId{"grade.primary"};
    change.target.adjustment_instance_id = AdjustmentInstanceId{"grade.primary.exposure"};
    change.target.field_key              = "exposure";
    change.before_value                  = before;
    change.after_value                   = after;
    change.before_enabled = change.after_enabled = true;
    PipelineEditBatch batch;
    batch.operation_kind   = PipelineEditOperationKind::SetParameter;
    batch.presentation_key = "history.operation.set_parameter";
    batch.changes.push_back(std::move(change));
    auto commit =
        EditCommit::MakePipelineEdit(versions.GetRootId(), std::nullopt, std::move(batch));
    const auto head = commit.GetCommitHash();
    ASSERT_TRUE(versions.InsertCommit(std::move(commit)));
    versions.CreateVersionRefAtHead("Inactive edited version", head);
    // The active Version remains at root, as after Undo; the inactive Version retains the edit.
    pipelines.PersistHistory(history, versions);
    MaskAsset asset;
    asset.descriptor = {{2, 2}, {0, 0, 1, 1}};
    asset.pixels     = {0, 64, 128, 255};
    asset.key        = MakeMaskAssetKey(asset.descriptor, asset.pixels);
    pipelines.PutMaskAsset(asset);
  }
  const std::vector<std::string> tables{"Image",        "Element",    "FileImage",
                                        "PipelineRoot", "VersionRef", "ImageEditState",
                                        "EditCommit",   "MaskAsset",  "FolderContent"};
  std::vector<std::string>       before;
  for (const auto& table : tables) before.push_back(StoredRows(table));
  EXPECT_EQ(Run({source_}).duplicates_, 1);
  for (std::size_t i = 0; i < tables.size(); ++i)
    EXPECT_EQ(StoredRows(tables[i]), before[i]) << tables[i];
  std::ofstream(source_, std::ios::binary | std::ios::app) << 'x';
  const auto updated = Run({source_});
  EXPECT_EQ(updated.updated_, 1);
  EXPECT_EQ(updated.failed_, 0);
  Reopen();
  for (std::size_t i = 0; i < tables.size(); ++i)
    EXPECT_EQ(StoredRows(tables[i]), before[i]) << tables[i];
}
TEST_F(ImportDuplicateSkippingTest, DuplicateIntoAnotherAlbumPreservesExistingMemberships) {
  const auto fs = project_->GetSleeveService();
  ASSERT_TRUE(fs->CreateFolder(L"/", L"Album").second.success_);
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto result = Run({source_}, true, {}, L"/Album");
  EXPECT_EQ(result.duplicates_, 1);
  EXPECT_TRUE(fs->ListFolderEntries(L"/Album").empty());
  EXPECT_EQ(Count("FileImage"), 1);
}
TEST_F(ImportDuplicateSkippingTest, CompletionPersistsBothLibraryAndDestinationAlbumMemberships) {
  const auto fs = project_->GetSleeveService();
  ASSERT_TRUE(fs->CreateFolder(L"/", L"Album").second.success_);
  ASSERT_EQ(Run({source_}, true, {}, L"/Album").imported_, 1);
  const auto entries = fs->ListFolderEntries(L"/Album");
  ASSERT_EQ(entries.size(), 1);
  const auto id   = entries.front()->element_id_;
  const auto root = fs->ListFolderEntries(L"/");
  EXPECT_TRUE(std::any_of(root.begin(), root.end(),
                          [id](const auto& entry) { return entry->element_id_ == id; }));
  EXPECT_TRUE(fs->Read<bool>([](FileSystem& state) {
    return state.GetUnsyncedElements().empty() && state.GetModifiedElements().empty();
  }));
}
TEST_F(ImportDuplicateSkippingTest, DistinctPathThroughputWithBothPoliciesPreservesEveryEntry) {
  std::vector<image_path_t> paths;
  for (int i = 0; i < 128; ++i) {
    const auto path = root_ / ("image_" + std::to_string(i) + ".jpg");
    std::filesystem::create_hard_link(source_, path);
    paths.push_back(path);
  }
  const auto start = std::chrono::steady_clock::now();
  EXPECT_EQ(Run(paths, true).imported_, paths.size());
  const auto middle = std::chrono::steady_clock::now();
  EXPECT_EQ(Run(paths, false).imported_, paths.size());
  const auto end = std::chrono::steady_clock::now();
  EXPECT_EQ(Count("ImageSourceSize"), 256);
  RecordProperty(
      "skip_enabled_ms",
      std::to_string(
          std::chrono::duration_cast<std::chrono::milliseconds>(middle - start).count()));
  RecordProperty(
      "skip_disabled_ms",
      std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(end - middle).count()));
}
#ifdef _WIN32
TEST_F(ImportDuplicateSkippingTest, ReadFailureCreatesNoSizeOrProvisionalBinding) {
  const auto handle = CreateFileW(source_.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  ASSERT_NE(handle, INVALID_HANDLE_VALUE);
  const auto result = Run({source_});
  CloseHandle(handle);
  EXPECT_EQ(result.failed_, 1);
  ASSERT_EQ(result.errors_.size(), 1);
  EXPECT_EQ(result.errors_[0].code_, ImportErrorCode::READ_FAILED);
  EXPECT_EQ(Count("ImageSourceSize"), 0);
  EXPECT_EQ(Count("FileImage"), 0);
}
TEST_F(ImportDuplicateSkippingTest, ChangedSizeUpdatesWithoutReadingSourceContents) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  std::filesystem::resize_file(source_, 35 * 1024 * 1024);
  IO_COUNTERS before{}, after{};
  ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &before));
  const auto result = Run({source_});
  ASSERT_TRUE(GetProcessIoCounters(GetCurrentProcess(), &after));
  EXPECT_EQ(result.failed_, 0);
  EXPECT_EQ(result.updated_, 1);
  EXPECT_LT(after.ReadTransferCount - before.ReadTransferCount, 1024 * 1024);
  RecordProperty("read_bytes", std::to_string(after.ReadTransferCount - before.ReadTransferCount));
}
#endif

TEST_F(ImportDuplicateSkippingTest, FailedSourceSizeUpdateKeepsThePreviousSizeAndAllUserState) {
  ASSERT_EQ(Run({source_}).imported_, 1);
  const auto size = std::filesystem::file_size(source_);
  Execute("CREATE UNIQUE INDEX reject_repeated_size ON ImageSourceSize(byte_size)");
  // A second live entry with the replacement size makes the incoming update fail atomically.
  const auto other = Copy("other/image.jpg");
  std::ofstream(other, std::ios::binary | std::ios::app) << 'x';
  ASSERT_EQ(Run({other}).imported_, 1);
  const auto images = StoredRows("Image");
  const auto roots  = StoredRows("PipelineRoot");
  const auto sizes  = StoredRows("ImageSourceSize");
  std::ofstream(source_, std::ios::binary | std::ios::app) << 'x';
  const auto result = Run({source_});
  EXPECT_EQ(result.updated_, 0);
  EXPECT_EQ(result.failed_, 1);
  EXPECT_TRUE(result.source_changes_.empty());
  EXPECT_EQ(StoredRows("ImageSourceSize"), sizes);
  EXPECT_EQ(StoredRows("Image"), images);
  EXPECT_EQ(StoredRows("PipelineRoot"), roots);
  EXPECT_EQ(Count("ImageSourceSize WHERE byte_size = " + std::to_string(size)), 1);
  EXPECT_EQ(Count("PipelineRoot"), 2);
}
}  // namespace
}  // namespace alcedo
