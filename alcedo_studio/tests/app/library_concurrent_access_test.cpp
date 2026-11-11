//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Storage concurrent access (#809). Library reads and writes run on several threads at the same
// time for a fixed time, through the production services that the app calls from its worker
// threads:
// - writes: file creation as the import does it (pinned pool Image, file element, history root),
//   ratings and color labels through the image pool (Write_NoSync, then SyncWithStorage, as
//   ImageController does), library Paste commits (PersistHistory), Mask assets, and album
//   membership (AlbumBrowseService);
// - reads: the album query with groups and statistics, the folder photo counts, the project
//   overview, the library search page, and the display rows of known files.
// Each read must agree with itself (counts, rows, and statistics of one statement or one
// transaction), and the photo counts never decrease, since no thread removes a file. After the
// run the storage invariant check (#805) finds no broken row, and the project, opened again,
// holds the last value that each writer wrote.
//
// Run the test with the AddressSanitizer build on Windows (preset win_debug_asan) and with the
// ThreadSanitizer build on macOS (preset macos_debug_tsan).

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <format>
#include <functional>
#include <iostream>
#include <latch>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/adjustment_transfer_service.hpp"
#include "app/album_browse_service.hpp"
#include "app/image_pool_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "app/sleeve_filter_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/mask/mask_asset.hpp"
#include "image/color_label.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/album_query.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/storage_invariants.hpp"
#include "support/test_temp_directory.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
namespace {

using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;
using Clock = std::chrono::steady_clock;

// A sanitizer build runs the steps about ten times slower, so it runs longer to reach as many
// interleavings.
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ALCEDO_TEST_SANITIZER_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define ALCEDO_TEST_SANITIZER_BUILD 1
#endif
#endif
#ifdef ALCEDO_TEST_SANITIZER_BUILD
constexpr auto kRunDuration = std::chrono::seconds(60);
#else
constexpr auto kRunDuration = std::chrono::seconds(5);
#endif
constexpr int    kInitialFileCount     = 64;
constexpr int    kAlbumCount           = 2;
constexpr int    kCreatedFilesPerBatch = 3;
constexpr size_t kMaxRecordedFailures  = 20;
constexpr int    kAlbumQueryPageRows   = 200;
constexpr auto   kPauseBetweenSteps    = std::chrono::milliseconds(1);

/// Failures of the worker threads. The test body reports them after the threads end, so no
/// gtest assertion runs on a worker thread.
class ThreadFailures {
 public:
  void Add(std::string message) {
    std::lock_guard lock(mutex_);
    ++count_;
    if (messages_.size() < kMaxRecordedFailures) {
      messages_.push_back(std::move(message));
    }
  }

  [[nodiscard]] auto Count() const -> size_t {
    std::lock_guard lock(mutex_);
    return count_;
  }

  [[nodiscard]] auto Text() const -> std::string {
    std::lock_guard lock(mutex_);
    std::string     text = std::format("{} failures; the first {}:\n", count_, messages_.size());
    for (const auto& message : messages_) {
      text += message + "\n";
    }
    return text;
  }

 private:
  mutable std::mutex       mutex_;
  std::vector<std::string> messages_;
  size_t                   count_ = 0;
};

/// One worker thread: @p step runs again until the deadline. A step that throws records a
/// failure and the loop goes on.
struct Worker {
  std::string           name;
  std::function<void()> step;
  int64_t               steps = 0;
  /// Longest step: a long wait for the database lock shows here.
  Clock::duration       longest_step{};
};

class LibraryConcurrentAccessTest : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;

  void                    SetUp() override {
    TimeProvider::Refresh();
    OpenProject();

    std::vector<SyntheticImageSpec> specs;
    for (int i = 0; i < kInitialFileCount; ++i) {
      const auto name = std::format(L"DSC_{:04}.jpg", i);
      specs.push_back(SyntheticImageSpec{
                             .file_name_   = name,
                             .image_path_  = L"D:/library/" + name,
                             .make_        = "Make",
                             .model_       = i % 2 == 0 ? "Canon EOS R5" : "X100V",
                             .lens_        = i % 3 == 0 ? "RF50mm F1.2 L USM" : "",
                             .date_time_   = std::format("2026:06:{:02} 10:11:12", 1 + i % 28),
                             .iso_         = 100,
                             .rating_      = i % 6,
                             .is_raw_file_ = false,
      });
    }
    initial_ids_ = SyntheticLibraryBuilder(*project_).AddFiles(specs);
    ASSERT_EQ(initial_ids_.size(), static_cast<size_t>(kInitialFileCount));
    for (const auto id : initial_ids_) {
      pipelines_->InitializeImageRoot(id, CreateDefaultPipelineDocument(), nullptr);
    }
    for (const auto& row : Browse().ReadAlbumFileRows(initial_ids_)) {
      image_ids_[row.file_id_] = row.image_id_;
      ratings_[row.file_id_]   = row.rating_;
      labels_[row.file_id_]    = row.color_labels_;
    }
    ASSERT_EQ(image_ids_.size(), initial_ids_.size());
    for (int i = 0; i < kAlbumCount; ++i) {
      const auto folder = Browse().CreateFolder(L"/", std::format(L"Album {}", i));
      ASSERT_TRUE(folder.has_value());
      albums_.emplace(folder->folder_id_, std::set<sl_element_id_t>{});
    }
    for (const auto id : initial_ids_) {
      initial_version_counts_[id] =
          pipelines_->LoadHistorySnapshot(id).graph_->GetAllVersionRefs().size();
    }
    test::ExpectStorageInvariants(*project_->GetStorage());
  }

  void TearDown() override {
    pipelines_.reset();
    project_.reset();
  }

  void OpenProject() {
    pipelines_.reset();
    project_.reset();
    project_ =
        std::make_unique<ProjectService>(temp_dir_ / "library.db", temp_dir_ / "library.json");
    pipelines_ = std::make_unique<PipelineMgmtService>(project_->GetStorage());
  }

  auto Browse() -> AlbumBrowseService& { return *project_->GetAlbumBrowseService(); }

  /// Change one image in the pool and write it, as ImageController does.
  void WriteImage(image_id_t image_id, std::function<void(std::shared_ptr<Image>)> change) {
    auto image_pool = project_->GetImagePoolService();
    image_pool->Write_NoSync<void>(image_id, std::move(change));
    const auto status = image_pool->SyncWithStorage();
    if (!status.failed_images_.empty()) {
      throw std::runtime_error(std::format("sync of image {} failed: {}", image_id,
                                           status.failed_images_.front().message_));
    }
  }

  // -- Writers -------------------------------------------------------------------------------

  /// Create files as the import does: Image rows from pinned pool images, file elements in the
  /// library root, and their history roots.
  auto CreateFilesStep() -> std::function<void()> {
    // The builder makes a large DNG profile once; the thread keeps it for the run.
    return [this, builder = std::make_shared<SyntheticLibraryBuilder>(*project_)] {
      std::vector<SyntheticImageSpec> specs;
      for (int i = 0; i < kCreatedFilesPerBatch; ++i) {
        const auto name = std::format(L"IMG_C{:05}.jpg", next_created_name_++);
        specs.push_back(SyntheticImageSpec{.file_name_   = name,
                                           .image_path_  = L"D:/library/" + name,
                                           .model_       = "NIKON Z 8",
                                           .date_time_   = "2026:07:01 08:00:00",
                                           .rating_      = 1,
                                           .is_raw_file_ = false});
      }
      const auto ids = builder->AddFiles(specs);
      if (ids.size() != specs.size()) {
        throw std::runtime_error(std::format("{} files created of {}", ids.size(), specs.size()));
      }
      for (const auto id : ids) {
        pipelines_->InitializeImageRoot(id, CreateDefaultPipelineDocument(), nullptr);
      }
      created_ids_.insert(created_ids_.end(), ids.begin(), ids.end());
    };
  }

  auto SetRatingsStep() -> std::function<void()> {
    return [this, rng = std::mt19937_64{11}]() mutable {
      const auto id     = initial_ids_[rng() % initial_ids_.size()];
      const int  rating = static_cast<int>(rng() % 6);
      WriteImage(image_ids_.at(id), [rating](const std::shared_ptr<Image>& image) {
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
      ratings_[id] = rating;
    };
  }

  auto SetColorLabelsStep() -> std::function<void()> {
    return [this, rng = std::mt19937_64{12}]() mutable {
      const auto id   = initial_ids_[rng() % initial_ids_.size()];
      const auto code = static_cast<ColorLabel>(rng() % kColorLabelCount);
      const bool on   = rng() % 2 == 0;
      WriteImage(image_ids_.at(id), [code, on](const std::shared_ptr<Image>& image) {
        image->SetColorLabel(code, on);
      });
      labels_[id] = WithColorLabel(labels_[id], code, on);
    };
  }

  /// Paste an exposure adjustment as a new Version: the library Paste of
  /// AdjustmentTransferApplyCoordinator (stored history, private copy, one persist).
  auto PasteCommitsStep() -> std::function<void()> {
    return [this, rng = std::mt19937_64{13}]() mutable {
      const auto  id   = initial_ids_[rng() % initial_ids_.size()];
      const auto  base = pipelines_->LoadHistorySnapshot(id);
      CommitGraph graph{*base.graph_};
      const auto  pasted = AdjustmentTransferService::PasteAsRootRelativeVersion(
          graph, base.root_->document,
          test::MakeExposureTransferPackage(0.25 * static_cast<double>(1 + rng() % 8)), "Pasted");
      if (!pasted.pasted) {
        throw std::runtime_error("paste: " + pasted.error);
      }
      if (pipelines_->PersistHistory(base, graph) == nullptr) {
        throw std::runtime_error(std::format("persist of the paste on file {} failed", id));
      }
      ++pastes_[id];
    };
  }

  /// Write a Mask asset and read it back, as an AI Mask edit and the executor of a render do.
  auto MaskAssetsStep() -> std::function<void()> {
    return [this] {
      const auto value = next_mask_value_++;
      MaskAsset  asset;
      asset.descriptor.extent           = {4, 4};
      asset.descriptor.reference_bounds = NormalizedRect{0.25f, 0.25f, 0.5f, 0.5f};
      asset.pixels.resize(16);
      for (size_t i = 0; i < asset.pixels.size(); ++i) {
        asset.pixels[i] = static_cast<std::uint8_t>((value >> (i % 4 * 8)) & 0xff);
      }
      asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
      pipelines_->PutMaskAsset(asset);
      if (pipelines_->ReadMaskAsset(asset.key).pixels != asset.pixels) {
        throw std::runtime_error("the stored Mask asset has other pixels");
      }
      mask_assets_.push_back(std::move(asset));
    };
  }

  /// Add a file to an album or take it out.
  auto AlbumMembershipStep() -> std::function<void()> {
    return [this, rng = std::mt19937_64{14}]() mutable {
      auto album = albums_.begin();
      std::advance(album, static_cast<long>(rng() % albums_.size()));
      const auto id = initial_ids_[rng() % initial_ids_.size()];
      if (album->second.contains(id)) {
        const auto result = Browse().DeleteFilesInFolderByElementIds(album->first, {id});
        if (!result.failed_element_ids_.empty()) {
          throw std::runtime_error(std::format("remove of {} from an album failed", id));
        }
        album->second.erase(id);
      } else {
        const auto result = Browse().LinkFilesToFolder({id}, album->first);
        if (!result.failed_element_ids_.empty()) {
          throw std::runtime_error(std::format("link of {} to an album failed", id));
        }
        album->second.insert(id);
      }
    };
  }

  // -- Readers -------------------------------------------------------------------------------

  /// The album query of the library root and of the albums, with groups and statistics. The
  /// counts, rows, groups, and statistics of one read come from one transaction, so they agree.
  auto AlbumQueryStep() -> std::function<void()> {
    return [this, rng = std::mt19937_64{21}, last_root_count = int64_t{0}]() mutable {
      static const std::vector<AlbumQueryOptions> kOptions{
          {},
          {.sort_field_ = AlbumSortField::kCaptureTime, .group_field_ = AlbumGroupField::kRating},
          {.sort_field_     = AlbumSortField::kRating,
           .sort_direction_ = SortDirection::kDescending,
           .group_field_    = AlbumGroupField::kCameraModel},
          {.sort_field_ = AlbumSortField::kEditTime, .group_field_ = AlbumGroupField::kCaptureDay},
      };
      const auto&               options = kOptions[rng() % kOptions.size()];
      std::optional<FilterNode> filter;
      if (rng() % 3 == 0) {
        filter =
            sleeve_filter::BuildColorLabelFilter(static_cast<ColorLabel>(rng() % kColorLabelCount));
      }
      sl_element_id_t scope = 0;
      if (rng() % 3 == 0) {
        auto album = albums_.begin();
        std::advance(album, static_cast<long>(rng() % albums_.size()));
        scope = album->first;
      }
      const auto result =
          Browse().ReadAlbumQuery(scope, filter, options, "",
                                  AlbumQueryRead{.offset_                   = 0,
                                                 .limit_                    = kAlbumQueryPageRows,
                                                 .read_groups_              = true,
                                                 .read_statistics_          = true,
                                                 .read_occurrence_file_ids_ = true});
      const auto describe = [&] {
        return std::format("album query of folder {} (sort {}, group {}, filter {})", scope,
                           static_cast<int>(options.sort_field_),
                           static_cast<int>(options.group_field_), filter.has_value());
      };
      if (static_cast<int64_t>(result.occurrence_file_ids_.size()) != result.occurrence_count_) {
        throw std::runtime_error(std::format("{}: {} occurrence ids, occurrence count {}",
                                             describe(), result.occurrence_file_ids_.size(),
                                             result.occurrence_count_));
      }
      const std::set<sl_element_id_t> unique(result.occurrence_file_ids_.begin(),
                                             result.occurrence_file_ids_.end());
      if (static_cast<int64_t>(unique.size()) != result.unique_file_count_) {
        throw std::runtime_error(std::format("{}: {} unique occurrence ids, file count {}",
                                             describe(), unique.size(), result.unique_file_count_));
      }
      if (!result.statistics_.has_value() ||
          result.statistics_->total_photo_count_ != result.unique_file_count_) {
        throw std::runtime_error(
            std::format("{}: statistics count {}, file count {}", describe(),
                        result.statistics_ ? result.statistics_->total_photo_count_ : -1,
                        result.unique_file_count_));
      }
      const auto expected_rows = std::min<int64_t>(kAlbumQueryPageRows, result.occurrence_count_);
      if (static_cast<int64_t>(result.rows_.size()) != expected_rows) {
        throw std::runtime_error(std::format("{}: {} rows, expected {}", describe(),
                                             result.rows_.size(), expected_rows));
      }
      for (size_t i = 0; i < result.rows_.size(); ++i) {
        if (result.rows_[i].photo_.file_id_ != result.occurrence_file_ids_[i]) {
          throw std::runtime_error(std::format("{}: row {} is file {}, occurrence {}", describe(),
                                               i, result.rows_[i].photo_.file_id_,
                                               result.occurrence_file_ids_[i]));
        }
      }
      if (scope == 0 && !filter.has_value()) {
        // No thread removes a file: the library photo count never decreases.
        if (result.unique_file_count_ < last_root_count ||
            result.unique_file_count_ < kInitialFileCount) {
          throw std::runtime_error(std::format("{}: {} files after {}", describe(),
                                               result.unique_file_count_, last_root_count));
        }
        last_root_count = result.unique_file_count_;
      }
    };
  }

  /// The folder photo counts, the project overview, and the library search page.
  auto CountsAndSearchStep() -> std::function<void()> {
    return [this, last_project_count = uint64_t{0}, last_search_total = size_t{0}]() mutable {
      const auto counts = Browse().ReadFolderImageCounts();
      if (counts.project_photo_count_ < last_project_count ||
          counts.project_photo_count_ < static_cast<uint64_t>(kInitialFileCount)) {
        throw std::runtime_error(std::format("project photo count {} after {}",
                                             counts.project_photo_count_, last_project_count));
      }
      last_project_count  = counts.project_photo_count_;

      const auto overview = Browse().ReadProjectOverview();
      if (overview.photo_count_ < static_cast<uint64_t>(kInitialFileCount) ||
          overview.edited_photo_count_ > overview.photo_count_) {
        throw std::runtime_error(std::format("overview: {} photos, {} edited",
                                             overview.photo_count_, overview.edited_photo_count_));
      }

      // The search page and its total come from one statement. Only created files match, and
      // no thread renames or removes one.
      const auto page = project_->GetSleeveFilterService()->SearchFolderPage(0, L"IMG_C", 0, 20);
      if (page.rows_.size() != std::min<size_t>(20, page.total_) ||
          page.total_ < last_search_total) {
        throw std::runtime_error(std::format("search page: {} rows, total {} after {}",
                                             page.rows_.size(), page.total_, last_search_total));
      }
      for (const auto& row : page.rows_) {
        if (!row.file_name_.starts_with("IMG_C")) {
          throw std::runtime_error("search page row " + row.file_name_);
        }
      }
      last_search_total = page.total_;
    };
  }

  /// The display rows of the files that exist before the run, and the last edited file.
  auto FileRowsStep() -> std::function<void()> {
    return [this] {
      const auto rows = Browse().ReadAlbumFileRows(initial_ids_);
      if (rows.size() != initial_ids_.size()) {
        throw std::runtime_error(
            std::format("{} display rows of {} files", rows.size(), initial_ids_.size()));
      }
      for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].file_id_ != initial_ids_[i] ||
            rows[i].image_id_ != image_ids_.at(initial_ids_[i]) || rows[i].rating_ < 0 ||
            rows[i].rating_ > 5) {
          throw std::runtime_error(std::format("display row {}: file {} image {} rating {}", i,
                                               rows[i].file_id_, rows[i].image_id_,
                                               rows[i].rating_));
        }
      }
      (void)Browse().ReadLastEditedFile();
    };
  }

  // -- Checks after the run ------------------------------------------------------------------

  /// The stored rows hold the last value of each writer.
  void ExpectStoredValuesOfTheWriters() {
    const auto rows = Browse().ReadAlbumFileRows(initial_ids_);
    ASSERT_EQ(rows.size(), initial_ids_.size());
    for (const auto& row : rows) {
      EXPECT_EQ(row.rating_, ratings_.at(row.file_id_)) << "rating of file " << row.file_id_;
      EXPECT_EQ(row.color_labels_, labels_.at(row.file_id_))
          << "color labels of file " << row.file_id_;
    }

    const auto created_rows = Browse().ReadAlbumFileRows(created_ids_);
    EXPECT_EQ(created_rows.size(), created_ids_.size());
    EXPECT_EQ(Browse().ReadFolderImageCounts().project_photo_count_,
              initial_ids_.size() + created_ids_.size());

    for (const auto& [album_id, members] : albums_) {
      std::set<sl_element_id_t> stored;
      for (const auto& file : Browse().ListFilesInFolderById(album_id)) {
        stored.insert(file.file_id_);
      }
      EXPECT_EQ(stored, members) << "files of album " << album_id;
    }

    for (const auto id : initial_ids_) {
      const auto history = pipelines_->LoadHistorySnapshot(id);
      ASSERT_TRUE(history.graph_) << "history of file " << id;
      const auto pastes = pastes_.contains(id) ? pastes_.at(id) : 0;
      EXPECT_EQ(history.graph_->GetAllVersionRefs().size(),
                initial_version_counts_.at(id) + static_cast<size_t>(pastes))
          << "Versions of file " << id;
    }

    for (const auto& asset : mask_assets_) {
      EXPECT_EQ(pipelines_->ReadMaskAsset(asset.key).pixels, asset.pixels);
    }
  }

  std::unique_ptr<ProjectService>                      project_;
  std::unique_ptr<PipelineMgmtService>                 pipelines_;
  std::vector<sl_element_id_t>                         initial_ids_;
  std::map<sl_element_id_t, image_id_t>                image_ids_;
  std::map<sl_element_id_t, size_t>                    initial_version_counts_;
  std::map<sl_element_id_t, std::set<sl_element_id_t>> albums_;

  // Each value below has one writer thread during the run.
  std::map<sl_element_id_t, int>                       ratings_;
  std::map<sl_element_id_t, ColorLabelMask>            labels_;
  std::map<sl_element_id_t, int>                       pastes_;
  std::vector<sl_element_id_t>                         created_ids_;
  std::vector<MaskAsset>                               mask_assets_;
  int                                                  next_created_name_ = 0;
  std::uint32_t                                        next_mask_value_   = 1;
};

TEST_F(LibraryConcurrentAccessTest, ConcurrentReadsAndWritesKeepStorageInvariants) {
  std::vector<Worker> workers;
  workers.push_back({"create files", CreateFilesStep()});
  workers.push_back({"ratings", SetRatingsStep()});
  workers.push_back({"color labels", SetColorLabelsStep()});
  workers.push_back({"paste commits", PasteCommitsStep()});
  workers.push_back({"Mask assets", MaskAssetsStep()});
  workers.push_back({"album membership", AlbumMembershipStep()});
  workers.push_back({"album query", AlbumQueryStep()});
  workers.push_back({"album query 2", AlbumQueryStep()});
  workers.push_back({"counts and search", CountsAndSearchStep()});
  workers.push_back({"file rows", FileRowsStep()});

  ThreadFailures    failures;
  // Every thread starts its steps at the same time. The main thread sets the deadline before it
  // arrives at the latch, and each worker reads it after the latch.
  std::latch        start(static_cast<std::ptrdiff_t>(workers.size()) + 1);
  Clock::time_point deadline;
  {
    // std::thread with explicit joins: Apple libc++ has no std::jthread.
    std::vector<std::thread> threads;
    threads.reserve(workers.size());
    for (auto& worker : workers) {
      threads.emplace_back([&worker, &failures, &start, &deadline] {
        start.arrive_and_wait();
        while (Clock::now() < deadline) {
          try {
            const auto step_start = Clock::now();
            worker.step();
            worker.longest_step = std::max(worker.longest_step, Clock::now() - step_start);
            ++worker.steps;
          } catch (const std::exception& e) {
            failures.Add(std::format("{}: {}", worker.name, e.what()));
          }
          std::this_thread::sleep_for(kPauseBetweenSteps);
        }
      });
    }
    deadline = Clock::now() + kRunDuration;
    start.arrive_and_wait();
    for (auto& thread : threads) {
      thread.join();
    }
  }

  std::string step_counts;
  for (const auto& worker : workers) {
    step_counts += std::format(
        "{} {} (longest {} ms); ", worker.name, worker.steps,
        std::chrono::duration_cast<std::chrono::milliseconds>(worker.longest_step).count());
    EXPECT_GT(worker.steps, 0) << worker.name << " finished no step";
  }
  std::cout << "Steps in " << kRunDuration.count() << " s: " << step_counts << "\n";
  EXPECT_EQ(failures.Count(), 0u) << failures.Text();

  test::ExpectStorageInvariants(*project_->GetStorage());
  ExpectStoredValuesOfTheWriters();

  // The stored rows alone give the same values.
  OpenProject();
  SCOPED_TRACE("after the project is opened again");
  test::ExpectStorageInvariants(*project_->GetStorage());
  ExpectStoredValuesOfTheWriters();
}

}  // namespace
}  // namespace alcedo
