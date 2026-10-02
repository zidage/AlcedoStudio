//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Storage reads of the library sort, group, and import-time feature
// (album_sort_group_and_import_time_plan.md, Phase 1). Every case runs against a persisted
// project with typed metadata, written through the image pool and Sleeve services.

#include "sleeve/album_query.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <variant>
#include <vector>

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"
#include "storage/mapper/sleeve/element/element_mapper.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "support/non_raw_import_files.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/import/import_log.hpp"

namespace alcedo {
namespace {

using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

constexpr const char* kActiveModel   = "model-active";
constexpr const char* kInactiveModel = "model-inactive";
constexpr const char* kNewYork       = "America/New_York";

/// Unix seconds of a UTC wall time.
auto Utc(int year, unsigned month, unsigned day, int hour = 0, int minute = 0, int second = 0)
    -> std::time_t {
  using namespace std::chrono;
  const auto time_point = sys_days{std::chrono::year{year} / month / day} + hours{hour} +
                          minutes{minute} + seconds{second};
  return static_cast<std::time_t>(time_point.time_since_epoch().count());
}

void RunStatement(ProjectService& project, const std::string& sql) {
  auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  ASSERT_EQ(duckdb_query(guard.conn_, sql.c_str(), &result), DuckDBSuccess)
      << sql << ": " << duckdb_result_error(&result);
  duckdb_destroy_result(&result);
}

auto QueryText(ProjectService& project, const std::string& sql) -> std::string {
  auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    ADD_FAILURE() << sql << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return {};
  }
  std::string text = "<null>";
  if (duckdb_row_count(&result) > 0 && !duckdb_value_is_null(&result, 0, 0)) {
    char* value = duckdb_value_varchar(&result, 0, 0);
    text        = value;
    duckdb_free(value);
  }
  duckdb_destroy_result(&result);
  return text;
}

auto CompileFilter(const FilterNode& node) -> std::optional<duckorm::SqlFragment> {
  return CompileFilterPredicate(node);
}

auto RowIds(const AlbumQueryResult& result) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const auto& row : result.rows_) {
    ids.push_back(row.photo_.file_id_);
  }
  return ids;
}

auto KeyText(const AlbumGroupKey& key) -> std::string {
  if (std::holds_alternative<std::monostate>(key)) {
    return "<unknown>";
  }
  if (const auto* number = std::get_if<int64_t>(&key)) {
    return std::to_string(*number);
  }
  return std::get<std::string>(key);
}

/// One fixture photo with the values the expected orders are computed from.
struct FixturePhoto {
  SyntheticImageSpec spec_;
  std::string        capture_at_;  ///< `YYYY-MM-DD HH:MM:SS`; empty when unknown.
};

/// Eight photos with repeated timestamps, unknown fields, uneven groups, and equal ratings.
auto FixturePhotos() -> std::vector<FixturePhoto> {
  const auto photo = [](const wchar_t* name, std::string camera, std::string lens,
                        std::string capture, int rating, std::time_t added) {
    return FixturePhoto{
        .spec_       = SyntheticImageSpec{.file_name_   = name,
                                          .image_path_  = std::wstring(L"D:/photos/") + name,
                                          .make_        = "Maker",
                                          .model_       = camera,
                                          .lens_        = lens,
                                          .date_time_   = capture,
                                          .rating_      = rating,
                                          .is_raw_file_ = false,
                                          .added_time_  = added},
        .capture_at_ = capture};
  };
  return {
      photo(L"a.ARW", "Canon R5", "RF 50", "2026-06-07 09:00:00", 3, Utc(2026, 9, 1, 10)),
      photo(L"b.ARW", "Canon R5", "", "2026-06-07 18:00:00", 5, Utc(2026, 9, 1, 10)),
      photo(L"c.ARW", "Nikon Z8", "Z 24-70", "2026-06-07 18:00:00", 3, Utc(2026, 9, 2, 3)),
      photo(L"d.ARW", "", "", "", 0, Utc(2026, 9, 2, 3)),
      photo(L"e.ARW", "Nikon Z8", "Z 24-70", "2026-06-06 23:30:00", 5, Utc(2026, 9, 2, 15)),
      photo(L"f.ARW", "Sony A7", "FE 35", "2026-06-05 12:00:00", 1, Utc(2026, 8, 30, 8)),
      photo(L"g.ARW", "Canon R5", "RF 50", "2026-06-06 08:00:00", 3, Utc(2026, 8, 30, 8)),
      photo(L"h.ARW", "", "FE 35", "", 3, Utc(2026, 9, 1, 23, 59, 59)),
  };
}

class AlbumQueryTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    const auto* test_name = ::testing::UnitTest::GetInstance()->current_test_info()->name();
    const auto  dir       = std::filesystem::temp_directory_path();
    db_path_              = dir / (std::string("album_query_") + test_name + ".db");
    meta_path_            = dir / (std::string("album_query_") + test_name + ".json");
    scratch_dir_          = dir / (std::string("album_query_") + test_name);
    RemoveProjectFiles();
    std::filesystem::create_directories(scratch_dir_);
  }
  void TearDown() override { RemoveProjectFiles(); }

  void RemoveProjectFiles() {
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    std::filesystem::remove(db_path_.string() + ".wal", ec);
    std::filesystem::remove(meta_path_, ec);
    std::filesystem::remove_all(scratch_dir_, ec);
  }

  /// Build the eight fixture photos and remember their file ids in fixture order.
  void BuildFixture(ProjectService& project) {
    photos_ = FixturePhotos();
    std::vector<SyntheticImageSpec> specs;
    for (const auto& photo : photos_) {
      specs.push_back(photo.spec_);
    }
    SyntheticLibraryBuilder builder(project);
    ids_ = builder.AddFiles(specs);
    ASSERT_EQ(ids_.size(), photos_.size());
    for (size_t index = 0; index < ids_.size(); ++index) {
      index_of_[ids_[index]] = index;
    }
  }

  auto Store(ProjectService& project) -> ElementStore& {
    return project.GetStorage()->GetElementStore();
  }

  auto ReadAll(ProjectService& project, const AlbumQueryOptions& options,
               const std::optional<duckorm::SqlFragment>& filter = std::nullopt,
               sl_element_id_t folder_id = 0, const std::string& model = kActiveModel)
      -> AlbumQueryResult {
    return Store(project).ReadAlbumQuery(
        folder_id, filter, options, model,
        AlbumQueryRead{
            .offset_ = 0, .limit_ = 1000, .read_groups_ = true, .read_statistics_ = true});
  }

  /// Read the whole stream in pages of @p page_size and concatenate the ids.
  auto ReadInPages(ProjectService& project, const AlbumQueryOptions& options, int64_t page_size)
      -> std::vector<sl_element_id_t> {
    std::vector<sl_element_id_t> ids;
    for (int64_t offset = 0;; offset += page_size) {
      const auto page =
          Store(project).ReadAlbumQuery(0, std::nullopt, options, kActiveModel,
                                        AlbumQueryRead{.offset_ = offset, .limit_ = page_size});
      EXPECT_EQ(page.first_occurrence_, offset);
      if (page.rows_.empty()) {
        break;
      }
      for (const auto& row : page.rows_) {
        ids.push_back(row.photo_.file_id_);
      }
    }
    return ids;
  }

  auto Photo(sl_element_id_t id) const -> const FixturePhoto& { return photos_[index_of_.at(id)]; }

  std::filesystem::path             db_path_;
  std::filesystem::path             meta_path_;
  std::filesystem::path             scratch_dir_;
  std::vector<FixturePhoto>         photos_;
  std::vector<sl_element_id_t>      ids_;
  std::map<sl_element_id_t, size_t> index_of_;
};

/// The sort value of one fixture photo as an optional comparable text (NULL when missing).
auto SortValue(const FixturePhoto& photo, AlbumSortField field) -> std::optional<std::string> {
  const auto non_empty = [](const std::string& value) -> std::optional<std::string> {
    return value.empty() ? std::nullopt : std::optional<std::string>(value);
  };
  switch (field) {
    case AlbumSortField::kCaptureTime:
      return non_empty(photo.capture_at_);
    case AlbumSortField::kImportTime: {
      char buffer[32];
      std::snprintf(buffer, sizeof(buffer), "%020lld",
                    static_cast<long long>(*photo.spec_.added_time_));
      return std::string(buffer);
    }
    case AlbumSortField::kCameraModel:
      return non_empty(photo.spec_.model_);
    case AlbumSortField::kLens:
      return non_empty(photo.spec_.lens_);
    case AlbumSortField::kRating:
      return std::to_string(photo.spec_.rating_);
    default:
      return std::nullopt;
  }
}

/// Expected flat order: the sort value in @p direction with NULLs last, then the file id.
auto ExpectedFlatOrder(const std::vector<sl_element_id_t>&                        ids,
                       const std::function<const FixturePhoto&(sl_element_id_t)>& photo,
                       AlbumSortField field, SortDirection direction)
    -> std::vector<sl_element_id_t> {
  auto expected = ids;
  std::sort(expected.begin(), expected.end(), [&](sl_element_id_t left, sl_element_id_t right) {
    const auto left_value  = SortValue(photo(left), field);
    const auto right_value = SortValue(photo(right), field);
    if (left_value.has_value() != right_value.has_value()) {
      return left_value.has_value();
    }
    if (left_value.has_value() && *left_value != *right_value) {
      return direction == SortDirection::kAscending ? *left_value < *right_value
                                                    : *left_value > *right_value;
    }
    return left < right;
  });
  return expected;
}

TEST_F(AlbumQueryTest, SortedPagesCoverAllMatchingFilesWithoutDuplicates) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto photo = [this](sl_element_id_t id) -> const FixturePhoto& { return Photo(id); };

  for (const auto field :
       {AlbumSortField::kCaptureTime, AlbumSortField::kImportTime, AlbumSortField::kCameraModel,
        AlbumSortField::kLens, AlbumSortField::kRating}) {
    for (const auto direction : {SortDirection::kAscending, SortDirection::kDescending}) {
      const AlbumQueryOptions options{.sort_field_ = field, .sort_direction_ = direction};
      const auto              expected = ExpectedFlatOrder(ids_, photo, field, direction);
      EXPECT_EQ(RowIds(ReadAll(project, options)), expected)
          << "field " << static_cast<int>(field) << " direction " << static_cast<int>(direction);
      // Pages of 3 split groups of equal values; the stream stays the same.
      const auto paged = ReadInPages(project, options, 3);
      EXPECT_EQ(paged, expected);
      EXPECT_EQ(std::set<sl_element_id_t>(paged.begin(), paged.end()).size(), ids_.size());
    }
  }
}

TEST_F(AlbumQueryTest, NoExplicitGroupOrSortKeepsFileIdOrder) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto result = ReadAll(project, AlbumQueryOptions{});
  auto       sorted = ids_;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(RowIds(result), sorted);
  EXPECT_TRUE(result.groups_.empty());
  EXPECT_EQ(result.unique_file_count_, 8);
  EXPECT_EQ(result.occurrence_count_, 8);
  for (const auto& row : result.rows_) {
    EXPECT_TRUE(std::holds_alternative<std::monostate>(row.group_key_));
  }
}

TEST_F(AlbumQueryTest, ScalarGroupsCountEveryFilteredFileOnce) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  // Rating 3 or above: every photo except d (0) and f (1).
  FilterNode rating_filter{
      FilterNode::Type::Condition,
      {},
      {},
      FieldCondition{
          .field_ = FilterField::Rating, .op_ = CompareOp::GREATER_EQUAL, .value_ = int64_t{3}},
      std::nullopt};
  const auto filter = CompileFilter(rating_filter);

  const std::map<AlbumGroupField, std::vector<std::pair<std::string, int64_t>>> expected{
      {AlbumGroupField::kCameraModel, {{"Canon R5", 3}, {"Nikon Z8", 2}, {"<unknown>", 1}}},
      {AlbumGroupField::kLens, {{"FE 35", 1}, {"RF 50", 2}, {"Z 24-70", 2}, {"<unknown>", 1}}},
      {AlbumGroupField::kRating, {{"5", 2}, {"3", 4}}},
      {AlbumGroupField::kCaptureDay, {{"2026-06-07", 3}, {"2026-06-06", 2}, {"<unknown>", 1}}},
  };
  for (const auto& [field, groups] : expected) {
    const auto result = ReadAll(project, AlbumQueryOptions{.group_field_ = field}, filter);
    EXPECT_EQ(result.unique_file_count_, 6);
    EXPECT_EQ(result.occurrence_count_, 6);
    std::vector<std::pair<std::string, int64_t>> actual;
    int64_t                                      expected_first = 0;
    for (const auto& group : result.groups_) {
      actual.emplace_back(KeyText(group.key_), group.photo_count_);
      EXPECT_EQ(group.first_occurrence_, expected_first);
      expected_first += group.photo_count_;
    }
    EXPECT_EQ(actual, groups) << "group field " << static_cast<int>(field);
    EXPECT_EQ(expected_first, result.unique_file_count_);
    // Every row carries the key of the group whose range holds it.
    size_t group_index = 0;
    for (size_t row = 0; row < result.rows_.size(); ++row) {
      while (static_cast<int64_t>(row) >= result.groups_[group_index].first_occurrence_ +
                                              result.groups_[group_index].photo_count_) {
        ++group_index;
      }
      EXPECT_EQ(KeyText(result.rows_[row].group_key_), KeyText(result.groups_[group_index].key_));
    }
  }
}

TEST_F(AlbumQueryTest, LabelGroupsUseAllCanonicalAssignmentsFromActiveModel) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto insert_label = [&project](sl_element_id_t file_id, const char* model,
                                       const std::string& label) {
    RunStatement(project,
                 "INSERT INTO SemanticImageLabel (file_id, model_key, label, score, confident) "
                 "VALUES (" +
                     std::to_string(file_id) + ", '" + model + "', '" + label + "', 0.9, TRUE)");
  };
  // a and b hold alias-equivalent labels; c holds a label of the active model and one of
  // another model; d holds only a label of the other model.
  insert_label(ids_[0], kActiveModel, "portrait");
  insert_label(ids_[1], kActiveModel, " \xE4\xBA\xBA\xE5\x83\x8F ");  // "人像" with spaces
  insert_label(ids_[2], kActiveModel, "Landscape");
  insert_label(ids_[2], kInactiveModel, "street");
  insert_label(ids_[3], kInactiveModel, "portrait");

  const auto result = ReadAll(project, AlbumQueryOptions{.group_field_ = AlbumGroupField::kLabels});
  std::vector<std::pair<std::string, int64_t>> groups;
  for (const auto& group : result.groups_) {
    groups.emplace_back(KeyText(group.key_), group.photo_count_);
  }
  const std::vector<std::pair<std::string, int64_t>> expected_groups{
      {"landscape", 1}, {"portrait", 2}, {"<unknown>", 5}};
  EXPECT_EQ(groups, expected_groups);
  EXPECT_EQ(result.unique_file_count_, 8);
  EXPECT_EQ(result.occurrence_count_, 8);

  std::map<std::string, std::set<sl_element_id_t>> members;
  for (const auto& row : result.rows_) {
    members[KeyText(row.group_key_)].insert(row.photo_.file_id_);
  }
  EXPECT_EQ(members["portrait"], (std::set<sl_element_id_t>{ids_[0], ids_[1]}));
  EXPECT_EQ(members["landscape"], (std::set<sl_element_id_t>{ids_[2]}));
  // d has a label only in the inactive model, so it stays in the unlabelled group.
  EXPECT_TRUE(members["<unknown>"].contains(ids_[3]));

  // The Inspector label buckets read the same canonical relation.
  ASSERT_TRUE(result.statistics_.has_value());
  std::map<std::string, int> label_buckets;
  for (const auto& bucket : result.statistics_->label_stats_) {
    label_buckets[bucket.label_] = bucket.count_;
  }
  EXPECT_EQ(label_buckets, (std::map<std::string, int>{{"landscape", 1}, {"portrait", 2}}));

  // The label sort uses each file's minimum canonical key, unlabelled files last.
  const auto sorted = ReadAll(project, AlbumQueryOptions{.sort_field_ = AlbumSortField::kLabels});
  const auto ids    = RowIds(sorted);
  ASSERT_EQ(ids.size(), 8u);
  EXPECT_EQ(ids[0], ids_[2]);
  EXPECT_EQ(std::set<sl_element_id_t>(ids.begin() + 1, ids.begin() + 3),
            (std::set<sl_element_id_t>{ids_[0], ids_[1]}));
}

TEST_F(AlbumQueryTest, ImportDayFilterUsesLocalMidnightBoundaries) {
  ProjectService                                            project(db_path_, meta_path_);
  // America/New_York: 2026-06-10 is an ordinary EDT day (UTC-4, starts 04:00 UTC); 2026-03-08
  // is the spring-forward day (starts 05:00 UTC, 23 hours long, next midnight 04:00 UTC).
  const std::vector<std::pair<const wchar_t*, std::time_t>> times{
      {L"before_june.ARW", Utc(2026, 6, 10, 3, 59, 59)},
      {L"june_start.ARW", Utc(2026, 6, 10, 4, 0, 0)},
      {L"june_end.ARW", Utc(2026, 6, 11, 3, 59, 59)},
      {L"after_june.ARW", Utc(2026, 6, 11, 4, 0, 0)},
      {L"before_dst.ARW", Utc(2026, 3, 8, 4, 59, 59)},
      {L"dst_start.ARW", Utc(2026, 3, 8, 5, 0, 0)},
      {L"dst_end.ARW", Utc(2026, 3, 9, 3, 59, 59)},
      {L"after_dst.ARW", Utc(2026, 3, 9, 4, 0, 0)},
  };
  std::vector<SyntheticImageSpec> specs;
  for (const auto& [name, added] : times) {
    specs.push_back(SyntheticImageSpec{.file_name_   = name,
                                       .image_path_  = std::wstring(L"D:/") + name,
                                       .is_raw_file_ = false,
                                       .added_time_  = added});
  }
  SyntheticLibraryBuilder builder(project);
  const auto              ids = builder.AddFiles(specs);
  ASSERT_EQ(ids.size(), times.size());

  const auto names_on_day = [&](const wchar_t* day) {
    const auto result =
        ReadAll(project, AlbumQueryOptions{.import_day_time_zone_ = kNewYork},
                CompileFilter(sleeve_filter::BuildImportDateBucketFilter(day, kNewYork)));
    std::set<std::string> names;
    for (const auto& row : result.rows_) {
      names.insert(row.photo_.file_name_);
    }
    return names;
  };
  EXPECT_EQ(names_on_day(L"2026-06-10"), (std::set<std::string>{"june_start.ARW", "june_end.ARW"}));
  EXPECT_EQ(names_on_day(L"2026-03-08"), (std::set<std::string>{"dst_start.ARW", "dst_end.ARW"}));

  // Groups, statistics, and the filter agree on the same local day.
  const auto grouped =
      ReadAll(project, AlbumQueryOptions{.group_field_          = AlbumGroupField::kImportDay,
                                         .import_day_time_zone_ = kNewYork});
  std::map<std::string, std::string> day_of;
  for (const auto& row : grouped.rows_) {
    day_of[row.photo_.file_name_] = KeyText(row.group_key_);
  }
  EXPECT_EQ(day_of["before_june.ARW"], "2026-06-09");
  EXPECT_EQ(day_of["june_start.ARW"], "2026-06-10");
  EXPECT_EQ(day_of["june_end.ARW"], "2026-06-10");
  EXPECT_EQ(day_of["after_june.ARW"], "2026-06-11");
  EXPECT_EQ(day_of["before_dst.ARW"], "2026-03-07");
  EXPECT_EQ(day_of["dst_start.ARW"], "2026-03-08");
  EXPECT_EQ(day_of["dst_end.ARW"], "2026-03-08");
  EXPECT_EQ(day_of["after_dst.ARW"], "2026-03-09");
  ASSERT_TRUE(grouped.statistics_.has_value());
  std::map<std::string, int> buckets;
  for (const auto& bucket : grouped.statistics_->import_date_stats_) {
    buckets[bucket.label_] = bucket.count_;
  }
  EXPECT_EQ(buckets["2026-06-10"], 2);
  EXPECT_EQ(buckets["2026-03-08"], 2);
}

TEST_F(AlbumQueryTest, ImportDayFilterRejectsInvalidDaysAndEmptyZones) {
  EXPECT_THROW(sleeve_filter::BuildImportDateBucketFilter(L"2026-02-30", kNewYork),
               std::invalid_argument);
  EXPECT_THROW(sleeve_filter::BuildImportDateBucketFilter(L"2026-6-1", kNewYork),
               std::invalid_argument);
  EXPECT_THROW(sleeve_filter::BuildImportDateBucketFilter(L"not a day", kNewYork),
               std::invalid_argument);
  EXPECT_THROW(sleeve_filter::BuildImportDateBucketFilter(L"2026-06-01", ""),
               std::invalid_argument);
  EXPECT_NO_THROW(sleeve_filter::BuildImportDateBucketFilter(L"2024-02-29", kNewYork));
}

TEST_F(AlbumQueryTest, UnknownImportTimeFormsTheLastImportDayGroup) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  RunStatement(project,
               "UPDATE Element SET added_time = NULL WHERE id = " + std::to_string(ids_[3]));

  const auto grouped =
      ReadAll(project, AlbumQueryOptions{.group_field_          = AlbumGroupField::kImportDay,
                                         .import_day_time_zone_ = "UTC"});
  ASSERT_FALSE(grouped.groups_.empty());
  EXPECT_EQ(KeyText(grouped.groups_.back().key_), "<unknown>");
  EXPECT_EQ(grouped.groups_.back().photo_count_, 1);
  EXPECT_EQ(grouped.rows_.back().photo_.file_id_, ids_[3]);
  EXPECT_FALSE(grouped.rows_.back().photo_.added_time_.has_value());

  const auto unknown = ReadAll(project, AlbumQueryOptions{},
                               CompileFilter(sleeve_filter::BuildImportDateUnknownFilter()));
  EXPECT_EQ(RowIds(unknown), std::vector<sl_element_id_t>{ids_[3]});
}

TEST_F(AlbumQueryTest, DateGroupsSortPhotosByFullTimestamp) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto id = [this](size_t index) { return ids_[index]; };
  // Capture days newest first; inside a day the full capture time, then the file id.
  {
    const auto ascending =
        ReadAll(project, AlbumQueryOptions{.sort_field_     = AlbumSortField::kCaptureTime,
                                           .sort_direction_ = SortDirection::kAscending,
                                           .group_field_    = AlbumGroupField::kCaptureDay});
    EXPECT_EQ(RowIds(ascending), (std::vector<sl_element_id_t>{id(0), id(1), id(2), id(6), id(4),
                                                               id(5), id(3), id(7)}));
    const auto descending =
        ReadAll(project, AlbumQueryOptions{.sort_field_     = AlbumSortField::kCaptureTime,
                                           .sort_direction_ = SortDirection::kDescending,
                                           .group_field_    = AlbumGroupField::kCaptureDay});
    EXPECT_EQ(RowIds(descending), (std::vector<sl_element_id_t>{id(1), id(2), id(0), id(4), id(6),
                                                                id(5), id(3), id(7)}));
  }
  // Import days in UTC: 09-02 {c, d, e}, 09-01 {a, b, h}, 08-30 {f, g}.
  {
    const auto ascending =
        ReadAll(project, AlbumQueryOptions{.sort_field_           = AlbumSortField::kImportTime,
                                           .sort_direction_       = SortDirection::kAscending,
                                           .group_field_          = AlbumGroupField::kImportDay,
                                           .import_day_time_zone_ = "UTC"});
    EXPECT_EQ(RowIds(ascending), (std::vector<sl_element_id_t>{id(2), id(3), id(4), id(0), id(1),
                                                               id(7), id(5), id(6)}));
    const auto descending =
        ReadAll(project, AlbumQueryOptions{.sort_field_           = AlbumSortField::kImportTime,
                                           .sort_direction_       = SortDirection::kDescending,
                                           .group_field_          = AlbumGroupField::kImportDay,
                                           .import_day_time_zone_ = "UTC"});
    EXPECT_EQ(RowIds(descending), (std::vector<sl_element_id_t>{id(4), id(2), id(3), id(7), id(0),
                                                                id(1), id(5), id(6)}));
    std::vector<std::string> days;
    for (const auto& group : descending.groups_) {
      days.push_back(KeyText(group.key_));
    }
    EXPECT_EQ(days, (std::vector<std::string>{"2026-09-02", "2026-09-01", "2026-08-30"}));
  }
}

TEST_F(AlbumQueryTest, DateGroupTimestampResolvesEqualPhotoSortValues) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto              id = [this](size_t index) { return ids_[index]; };
  // Capture day 2026-06-07 holds a (3, 09:00), b (5, 18:00), c (3, 18:00): rating DESC, then
  // capture time newest first, then the file id.
  const AlbumQueryOptions capture_options{.sort_field_     = AlbumSortField::kRating,
                                          .sort_direction_ = SortDirection::kDescending,
                                          .group_field_    = AlbumGroupField::kCaptureDay};
  const auto              capture = ReadAll(project, capture_options);
  EXPECT_EQ(RowIds(capture),
            (std::vector<sl_element_id_t>{id(1), id(2), id(0), id(4), id(6), id(5), id(7), id(3)}));
  // Import day 2026-09-01 (UTC) holds a (3, 10:00), b (5, 10:00), h (3, 23:59:59).
  const AlbumQueryOptions import_options{.sort_field_           = AlbumSortField::kRating,
                                         .sort_direction_       = SortDirection::kDescending,
                                         .group_field_          = AlbumGroupField::kImportDay,
                                         .import_day_time_zone_ = "UTC"};
  const auto              imported = ReadAll(project, import_options);
  EXPECT_EQ(RowIds(imported),
            (std::vector<sl_element_id_t>{id(4), id(2), id(3), id(1), id(7), id(0), id(6), id(5)}));

  // Position reads agree with the page order for every photo, with ties and NULL fields.
  for (const auto& [options, result] :
       {std::pair{capture_options, capture}, std::pair{import_options, imported}}) {
    for (size_t index = 0; index < result.rows_.size(); ++index) {
      const auto position =
          Store(project).ReadAlbumFilePosition(0, std::nullopt, options, kActiveModel,
                                               result.rows_[index].photo_.file_id_, std::nullopt);
      ASSERT_TRUE(position.has_value());
      EXPECT_EQ(position->occurrence_index_, static_cast<int64_t>(index));
      EXPECT_EQ(KeyText(position->group_key_), KeyText(result.rows_[index].group_key_));
    }
    EXPECT_EQ(Store(project).ReadAlbumFileIds(0, std::nullopt, options, kActiveModel, 0, 8),
              RowIds(result));
    EXPECT_EQ(Store(project).ReadAlbumFileIds(0, std::nullopt, options, kActiveModel, 2, 5),
              (std::vector<sl_element_id_t>{result.rows_[2].photo_.file_id_,
                                            result.rows_[3].photo_.file_id_,
                                            result.rows_[4].photo_.file_id_}));
  }
}

TEST_F(AlbumQueryTest, DateGroupWithoutExplicitSortUsesFullTimestamp) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto id = [this](size_t index) { return ids_[index]; };
  const auto grouped =
      ReadAll(project, AlbumQueryOptions{.group_field_ = AlbumGroupField::kCaptureDay});
  EXPECT_EQ(RowIds(grouped),
            (std::vector<sl_element_id_t>{id(1), id(2), id(0), id(4), id(6), id(5), id(3), id(7)}));
  auto sorted = ids_;
  std::sort(sorted.begin(), sorted.end());
  EXPECT_EQ(RowIds(ReadAll(project, AlbumQueryOptions{})), sorted);
}

TEST_F(AlbumQueryTest, SameScalarGroupAndSortUsesFileIdForTies) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto id = [this](size_t index) { return ids_[index]; };
  for (const auto direction : {SortDirection::kAscending, SortDirection::kDescending}) {
    const auto camera =
        ReadAll(project, AlbumQueryOptions{.sort_field_     = AlbumSortField::kCameraModel,
                                           .sort_direction_ = direction,
                                           .group_field_    = AlbumGroupField::kCameraModel});
    EXPECT_EQ(RowIds(camera), (std::vector<sl_element_id_t>{id(0), id(1), id(6), id(2), id(4),
                                                            id(5), id(3), id(7)}));
    const auto rating =
        ReadAll(project, AlbumQueryOptions{.sort_field_     = AlbumSortField::kRating,
                                           .sort_direction_ = direction,
                                           .group_field_    = AlbumGroupField::kRating});
    EXPECT_EQ(RowIds(rating), (std::vector<sl_element_id_t>{id(1), id(4), id(0), id(2), id(6),
                                                            id(7), id(5), id(3)}));
    const auto lens = ReadAll(project, AlbumQueryOptions{.sort_field_     = AlbumSortField::kLens,
                                                         .sort_direction_ = direction,
                                                         .group_field_ = AlbumGroupField::kLens});
    EXPECT_EQ(RowIds(lens), (std::vector<sl_element_id_t>{id(5), id(7), id(0), id(6), id(2), id(4),
                                                          id(1), id(3)}));
  }
}

TEST_F(AlbumQueryTest, AlbumScopeAndEmptyScopeUseTheSameQuery) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  auto       sleeve = project.GetSleeveService();
  const auto album  = sleeve->CreateFolder(L"/", L"Picks");
  ASSERT_TRUE(album.second.success_);
  const auto album_id = album.first->element_id_;
  ASSERT_TRUE(sleeve->LinkFileToFolder(ids_[4], album_id).success_);
  ASSERT_TRUE(sleeve->LinkFileToFolder(ids_[1], album_id).success_);

  const auto result = ReadAll(project,
                              AlbumQueryOptions{.sort_field_     = AlbumSortField::kRating,
                                                .sort_direction_ = SortDirection::kDescending,
                                                .group_field_    = AlbumGroupField::kCameraModel},
                              std::nullopt, album_id);
  EXPECT_EQ(RowIds(result), (std::vector<sl_element_id_t>{ids_[1], ids_[4]}));
  EXPECT_EQ(result.unique_file_count_, 2);
  ASSERT_TRUE(result.statistics_.has_value());
  EXPECT_EQ(result.statistics_->total_photo_count_, 2);

  const auto empty_album = sleeve->CreateFolder(L"/", L"Empty");
  ASSERT_TRUE(empty_album.second.success_);
  const auto empty = ReadAll(project, AlbumQueryOptions{.group_field_ = AlbumGroupField::kLabels},
                             std::nullopt, empty_album.first->element_id_);
  EXPECT_EQ(empty.unique_file_count_, 0);
  EXPECT_EQ(empty.occurrence_count_, 0);
  EXPECT_TRUE(empty.groups_.empty());
  EXPECT_TRUE(empty.rows_.empty());
}

TEST_F(AlbumQueryTest, InvalidOptionsAndPageBoundsAreRejectedBeforeSql) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  auto&      store = Store(project);
  const auto read  = [&store](const AlbumQueryOptions& options, AlbumQueryRead page) {
    return store.ReadAlbumQuery(0, std::nullopt, options, kActiveModel, page);
  };
  const AlbumQueryRead page{.offset_ = 0, .limit_ = 10};
  EXPECT_THROW(read(AlbumQueryOptions{.sort_field_ = static_cast<AlbumSortField>(99)}, page),
               std::invalid_argument);
  EXPECT_THROW(read(AlbumQueryOptions{.sort_direction_ = static_cast<SortDirection>(7)}, page),
               std::invalid_argument);
  EXPECT_THROW(read(AlbumQueryOptions{.group_field_ = static_cast<AlbumGroupField>(42)}, page),
               std::invalid_argument);
  EXPECT_THROW(read(AlbumQueryOptions{.group_field_ = AlbumGroupField::kImportDay}, page),
               std::invalid_argument);
  EXPECT_THROW(read(AlbumQueryOptions{}, AlbumQueryRead{.offset_ = 0, .limit_ = 1001}),
               std::invalid_argument);
  EXPECT_THROW(read(AlbumQueryOptions{}, AlbumQueryRead{.offset_ = -1, .limit_ = 10}),
               std::invalid_argument);
  EXPECT_THROW(store.ReadAlbumFileIds(0, std::nullopt, AlbumQueryOptions{}, kActiveModel, 5, 2),
               std::invalid_argument);
}

TEST_F(AlbumQueryTest, AlbumQueryFailureReportsDuckDbError) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  auto&                      store = Store(project);
  // A predicate on a missing column fails when DuckDB prepares the match set statement.
  const duckorm::SqlFragment broken{"no_such_column = 1", {}};
  EXPECT_THROW(store.ReadAlbumQuery(0, broken, AlbumQueryOptions{}, kActiveModel,
                                    AlbumQueryRead{.offset_ = 0, .limit_ = 10}),
               std::runtime_error);
  // An unknown zone fails when DuckDB executes the import-day conversion.
  EXPECT_THROW(store.ReadAlbumQuery(0, std::nullopt,
                                    AlbumQueryOptions{.group_field_ = AlbumGroupField::kImportDay,
                                                      .import_day_time_zone_ = "Not/AZone"},
                                    kActiveModel, AlbumQueryRead{.offset_ = 0, .limit_ = 10}),
               std::runtime_error);
  EXPECT_THROW(store.ReadAlbumFilePosition(0, broken, AlbumQueryOptions{}, kActiveModel, ids_[0],
                                           std::nullopt),
               std::runtime_error);
  EXPECT_THROW(store.ListFilteredFileIds(0, broken), std::runtime_error);
  EXPECT_THROW(store.CountFilesInFolder(0, broken), std::runtime_error);

  // The failed reads released the database lock and the match set: another thread reads.
  auto other_thread = std::async(std::launch::async, [&store] {
    return store.ReadAlbumQuery(0, std::nullopt, AlbumQueryOptions{}, kActiveModel,
                                AlbumQueryRead{.offset_ = 0, .limit_ = 10, .read_groups_ = true});
  });
  ASSERT_EQ(other_thread.wait_for(std::chrono::seconds(30)), std::future_status::ready);
  EXPECT_EQ(other_thread.get().unique_file_count_, 8);
}

/// Set the process time zone for the C library (mktime, localtime) and restore it at exit.
class ScopedProcessTimeZone {
 public:
  explicit ScopedProcessTimeZone(const char* zone) {
    if (const char* previous = std::getenv("TZ")) {
      previous_ = previous;
    }
    Set(zone);
  }
  ~ScopedProcessTimeZone() { Set(previous_.has_value() ? previous_->c_str() : ""); }

 private:
  static void Set(const char* zone) {
#ifdef _WIN32
    _putenv_s("TZ", zone);
    _tzset();
#else
    if (zone[0] == '\0') {
      unsetenv("TZ");
    } else {
      setenv("TZ", zone, 1);
    }
    tzset();
#endif
  }
  std::optional<std::string> previous_;
};

TEST_F(AlbumQueryTest, ElementTimesRoundTripAsUtcAcrossLocalTimeZones) {
  const auto added = Utc(2026, 9, 1, 10, 30, 15);
  for (const char* zone : {"JST-9", "EST5EDT", "UTC0"}) {
    RemoveProjectFiles();
    ScopedProcessTimeZone   process_zone(zone);
    ProjectService          project(db_path_, meta_path_);
    SyntheticLibraryBuilder builder(project);
    const auto              ids = builder.AddFiles({SyntheticImageSpec{.file_name_   = L"x.ARW",
                                                                       .image_path_  = L"D:/x.ARW",
                                                                       .is_raw_file_ = false,
                                                                       .added_time_  = added}});
    ASSERT_EQ(ids.size(), 1u);
    auto& store = Store(project);
    // The column holds the UTC wall time.
    EXPECT_EQ(QueryText(project, "SELECT CAST(added_time AS VARCHAR) FROM Element WHERE id = " +
                                     std::to_string(ids[0])),
              "2026-09-01 10:30:15")
        << zone;
    // A mapper read and a mapper update keep the same instant.
    auto element = store.GetElementById(ids[0]);
    EXPECT_EQ(element->added_time_, added) << zone;
    {
      auto guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
      auto lock  = guard.Lock();
      ElementMapper(guard.conn_).Update(element, element->element_id_);
    }
    EXPECT_EQ(store.GetElementById(ids[0])->added_time_, added) << zone;
    EXPECT_EQ(QueryText(project, "SELECT CAST(added_time AS VARCHAR) FROM Element WHERE id = " +
                                     std::to_string(ids[0])),
              "2026-09-01 10:30:15")
        << zone;
  }
}

TEST_F(AlbumQueryTest, LinkingFileToAlbumKeepsImportTime) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  auto       sleeve = project.GetSleeveService();
  const auto album  = sleeve->CreateFolder(L"/", L"Trip");
  ASSERT_TRUE(album.second.success_);
  const auto album_id = album.first->element_id_;
  const auto file_id  = ids_[2];
  const auto expected = *photos_[2].spec_.added_time_;

  ASSERT_TRUE(sleeve->LinkFileToFolder(file_id, album_id).success_);
  const auto in_album = ReadAll(project, AlbumQueryOptions{}, std::nullopt, album_id);
  ASSERT_EQ(in_album.rows_.size(), 1u);
  EXPECT_EQ(in_album.rows_[0].photo_.added_time_, expected);

  ASSERT_TRUE(sleeve->DeleteFileFromFolder(file_id, album_id).success_);
  EXPECT_TRUE(ReadAll(project, AlbumQueryOptions{}, std::nullopt, album_id).rows_.empty());
  EXPECT_EQ(Store(project).GetElementById(file_id)->added_time_, expected);
  const auto root = ReadAll(project, AlbumQueryOptions{});
  for (const auto& row : root.rows_) {
    EXPECT_EQ(row.photo_.added_time_, *Photo(row.photo_.file_id_).spec_.added_time_);
  }
}

TEST_F(AlbumQueryTest, ImportTimePersistsThroughImportAndProjectReopen) {
  const auto raw_fixture = std::filesystem::path(std::string(TEST_IMG_PATH)) / "ci_rawfiles" /
                           "Tag @ryanbreitkreutz - Free files from @signatureeditscoDSC00830.ARW";
  if (!std::filesystem::exists(raw_fixture)) {
    GTEST_SKIP() << "RAW fixture is not available: " << raw_fixture.string();
  }
  const auto raw_copy = scratch_dir_ / "imported.ARW";
  std::filesystem::copy_file(raw_fixture, raw_copy);
  // A JPEG that import rejects by content: the failed file must not appear in the result.
  const auto rejected = scratch_dir_ / "rejected.jpg";
  test_support::WriteRgbRaster(rejected, ".jpg");

  const auto before = std::chrono::system_clock::to_time_t(
      std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
  std::optional<int64_t> imported_time;
  {
    TimeProvider::Refresh();
    ProjectService    project(db_path_, meta_path_);
    ImportServiceImpl import_service(project.GetSleeveService(), project.GetImagePoolService(),
                                     std::make_shared<PipelineMgmtService>(project.GetStorage()));
    auto              job = std::make_shared<ImportJob>();
    std::promise<ImportResult> finished;
    auto                       finished_future = finished.get_future();
    job->on_finished_ = [&finished](const ImportResult& result) { finished.set_value(result); };
    job               = import_service.ImportToFolder({raw_copy, rejected}, L"", {}, job);
    const auto result = finished_future.get();
    import_service.SyncImports(job->import_log_->Snapshot(), L"");
    EXPECT_EQ(result.imported_, 1u);
    EXPECT_EQ(result.failed_, 1u);

    const auto query =
        ReadAll(project, AlbumQueryOptions{.sort_field_ = AlbumSortField::kImportTime});
    ASSERT_EQ(query.rows_.size(), 1u);
    EXPECT_EQ(query.rows_[0].photo_.file_name_, "imported.ARW");
    imported_time = query.rows_[0].photo_.added_time_;
    ASSERT_TRUE(imported_time.has_value());
    project.SaveProject(meta_path_);
  }
  const auto after = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  EXPECT_GE(*imported_time, static_cast<int64_t>(before) - 1);
  EXPECT_LE(*imported_time, static_cast<int64_t>(after) + 1);

  ProjectService reopened(db_path_, meta_path_, ProjectOpenMode::kLoadExisting);
  const auto     query = ReadAll(reopened, AlbumQueryOptions{});
  ASSERT_EQ(query.rows_.size(), 1u);
  EXPECT_EQ(query.rows_[0].photo_.added_time_, imported_time);
  EXPECT_EQ(static_cast<int64_t>(
                Store(reopened).GetElementById(query.rows_[0].photo_.file_id_)->added_time_),
            *imported_time);
}

TEST_F(AlbumQueryTest, LinkedDuckDbConvertsIanaTimeZonesWithoutTheSessionSetting) {
  ProjectService project(db_path_, meta_path_);
  std::cout << "Linked DuckDB: " << QueryText(project, "SELECT version()") << "\n";
  // The session TimeZone does not change the conversion of the import-day expression.
  RunStatement(project, "SET TimeZone = 'Asia/Tokyo'");
  EXPECT_EQ(QueryText(project,
                      "SELECT CAST(CAST(timezone('America/New_York', timezone('UTC', "
                      "TIMESTAMP '2026-03-08 05:00:00')) AS DATE) AS VARCHAR)"),
            "2026-03-08");
  EXPECT_EQ(QueryText(project,
                      "SELECT CAST(timezone('UTC', timezone('America/New_York', "
                      "TIMESTAMP '2026-03-09 00:00:00')) AS VARCHAR)"),
            "2026-03-09 04:00:00");
}

/// Run @p setup, then @p sql on the same connection, and return column @p column of every row
/// joined by newlines (EXPLAIN output).
auto QueryColumnText(ProjectService& project, const std::string& setup, const std::string& sql,
                     idx_t column) -> std::string {
  auto          guard = project.GetStorage()->GetDatabase().GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_, setup.c_str(), &result) != DuckDBSuccess) {
    ADD_FAILURE() << setup << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return {};
  }
  duckdb_destroy_result(&result);
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    ADD_FAILURE() << sql << ": " << duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    return {};
  }
  std::string text;
  for (idx_t row = 0; row < duckdb_row_count(&result); ++row) {
    char* value = duckdb_value_varchar(&result, column, row);
    text += value;
    text += "\n";
    duckdb_free(value);
  }
  duckdb_destroy_result(&result);
  return text;
}

auto Percentile(std::vector<double> values, double fraction) -> double {
  std::sort(values.begin(), values.end());
  const auto index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1) + 0.5);
  return values[std::min(index, values.size() - 1)];
}

// Opt-in measurement (ALCEDO_ALBUM_QUERY_PROFILE=<file count>, 1 means 10,000): a library with
// repeated timestamps, unknown fields, one large and many small camera groups, and one active
// model label per file. Records the warm read timings and the physical plan of the deep page,
// and checks that every page stays bounded and that the stream holds every file once.
TEST_F(AlbumQueryTest, LargeLibraryPagesStayBoundedAndRecordTimings) {
  const char* profile = std::getenv("ALCEDO_ALBUM_QUERY_PROFILE");
  if (profile == nullptr) {
    GTEST_SKIP() << "Set ALCEDO_ALBUM_QUERY_PROFILE=<file count> to record album query timings";
  }
  const int requested = std::atoi(profile);
  const int kFiles    = requested >= 1000 ? requested : 10000;
  std::vector<SyntheticImageSpec> specs;
  specs.reserve(static_cast<size_t>(kFiles));
  for (int index = 0; index < kFiles; ++index) {
    const bool        large_group = index % 2 == 0;
    const std::string camera =
        large_group ? "Canon R5" : (index % 7 == 0 ? "" : "Model " + std::to_string(index % 400));
    char capture[32];
    std::snprintf(capture, sizeof(capture), "2026-%02d-%02d %02d:00:00", 1 + index % 12,
                  1 + index % 28, index % 24);
    specs.push_back(
        SyntheticImageSpec{.file_name_    = L"p" + std::to_wstring(index) + L".ARW",
                           .image_path_   = L"D:/scale/p" + std::to_wstring(index) + L".ARW",
                           .model_        = camera,
                           .lens_         = index % 5 == 0 ? "" : "Lens " + std::to_string(index % 30),
                           .date_time_    = index % 11 == 0 ? "" : capture,
                           .rating_       = index % 6,
                           .is_raw_file_  = false,
                           .added_time_   = Utc(2026, 9, 1 + index % 20, index % 3)});
  }
  ProjectService          project(db_path_, meta_path_);
  SyntheticLibraryBuilder builder(project);
  ASSERT_EQ(builder.AddFiles(specs).size(), static_cast<size_t>(kFiles));
  // One label of the active model per file (the SemanticImageLabel key allows one), taken from
  // twelve taxonomy labels and their Chinese aliases; every 9th file has none.
  RunStatement(project,
               "INSERT INTO SemanticImageLabel (file_id, model_key, label, score, confident) "
               "SELECT e.id, 'model-active', (['portrait', 'landscape', 'street', 'sports', "
               "'wedding', 'forest', 'mountain', 'interior', 'family', 'event', 'concert', "
               "'\xE4\xBA\xBA\xE5\x83\x8F'])[1 + e.id % 12], 0.9, TRUE FROM Element e "
               "WHERE e.type = 0 AND e.id % 9 <> 0");
  auto& store = Store(project);

  const auto time_ms = [](const std::function<void()>& run) {
    const auto start = std::chrono::steady_clock::now();
    run();
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
  };
  const AlbumQueryOptions scalar{.sort_field_     = AlbumSortField::kRating,
                                 .sort_direction_ = SortDirection::kDescending,
                                 .group_field_    = AlbumGroupField::kCameraModel};
  const AlbumQueryOptions labels{.sort_field_     = AlbumSortField::kCaptureTime,
                                 .sort_direction_ = SortDirection::kDescending,
                                 .group_field_    = AlbumGroupField::kLabels};
  const AlbumQueryRead    initial_read{
         .offset_ = 0, .limit_ = 1000, .read_groups_ = true, .read_statistics_ = true};
  std::map<std::string, std::vector<double>> samples;
  AlbumQueryResult                           first;
  AlbumQueryResult                           first_labels;
  for (int run = 0; run < 11; ++run) {
    const bool warm = run > 0;  // the first run is the cold read
    const auto record = [&](const std::string& name, double value) {
      if (warm) {
        samples[name].push_back(value);
      } else {
        std::cout << "cold " << name << " ms: " << value << "\n";
      }
    };
    record("scalar initial (groups+stats+page)", time_ms([&] {
             first = store.ReadAlbumQuery(0, std::nullopt, scalar, kActiveModel, initial_read);
           }));
    for (const auto& [name, offset] :
         {std::pair{"scalar shallow page", int64_t{1000}},
          std::pair{"scalar middle page", int64_t{kFiles / 2}},
          std::pair{"scalar deep page", int64_t{kFiles - 1000}}}) {
      record(name, time_ms([&] {
               EXPECT_EQ(store
                             .ReadAlbumQuery(0, std::nullopt, scalar, kActiveModel,
                                             AlbumQueryRead{.offset_ = offset, .limit_ = 1000})
                             .rows_.size(),
                         1000u);
             }));
    }
    record("scalar focus position", time_ms([&] {
             EXPECT_TRUE(store
                             .ReadAlbumFilePosition(0, std::nullopt, scalar, kActiveModel,
                                                    first.rows_.back().photo_.file_id_,
                                                    std::nullopt)
                             .has_value());
           }));
    record("label initial (groups+stats+page)", time_ms([&] {
             first_labels =
                 store.ReadAlbumQuery(0, std::nullopt, labels, kActiveModel, initial_read);
           }));
    record("label deep page", time_ms([&] {
             EXPECT_EQ(store
                           .ReadAlbumQuery(0, std::nullopt, labels, kActiveModel,
                                           AlbumQueryRead{.offset_ = kFiles - 1000, .limit_ = 1000})
                           .rows_.size(),
                       1000u);
           }));
  }
  EXPECT_EQ(first.unique_file_count_, kFiles);
  EXPECT_EQ(first.rows_.size(), 1000u);
  int64_t counted = 0;
  for (const auto& group : first.groups_) {
    counted += group.photo_count_;
  }
  EXPECT_EQ(counted, kFiles);
  EXPECT_EQ(first_labels.occurrence_count_, kFiles);
  const auto all_ids = store.ReadAlbumFileIds(0, std::nullopt, scalar, kActiveModel, 0, kFiles);
  EXPECT_EQ(std::set<sl_element_id_t>(all_ids.begin(), all_ids.end()).size(),
            static_cast<size_t>(kFiles));

  std::cout << "files: " << kFiles << ", camera groups: " << first.groups_.size()
            << ", label groups: " << first_labels.groups_.size() << "\n";
  for (const auto& [name, values] : samples) {
    std::cout << name << " ms p50 " << Percentile(values, 0.5) << " p95 "
              << Percentile(values, 0.95) << "\n";
  }
  // The physical plan of the deep page statement on the same match set columns.
  std::cout << QueryColumnText(project,
                               "CREATE TEMP TABLE SearchMatchSet AS SELECT e.id AS file_id, "
                               "fi.image_id, i.file_name, i.camera_model, i.lens, i.capture_date, "
                               "i.capture_at, i.rating, e.added_time, CAST(NULL AS DATE) AS "
                               "import_day FROM Element e JOIN FileImage fi ON fi.file_id = e.id "
                               "JOIN Image i ON i.id = fi.image_id WHERE e.type = 0",
                               "EXPLAIN ANALYZE SELECT s.file_id, s.image_id, s.file_name, "
                               "CAST(NULLIF(s.camera_model, '') AS VARCHAR) FROM SearchMatchSet s "
                               "ORDER BY NULLIF(s.camera_model, '') ASC NULLS LAST, s.rating DESC "
                               "NULLS LAST, s.file_id ASC NULLS LAST LIMIT 1000 OFFSET " +
                                   std::to_string(kFiles - 1000),
                               1);
}

}  // namespace
}  // namespace alcedo
