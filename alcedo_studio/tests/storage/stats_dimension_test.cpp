//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Storage S4 (#789): each stats dimension has one SQL definition, so the photos that a clicked
// bucket selects are the photos that the bucket counts. The fixture holds empty, NULL, and
// repeated values in every dimension; for each returned bucket the test runs the bucket filter
// of the stats bar and compares the page total with the bucket count.

#include "storage/query/stats_dimension.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "app/project_service.hpp"
#include "image/color_label.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/album_query.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"
#include "storage/query/filter_compiler.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/string/convert.hpp"
#include "../support/storage_test_access.hpp"
#include "support/test_temp_directory.hpp"

namespace alcedo {
namespace {

using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

constexpr const char* kModel     = "model-active";
constexpr const char* kNewYork   = "America/New_York";
constexpr size_t      kFileCount = 24;

void                  RunStatement(ProjectService& project, const std::string& sql) {
  auto          guard = StorageTestAccess::GetDatabase(*project.GetStorage()).GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  ASSERT_EQ(duckdb_query(guard.conn_, sql.c_str(), &result), DuckDBSuccess)
      << sql << ": " << duckdb_result_error(&result);
  duckdb_destroy_result(&result);
}

/// SQL literal of @p value, or NULL.
auto Literal(const std::optional<std::string>& value) -> std::string {
  return value.has_value() ? "'" + *value + "'" : "NULL";
}

/// The stats dimension values of fixture file @p index. Each dimension repeats with its own
/// period, so the buckets have uneven sizes and the dimensions are independent.
struct FixtureValues {
  std::optional<std::string> camera_model;
  std::optional<std::string> lens;
  std::optional<std::string> capture_date;
  std::optional<std::string> rating;
  std::optional<std::string> added_time;  ///< UTC wall time.
  std::optional<std::string> color_labels;
  std::optional<std::string> delete_mark;
  std::vector<std::string>   labels_of_model;  ///< SemanticImageLabel rows of kModel.
};

auto FixtureValuesOf(size_t index) -> FixtureValues {
  // The literal "(unknown)" camera is in the unknown bucket with NULL and the empty text.
  const std::array<std::optional<std::string>, 6> cameras{"Canon R5",   "Canon R5", "",
                                                          std::nullopt, "Nikon Z8", "(unknown)"};
  const std::array<std::optional<std::string>, 4> lenses{"RF 50", "", std::nullopt, "Z 24-70"};
  const std::array<std::optional<std::string>, 5> dates{"2026-06-07", "2026-06-07", std::nullopt,
                                                        "2026-06-05", "2026-01-31"};
  const std::array<std::optional<std::string>, 7> ratings{"0", "1",          "3", "3",
                                                          "5", std::nullopt, "2"};
  // Import times around the New York midnights of the daylight-saving days of 2026, and NULL.
  const std::array<std::optional<std::string>, 8> imports{
      "2026-03-08 04:30:00", "2026-03-08 05:00:00", "2026-03-09 03:59:59", "2026-03-09 04:00:00",
      std::nullopt,          "2026-11-01 04:30:00", "2026-11-02 04:59:59", "2026-11-02 05:00:00"};
  // Bit 7 is reserved and is no color label code.
  const std::array<std::optional<std::string>, 6> marks{"0", "1", "5", "64", "130", std::nullopt};
  const std::array<std::optional<std::string>, 3> deletes{"FALSE", "TRUE", std::nullopt};
  // Labels with another case and spaces, a blank label, and none (one label per file and model).
  const std::array<std::vector<std::string>, 5>   labels{
      std::vector<std::string>{"landscape"}, std::vector<std::string>{" Landscape "},
      std::vector<std::string>{"portrait"}, std::vector<std::string>{"  "},
      std::vector<std::string>{}};
  return FixtureValues{.camera_model    = cameras[index % cameras.size()],
                       .lens            = lenses[index % lenses.size()],
                       .capture_date    = dates[index % dates.size()],
                       .rating          = ratings[index % ratings.size()],
                       .added_time      = imports[index % imports.size()],
                       .color_labels    = marks[index % marks.size()],
                       .delete_mark     = deletes[index % deletes.size()],
                       .labels_of_model = labels[index % labels.size()]};
}

class StatsDimensionTest : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    TimeProvider::Refresh();
    db_path_   = temp_dir_ / "project.db";
    meta_path_ = temp_dir_ / "project.json";
  }

  /// Create the fixture files and write the dimension values of each one.
  void BuildFixture(ProjectService& project) {
    std::vector<SyntheticImageSpec> specs;
    for (size_t index = 0; index < kFileCount; ++index) {
      const auto name = std::format(L"photo_{:02}.JPG", index);
      specs.push_back(SyntheticImageSpec{
          .file_name_ = name, .image_path_ = L"D:/stats/" + name, .is_raw_file_ = false});
    }
    SyntheticLibraryBuilder builder(project);
    ids_ = builder.AddFiles(specs);
    ASSERT_EQ(ids_.size(), kFileCount);

    // Databases of older versions can hold NULL in these columns (the migrations that added
    // them have no NOT NULL), so the fixture holds NULL there too.
    for (const char* column : {"camera_model", "lens", "rating", "color_labels", "delete_mark"}) {
      RunStatement(project, std::format("ALTER TABLE Image ALTER COLUMN {} DROP NOT NULL", column));
    }
    for (size_t index = 0; index < kFileCount; ++index) {
      const auto values = FixtureValuesOf(index);
      RunStatement(
          project,
          std::format("UPDATE Image SET camera_model = {}, lens = {}, capture_date = {}, "
                      "rating = {}, color_labels = {}, delete_mark = {} WHERE id = "
                      "(SELECT image_id FROM FileImage WHERE file_id = {})",
                      Literal(values.camera_model), Literal(values.lens),
                      Literal(values.capture_date), Literal(values.rating),
                      Literal(values.color_labels), Literal(values.delete_mark), ids_[index]));
      RunStatement(project, std::format("UPDATE Element SET added_time = {} WHERE id = {}",
                                        Literal(values.added_time), ids_[index]));
      for (const auto& label : values.labels_of_model) {
        RunStatement(project, std::format("INSERT INTO SemanticImageLabel (file_id, model_key, "
                                          "label, score, confident) VALUES ({}, '{}', '{}', 0.9, "
                                          "TRUE)",
                                          ids_[index], kModel, label));
      }
      if (index % 6 == 0) {
        // A label of another model is in no bucket of the active model.
        RunStatement(project, std::format("INSERT INTO SemanticImageLabel (file_id, model_key, "
                                          "label, score, confident) VALUES ({}, 'model-other', "
                                          "'street', 0.9, TRUE)",
                                          ids_[index]));
      }
    }
  }

  auto Options() const -> AlbumQueryOptions {
    return AlbumQueryOptions{.local_day_time_zone_ = kNewYork};
  }

  auto Statistics(ProjectService& project) -> FolderStatsView {
    const auto result = project.GetStorage()->GetElementStore().ReadAlbumQuery(
        0, std::nullopt, Options(), kModel, AlbumQueryRead{.read_statistics_ = true});
    EXPECT_TRUE(result.statistics_.has_value());
    return result.statistics_.value_or(FolderStatsView{});
  }

  /// Files that @p filter selects, from the page read, and its page rows.
  auto PageTotal(ProjectService& project, const FilterNode& filter) -> int64_t {
    const auto result = project.GetStorage()->GetElementStore().ReadAlbumQuery(
        0, CompileFilterPredicate(filter), Options(), kModel,
        AlbumQueryRead{.offset_ = 0, .limit_ = static_cast<int64_t>(kFileCount)});
    EXPECT_EQ(static_cast<int64_t>(result.rows_.size()), result.unique_file_count_);
    return result.unique_file_count_;
  }

  std::filesystem::path        db_path_;
  std::filesystem::path        meta_path_;
  std::vector<sl_element_id_t> ids_;
};

auto Wide(const std::string& text) -> std::wstring { return conv::FromBytes(text); }

auto CountSum(const std::vector<StorageStatsBucket>& buckets) -> int {
  int sum = 0;
  for (const auto& bucket : buckets) {
    sum += bucket.count_;
  }
  return sum;
}

auto HasBucket(const std::vector<StorageStatsBucket>& buckets, const std::string& label) -> bool {
  for (const auto& bucket : buckets) {
    if (bucket.label_ == label) {
      return true;
    }
  }
  return false;
}

TEST_F(StatsDimensionTest, EachBucketPredicateSelectsItsBucketCount) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto stats = Statistics(project);
  ASSERT_EQ(stats.total_photo_count_, static_cast<int>(kFileCount));

  // The single-value dimensions partition the files and each has its unknown bucket.
  EXPECT_EQ(CountSum(stats.date_stats_), static_cast<int>(kFileCount));
  EXPECT_EQ(CountSum(stats.camera_stats_), static_cast<int>(kFileCount));
  EXPECT_EQ(CountSum(stats.lens_stats_), static_cast<int>(kFileCount));
  EXPECT_EQ(CountSum(stats.rating_stats_), static_cast<int>(kFileCount));
  EXPECT_EQ(CountSum(stats.import_date_stats_), static_cast<int>(kFileCount));
  EXPECT_TRUE(HasBucket(stats.date_stats_, ""));
  EXPECT_TRUE(HasBucket(stats.camera_stats_, "(unknown)"));
  EXPECT_TRUE(HasBucket(stats.lens_stats_, "(unknown)"));
  EXPECT_TRUE(HasBucket(stats.rating_stats_, ""));
  EXPECT_TRUE(HasBucket(stats.import_date_stats_, ""));
  EXPECT_TRUE(HasBucket(stats.label_stats_, "landscape"));
  EXPECT_FALSE(HasBucket(stats.label_stats_, "street"));

  for (const auto& bucket : stats.date_stats_) {
    SCOPED_TRACE("capture date " + bucket.label_);
    const auto filter = bucket.label_.empty()
                            ? sleeve_filter::BuildCaptureDateUnknownFilter()
                            : sleeve_filter::BuildCaptureDateBucketFilter(Wide(bucket.label_));
    EXPECT_EQ(PageTotal(project, filter), bucket.count_);
  }
  for (const auto& bucket : stats.camera_stats_) {
    SCOPED_TRACE("camera " + bucket.label_);
    EXPECT_EQ(PageTotal(project, sleeve_filter::BuildCameraModelBucketFilter(Wide(bucket.label_))),
              bucket.count_);
  }
  for (const auto& bucket : stats.lens_stats_) {
    SCOPED_TRACE("lens " + bucket.label_);
    EXPECT_EQ(PageTotal(project, sleeve_filter::BuildLensBucketFilter(Wide(bucket.label_))),
              bucket.count_);
  }
  for (const auto& bucket : stats.rating_stats_) {
    SCOPED_TRACE("rating " + bucket.label_);
    EXPECT_EQ(PageTotal(project, sleeve_filter::BuildRatingBucketFilter(Wide(bucket.label_))),
              bucket.count_);
  }
  for (const auto& bucket : stats.import_date_stats_) {
    SCOPED_TRACE("import day " + bucket.label_);
    const auto filter = bucket.label_.empty() ? sleeve_filter::BuildImportDateUnknownFilter()
                                              : sleeve_filter::BuildImportDateBucketFilter(
                                                    Wide(bucket.label_), kNewYork);
    EXPECT_EQ(PageTotal(project, filter), bucket.count_);
  }
  for (const auto& bucket : stats.label_stats_) {
    SCOPED_TRACE("semantic label " + bucket.label_);
    EXPECT_EQ(
        PageTotal(project, sleeve_filter::BuildSemanticLabelBucketFilter(kModel, bucket.label_)),
        bucket.count_);
  }
  for (int code = 0; code < kColorLabelCount; ++code) {
    SCOPED_TRACE(std::format("color label {}", code));
    EXPECT_EQ(
        PageTotal(project, sleeve_filter::BuildColorLabelFilter(static_cast<ColorLabel>(code))),
        stats.color_label_counts_[static_cast<size_t>(code)]);
  }
  EXPECT_EQ(PageTotal(project, sleeve_filter::BuildDeleteMarkFilter()), stats.delete_mark_count_);
}

TEST_F(StatsDimensionTest, BucketFilterStatsTotalEqualsTheClickedBucketCount) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto stats = Statistics(project);
  ASSERT_FALSE(stats.camera_stats_.empty());
  // A click reads the grid and the stats bar from one match set: its total is the bucket count.
  for (const auto& bucket : stats.camera_stats_) {
    SCOPED_TRACE("camera " + bucket.label_);
    const auto result = project.GetStorage()->GetElementStore().ReadAlbumQuery(
        0, CompileFilterPredicate(sleeve_filter::BuildCameraModelBucketFilter(Wide(bucket.label_))),
        Options(), kModel, AlbumQueryRead{.read_statistics_ = true});
    ASSERT_TRUE(result.statistics_.has_value());
    EXPECT_EQ(result.statistics_->total_photo_count_, bucket.count_);
    ASSERT_EQ(result.statistics_->camera_stats_.size(), 1u);
    EXPECT_EQ(result.statistics_->camera_stats_.front().label_, bucket.label_);
  }
}

TEST_F(StatsDimensionTest, SemanticLabelKeyInAnotherCaseSelectsTheSameBucket) {
  ProjectService project(db_path_, meta_path_);
  BuildFixture(project);
  const auto stats           = Statistics(project);
  int        landscape_count = 0;
  for (const auto& bucket : stats.label_stats_) {
    if (bucket.label_ == "landscape") {
      landscape_count = bucket.count_;
    }
  }
  ASSERT_GT(landscape_count, 0);
  // The stats bar shows label keys upper-case.
  EXPECT_EQ(PageTotal(project, sleeve_filter::BuildSemanticLabelBucketFilter(kModel, "LANDSCAPE")),
            landscape_count);
  EXPECT_EQ(PageTotal(project, sleeve_filter::BuildSemanticLabelBucketFilter("", "landscape")), 0);
}

TEST(StatsDimensionDefinitionTest, UnknownLabelsAndGroupExpressionsOfEachDimension) {
  using stats_dimension::GroupExpression;
  using stats_dimension::UnknownLabel;
  EXPECT_EQ(UnknownLabel(StatsDimensionId::kCameraModel), "(unknown)");
  EXPECT_EQ(UnknownLabel(StatsDimensionId::kLens), "(unknown)");
  EXPECT_FALSE(UnknownLabel(StatsDimensionId::kCaptureDate).has_value());
  EXPECT_FALSE(UnknownLabel(StatsDimensionId::kRating).has_value());
  EXPECT_EQ(GroupExpression(StatsDimensionId::kCameraModel).sql_,
            "COALESCE(NULLIF(s.camera_model, ''), '(unknown)')");
  EXPECT_EQ(GroupExpression(StatsDimensionId::kCaptureDate).sql_,
            "CAST(s.capture_date AS VARCHAR)");
  EXPECT_EQ(GroupExpression(StatsDimensionId::kImportDay).sql_, "CAST(s.import_day AS VARCHAR)");
  EXPECT_EQ(GroupExpression(StatsDimensionId::kSemanticLabel).sql_, "LOWER(TRIM(sl.label))");
  EXPECT_THROW((void)GroupExpression(StatsDimensionId::kColorLabel), std::invalid_argument);
  EXPECT_THROW((void)GroupExpression(static_cast<StatsDimensionId>(200)), std::invalid_argument);
}

TEST(StatsDimensionDefinitionTest, MatchSetPredicatesReadTheMatchSetAlias) {
  using stats_dimension::BucketPredicate;
  using stats_dimension::ColumnSource;
  EXPECT_EQ(BucketPredicate(StatsBucketCondition{.dimension = StatsDimensionId::kColorLabel,
                                                 .key       = ColorLabel::kRed},
                            ColumnSource::kMatchSet)
                .sql_,
            "(((s.color_labels >> 0) & 1) = 1)");
  EXPECT_EQ(BucketPredicate(StatsBucketCondition{.dimension = StatsDimensionId::kImportDay,
                                                 .key       = std::string("2026-03-08")},
                            ColumnSource::kMatchSet)
                .sql_,
            "(CAST(s.import_day AS VARCHAR) = ?)");
  EXPECT_THROW((void)BucketPredicate(StatsBucketCondition{.dimension = StatsDimensionId::kImportDay,
                                                          .key       = std::string("2026-03-08")},
                                     ColumnSource::kLiveTables),
               std::invalid_argument);
}

}  // namespace
}  // namespace alcedo
