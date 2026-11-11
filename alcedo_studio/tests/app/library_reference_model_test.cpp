//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Storage reference model (#806). A random library is built and changed through the production
// write paths (image pool, Sleeve file system, album service, pipeline history). An in-memory
// model holds the values that each write put there. After each operation:
// - the storage invariant check (#805) finds no broken row;
// - for random filter trees (AND, OR, NOT, field conditions, stats bucket conditions), the files
//   of ElementStore::ReadAlbumQuery are the files for which the model evaluates the filter to
//   TRUE under SQL three-valued logic;
// - the photo order and the groups follow the rules of AlbumQueryOptions where the model
//   computes them, and every read of one query agrees with the other reads (pages, file
//   positions, file id ranges, groups, statistics, stats bucket filters of the match set).
// At the end the project is opened again and the same checks run on the stored rows.
// Each test instance runs one fixed random seed; a failure message names it.

#include <gtest/gtest.h>

#include <QDate>
#include <QDateTime>
#include <QTimeZone>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <format>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "app/adjustment_transfer_service.hpp"
#include "app/album_browse_service.hpp"
#include "app/image_pool_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/commit_graph.hpp"
#include "image/color_label.hpp"
#include "image/image.hpp"
#include "image/metadata.hpp"
#include "library_search_test_support.hpp"
#include "sleeve/album_query.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "support/document_transfer_test_support.hpp"
#include "support/storage_invariants.hpp"
#include "support/storage_test_access.hpp"
#include "support/test_temp_directory.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
namespace {

using library_search_test::SyntheticImageSpec;
using library_search_test::SyntheticLibraryBuilder;

constexpr const char* kNewYork          = "America/New_York";
constexpr const char* kShanghai         = "Asia/Shanghai";
constexpr int         kInitialFileCount = 48;
constexpr int         kOperationCount   = 28;
constexpr int         kQueriesPerStep   = 2;

// ---------------------------------------------------------------------------------------------
// Library values
// ---------------------------------------------------------------------------------------------

/// EXIF date text and the `capture_at` value that ParseCaptureDateTime gives for it.
struct CaptureSample {
  const char*                exif_text;
  std::optional<std::string> capture_at;
};

const std::vector<CaptureSample>& CaptureSamples() {
  static const std::vector<CaptureSample> samples{
      {"", std::nullopt},
      {"2026:06:07 10:11:12", "2026-06-07 10:11:12"},
      {"2026-06-07 23:59:59", "2026-06-07 23:59:59"},
      {"2026:06:07 00:00:00", "2026-06-07 00:00:00"},
      {"2025:12:31 08:00:00", "2025-12-31 08:00:00"},
      {"2026:03:08", "2026-03-08 00:00:00"},
      {"2024:02:29 08:30:00", "2024-02-29 08:30:00"},
      // Not a real time or day: the capture columns are NULL.
      {"0000:00:00 00:00:00", std::nullopt},
      {"2026:02:29 10:00:00", std::nullopt},
  };
  return samples;
}

const std::vector<std::string>& CameraModels() {
  // Two spellings of one camera are two buckets; the empty text is the unknown bucket.
  static const std::vector<std::string> models{"Canon EOS R5", "NIKON Z 8", "", "canon eos r5",
                                               "X100V"};
  return models;
}

const std::vector<std::string>& Lenses() {
  static const std::vector<std::string> lenses{"RF50mm F1.2 L USM", "", "XF23mmF2 R WR",
                                               "NIKKOR Z 24-70mm f/4 S"};
  return lenses;
}

const std::vector<uint64_t>& IsoValues() {
  // 0 is stored as NULL.
  static const std::vector<uint64_t> values{0, 100, 400, 3200, 12800};
  return values;
}

/// File name parts. `_` and `%` are LIKE wildcard characters in SQL; the model treats every
/// character of a file name condition as itself.
const std::vector<std::wstring>& NamePrefixes() {
  static const std::vector<std::wstring> prefixes{L"IMG_",  L"DSC", L"photo%",
                                                  L"风景_", L"a_b", L"Ab"};
  return prefixes;
}

const std::vector<std::wstring>& NameExtensions() {
  static const std::vector<std::wstring> extensions{L".CR3", L".jpg", L".NEF", L".dng", L".JPG"};
  return extensions;
}

/// Import times (Unix seconds, UTC) around the daylight-saving changes of New York in 2026,
/// and around the midnights of New York and Shanghai.
const std::vector<int64_t>& SpecialImportTimes() {
  static const std::vector<int64_t> times{
      1772952000,  // 2026-03-08 06:40:00 UTC, 01:40 EST
      1772953199,  // 2026-03-08 06:59:59 UTC, 01:59:59 EST
      1772953200,  // 2026-03-08 07:00:00 UTC, 03:00 EDT
      1772942400,  // 2026-03-08 04:00:00 UTC, 23:00 EST of 03-07
      1793516400,  // 2026-11-01 05:00:00 UTC, 01:00 EDT
      1793520000,  // 2026-11-01 06:00:00 UTC, 01:00 EST
      1781020800,  // 2026-06-09 16:00:00 UTC, 00:00 Shanghai of 06-10
      1781020799,  // 2026-06-09 15:59:59 UTC
  };
  return times;
}

// ---------------------------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------------------------

/// The values that the writes of the test put into one library file.
struct ModelFile {
  sl_element_id_t            file_id  = 0;
  image_id_t                 image_id = 0;
  std::wstring               name;  ///< Element name and Image file name.
  std::string                camera;
  std::string                lens;
  std::optional<std::string> capture_at;  ///< `YYYY-MM-DD HH:MM:SS`; empty for NULL.
  std::optional<int64_t>     iso;
  int                        rating       = 0;
  ColorLabelMask             color_labels = 0;
  bool                       delete_mark  = false;
  int64_t                    added_time   = 0;  ///< Unix seconds.
  bool                       edited       = false;

  auto                       CaptureDate() const -> std::optional<std::string> {
    if (!capture_at.has_value()) {
      return std::nullopt;
    }
    return capture_at->substr(0, 10);
  }
  auto CameraLabel() const -> std::string { return camera.empty() ? "(unknown)" : camera; }
  auto LensLabel() const -> std::string { return lens.empty() ? "(unknown)" : lens; }
};

struct ModelAlbum {
  std::wstring              name;
  std::set<sl_element_id_t> files;
};

struct LibraryModel {
  std::map<sl_element_id_t, ModelFile>  files;
  std::map<sl_element_id_t, ModelAlbum> albums;
  /// Files in the order of their latest edit, newest last.
  std::vector<sl_element_id_t>          edit_order;

  /// Files of @p scope: the whole library (0) or one album.
  auto ScopeFiles(sl_element_id_t scope) const -> std::vector<const ModelFile*> {
    std::vector<const ModelFile*> out;
    if (scope == 0) {
      for (const auto& [id, file] : files) {
        out.push_back(&file);
      }
      return out;
    }
    for (const auto id : albums.at(scope).files) {
      out.push_back(&files.at(id));
    }
    return out;
  }
};

auto UtcText(int64_t unix_seconds) -> std::string {
  return std::format("{:%Y-%m-%d %H:%M:%S}",
                     std::chrono::sys_seconds{std::chrono::seconds{unix_seconds}});
}

/// Local calendar day of @p unix_seconds in the IANA zone @p time_zone. QTimeZone reads the zone
/// rules; std::chrono::locate_zone is missing from Apple libc++.
auto LocalDay(int64_t unix_seconds, const std::string& time_zone) -> std::string {
  const QTimeZone zone(QByteArray::fromStdString(time_zone));
  EXPECT_TRUE(zone.isValid()) << "Unknown time zone " << time_zone;
  const QDate day = QDateTime::fromSecsSinceEpoch(unix_seconds, zone).date();
  return std::format("{:04}-{:02}-{:02}", day.year(), day.month(), day.day());
}

auto TmText(const std::tm& value) -> std::string {
  return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", value.tm_year + 1900, value.tm_mon + 1,
                     value.tm_mday, value.tm_hour, value.tm_min, value.tm_sec);
}

auto UpperAscii(std::wstring text) -> std::wstring {
  for (auto& ch : text) {
    if (ch >= L'a' && ch <= L'z') {
      ch = static_cast<wchar_t>(ch - L'a' + L'A');
    }
  }
  return text;
}

// SQL three-valued logic: std::nullopt is NULL. A row matches only when the filter is TRUE.
using Truth = std::optional<bool>;

auto And(Truth a, Truth b) -> Truth {
  if (a == false || b == false) {
    return false;
  }
  if (!a.has_value() || !b.has_value()) {
    return std::nullopt;
  }
  return true;
}

auto Or(Truth a, Truth b) -> Truth {
  if (a == true || b == true) {
    return true;
  }
  if (!a.has_value() || !b.has_value()) {
    return std::nullopt;
  }
  return false;
}

auto Not(Truth a) -> Truth { return a.has_value() ? Truth{!*a} : std::nullopt; }

template <typename T>
auto Compare(const std::optional<T>& column, CompareOp op, const T& value,
             const std::optional<T>& second) -> Truth {
  if (!column.has_value()) {
    return std::nullopt;
  }
  const T& v = *column;
  switch (op) {
    case CompareOp::EQUALS:
      return v == value;
    case CompareOp::NOT_EQUALS:
      return v != value;
    case CompareOp::GREATER_THAN:
      return v > value;
    case CompareOp::LESS_THAN:
      return v < value;
    case CompareOp::GREATER_EQUAL:
      return v >= value;
    case CompareOp::LESS_EQUAL:
      return v <= value;
    case CompareOp::BETWEEN:
      return v >= value && v <= *second;
    default:
      break;
  }
  ADD_FAILURE() << "compare operator " << static_cast<int>(op) << " has no model";
  return std::nullopt;
}

auto TextCondition(const std::wstring& column, CompareOp op, const std::wstring& value) -> Truth {
  switch (op) {
    case CompareOp::EQUALS:
      return column == value;
    case CompareOp::NOT_EQUALS:
      return column != value;
    case CompareOp::CONTAINS:
      return column.find(value) != std::wstring::npos;
    case CompareOp::NOT_CONTAINS:
      return column.find(value) == std::wstring::npos;
    case CompareOp::STARTS_WITH:
      return column.starts_with(value);
    case CompareOp::ENDS_WITH:
      return column.ends_with(value);
    default:
      break;
  }
  ADD_FAILURE() << "text operator " << static_cast<int>(op) << " has no model";
  return std::nullopt;
}

auto EvaluateCondition(const FieldCondition& condition, const ModelFile& file) -> Truth {
  const auto int_value  = [&](const FilterValue& value) { return std::get<int64_t>(value); };
  const auto second_int = [&]() -> std::optional<int64_t> {
    if (!condition.second_value_.has_value()) {
      return std::nullopt;
    }
    return int_value(*condition.second_value_);
  };
  const auto second_tm = [&]() -> std::optional<std::string> {
    if (!condition.second_value_.has_value()) {
      return std::nullopt;
    }
    return TmText(std::get<std::tm>(*condition.second_value_));
  };
  switch (condition.field_) {
    case FilterField::Rating:
      return Compare<int64_t>(int64_t{file.rating}, condition.op_, int_value(condition.value_),
                              second_int());
    case FilterField::ExifISO:
      return Compare<int64_t>(file.iso, condition.op_, int_value(condition.value_), second_int());
    case FilterField::FileId:
      return Compare<int64_t>(static_cast<int64_t>(file.file_id), condition.op_,
                              int_value(condition.value_), second_int());
    case FilterField::FileName:
      return TextCondition(file.name, condition.op_, std::get<std::wstring>(condition.value_));
    case FilterField::FileExtension:
      return TextCondition(UpperAscii(file.name), condition.op_,
                           std::get<std::wstring>(condition.value_));
    case FilterField::ExifCameraModel:
      return TextCondition(conv::FromBytes(file.camera), condition.op_,
                           std::get<std::wstring>(condition.value_));
    case FilterField::CaptureDate:
      return Compare<std::string>(file.capture_at, condition.op_,
                                  TmText(std::get<std::tm>(condition.value_)), second_tm());
    case FilterField::ImportDate:
      return Compare<std::string>(UtcText(file.added_time), condition.op_,
                                  TmText(std::get<std::tm>(condition.value_)), second_tm());
    default:
      break;
  }
  ADD_FAILURE() << "filter field " << static_cast<int>(condition.field_) << " has no model";
  return std::nullopt;
}

auto EvaluateBucket(const StatsBucketCondition& bucket, const ModelFile& file) -> Truth {
  const bool unknown = std::holds_alternative<std::monostate>(bucket.key);
  switch (bucket.dimension) {
    case StatsDimensionId::kCameraModel:
      return file.CameraLabel() == (unknown ? "(unknown)" : std::get<std::string>(bucket.key));
    case StatsDimensionId::kLens:
      return file.LensLabel() == (unknown ? "(unknown)" : std::get<std::string>(bucket.key));
    case StatsDimensionId::kRating:
      return !unknown && file.rating == std::get<int64_t>(bucket.key);
    case StatsDimensionId::kCaptureDate: {
      const auto day = file.CaptureDate();
      if (unknown) {
        return !day.has_value();
      }
      if (!day.has_value()) {
        return std::nullopt;  // CAST(NULL AS VARCHAR) = '<day>'
      }
      return *day == std::get<std::string>(bucket.key);
    }
    case StatsDimensionId::kImportDay:
      if (unknown) {
        return false;  // every model file has an import time
      }
      return LocalDay(file.added_time, bucket.time_zone) == std::get<std::string>(bucket.key);
    case StatsDimensionId::kColorLabel:
      return HasColorLabel(file.color_labels, std::get<ColorLabel>(bucket.key));
    case StatsDimensionId::kDeleteMark:
      return file.delete_mark;
    case StatsDimensionId::kSemanticLabel:
      break;
  }
  ADD_FAILURE() << "stats dimension " << static_cast<int>(bucket.dimension) << " has no model";
  return std::nullopt;
}

auto Evaluate(const FilterNode& node, const ModelFile& file) -> Truth {
  switch (node.type_) {
    case FilterNode::Type::Condition:
      return EvaluateCondition(*node.condition_, file);
    case FilterNode::Type::StatsBucket:
      return EvaluateBucket(*node.stats_bucket_, file);
    case FilterNode::Type::Logical: {
      // FilterSQLCompiler: NOT of several children is NOT (c1 AND c2 ...).
      Truth combined = node.op_ == FilterOp::OR ? Truth{false} : Truth{true};
      for (const auto& child : node.children_) {
        const auto value = Evaluate(child, file);
        combined         = node.op_ == FilterOp::OR ? Or(combined, value) : And(combined, value);
      }
      return node.op_ == FilterOp::NOT ? Not(combined) : combined;
    }
    case FilterNode::Type::LibrarySearch:
      break;
  }
  ADD_FAILURE() << "filter node type " << static_cast<int>(node.type_) << " has no model";
  return std::nullopt;
}

/// Files of @p scope that @p filter selects, in file id order.
auto ExpectedMatches(const LibraryModel& model, sl_element_id_t scope,
                     const std::optional<FilterNode>& filter) -> std::vector<const ModelFile*> {
  std::vector<const ModelFile*> out;
  for (const auto* file : model.ScopeFiles(scope)) {
    if (!filter.has_value() || Evaluate(*filter, *file) == true) {
      out.push_back(file);
    }
  }
  std::sort(out.begin(), out.end(),
            [](const ModelFile* a, const ModelFile* b) { return a->file_id < b->file_id; });
  return out;
}

// ---------------------------------------------------------------------------------------------
// Expected order and groups of the options that the model computes
// ---------------------------------------------------------------------------------------------

struct ExpectedGroup {
  AlbumGroupKey                key;
  std::vector<sl_element_id_t> files;
};

/// Group key of @p file for @p field (kNone gives one unnamed group).
auto GroupKeyOf(const ModelFile& file, AlbumGroupField field) -> AlbumGroupKey {
  switch (field) {
    case AlbumGroupField::kRating:
      return int64_t{file.rating};
    case AlbumGroupField::kCameraModel:
      return file.camera.empty() ? AlbumGroupKey{} : AlbumGroupKey{file.camera};
    case AlbumGroupField::kLens:
      return file.lens.empty() ? AlbumGroupKey{} : AlbumGroupKey{file.lens};
    case AlbumGroupField::kCaptureDay: {
      const auto day = file.CaptureDate();
      return day.has_value() ? AlbumGroupKey{*day} : AlbumGroupKey{};
    }
    default:
      return AlbumGroupKey{};
  }
}

/**
 * @brief Groups and photo order of @p matches for the options whose rules the model computes:
 *        flat or grouped by rating, camera, lens, or capture day, with no photo sort or a
 *        rating sort. Empty for other options.
 *
 * Rules (AlbumQueryOptions): rating groups highest first; camera and lens groups ascending by
 * key; capture days newest first; the unknown group last. Photos: the selected sort, then in a
 * capture day group the capture time newest first, then the file id ascending.
 */
auto ExpectedGroups(const std::vector<const ModelFile*>& matches, const AlbumQueryOptions& options)
    -> std::optional<std::vector<ExpectedGroup>> {
  const bool rating_sort = options.sort_field_ == AlbumSortField::kRating;
  if (options.sort_field_ != AlbumSortField::kNone && !rating_sort) {
    return std::nullopt;
  }
  const auto group = options.group_field_;
  if (group != AlbumGroupField::kNone && group != AlbumGroupField::kRating &&
      group != AlbumGroupField::kCameraModel && group != AlbumGroupField::kLens &&
      group != AlbumGroupField::kCaptureDay) {
    return std::nullopt;
  }
  if (group == AlbumGroupField::kRating && rating_sort) {
    return std::nullopt;  // groups follow the sort direction; not modelled
  }
  const bool descending = options.sort_direction_ == SortDirection::kDescending;

  auto       photos     = matches;
  std::sort(photos.begin(), photos.end(), [&](const ModelFile* a, const ModelFile* b) {
    if (rating_sort && a->rating != b->rating) {
      return descending ? a->rating > b->rating : a->rating < b->rating;
    }
    if (!rating_sort && group == AlbumGroupField::kCaptureDay && a->capture_at != b->capture_at) {
      return a->capture_at > b->capture_at;  // newest first; NULL only in the unknown group
    }
    return a->file_id < b->file_id;
  });

  std::vector<ExpectedGroup> groups;
  for (const auto* photo : photos) {
    const auto key = GroupKeyOf(*photo, group);
    auto       it  = std::find_if(groups.begin(), groups.end(),
                                  [&](const ExpectedGroup& g) { return g.key == key; });
    if (it == groups.end()) {
      groups.push_back(ExpectedGroup{.key = key, .files = {}});
      it = std::prev(groups.end());
    }
    it->files.push_back(photo->file_id);
  }
  std::stable_sort(groups.begin(), groups.end(),
                   [&](const ExpectedGroup& a, const ExpectedGroup& b) {
                     const bool a_unknown = std::holds_alternative<std::monostate>(a.key);
                     const bool b_unknown = std::holds_alternative<std::monostate>(b.key);
                     if (a_unknown != b_unknown) {
                       return b_unknown;
                     }
                     if (a_unknown) {
                       return false;
                     }
                     if (group == AlbumGroupField::kRating) {
                       return std::get<int64_t>(a.key) > std::get<int64_t>(b.key);
                     }
                     if (group == AlbumGroupField::kCaptureDay) {
                       return std::get<std::string>(a.key) > std::get<std::string>(b.key);
                     }
                     return std::get<std::string>(a.key) < std::get<std::string>(b.key);
                   });
  if (group == AlbumGroupField::kNone && !groups.empty()) {
    groups.front().key = AlbumGroupKey{};
  }
  return groups;
}

auto KeyText(const AlbumGroupKey& key) -> std::string {
  if (const auto* text = std::get_if<std::string>(&key)) {
    return "'" + *text + "'";
  }
  if (const auto* number = std::get_if<int64_t>(&key)) {
    return std::to_string(*number);
  }
  return "(unknown)";
}

auto DescribeValue(const FilterValue& value) -> std::string {
  if (const auto* number = std::get_if<int64_t>(&value)) {
    return std::to_string(*number);
  }
  if (const auto* text = std::get_if<std::wstring>(&value)) {
    return "'" + conv::ToBytes(*text) + "'";
  }
  if (const auto* time = std::get_if<std::tm>(&value)) {
    return TmText(*time);
  }
  return "?";
}

auto DescribeFilter(const FilterNode& node) -> std::string {
  switch (node.type_) {
    case FilterNode::Type::Condition: {
      const auto& c    = *node.condition_;
      auto        text = std::format("field{} op{} {}", static_cast<int>(c.field_),
                                     static_cast<int>(c.op_), DescribeValue(c.value_));
      if (c.second_value_.has_value()) {
        text += " and " + DescribeValue(*c.second_value_);
      }
      return text;
    }
    case FilterNode::Type::StatsBucket: {
      const auto& b   = *node.stats_bucket_;
      std::string key = "(unknown)";
      if (const auto* text = std::get_if<std::string>(&b.key)) {
        key = "'" + *text + "'";
      } else if (const auto* number = std::get_if<int64_t>(&b.key)) {
        key = std::to_string(*number);
      } else if (const auto* code = std::get_if<ColorLabel>(&b.key)) {
        key = std::format("label{}", static_cast<int>(*code));
      }
      return std::format("bucket{} {} {}", static_cast<int>(b.dimension), key, b.time_zone);
    }
    case FilterNode::Type::Logical: {
      std::string text =
          node.op_ == FilterOp::AND ? "AND(" : (node.op_ == FilterOp::OR ? "OR(" : "NOT(");
      for (size_t i = 0; i < node.children_.size(); ++i) {
        text += (i == 0 ? "" : ", ") + DescribeFilter(node.children_[i]);
      }
      return text + ")";
    }
    case FilterNode::Type::LibrarySearch:
      break;
  }
  return "?";
}

auto DescribeFile(const ModelFile& file) -> std::string {
  return std::format(
      "file {} '{}' camera '{}' lens '{}' capture {} iso {} rating {} labels {} delete {} "
      "added {} edited {}",
      file.file_id, conv::ToBytes(file.name), file.camera, file.lens,
      file.capture_at.value_or("NULL"), file.iso.has_value() ? std::to_string(*file.iso) : "NULL",
      file.rating, static_cast<int>(file.color_labels), file.delete_mark, UtcText(file.added_time),
      file.edited);
}

auto BucketMap(const std::vector<StorageStatsBucket>& buckets) -> std::map<std::string, int> {
  std::map<std::string, int> out;
  for (const auto& bucket : buckets) {
    out[bucket.label_] += bucket.count_;
  }
  return out;
}

// ---------------------------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------------------------

class LibraryReferenceModelTest : public ::testing::TestWithParam<uint64_t> {
 protected:
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    TimeProvider::Refresh();
    rng_.seed(GetParam());
    dir_ = temp_dir_.Path();
    OpenProject();
  }

  void TearDown() override {
    pipelines_.reset();
    project_.reset();
  }

  void OpenProject() {
    pipelines_.reset();
    project_.reset();
    project_   = std::make_unique<ProjectService>(dir_ / "library.db", dir_ / "library.json");
    pipelines_ = std::make_unique<PipelineMgmtService>(project_->GetStorage());
  }

  auto Browse() -> AlbumBrowseService& { return *project_->GetAlbumBrowseService(); }

  // -- Random values ---------------------------------------------------------------------------

  auto Uniform(int64_t low, int64_t high) -> int64_t {
    return std::uniform_int_distribution<int64_t>(low, high)(rng_);
  }
  auto Chance(double probability) -> bool { return std::bernoulli_distribution(probability)(rng_); }
  template <typename T>
  auto Pick(const std::vector<T>& values) -> const T& {
    return values[static_cast<size_t>(Uniform(0, static_cast<int64_t>(values.size()) - 1))];
  }
  auto PickFile() -> const ModelFile& {
    auto it = model_.files.begin();
    std::advance(it, Uniform(0, static_cast<int64_t>(model_.files.size()) - 1));
    return it->second;
  }
  auto PickFiles(int64_t max_count) -> std::vector<sl_element_id_t> {
    std::set<sl_element_id_t> ids;
    const auto                count = Uniform(1, max_count);
    for (int64_t i = 0; i < count && !model_.files.empty(); ++i) {
      ids.insert(PickFile().file_id);
    }
    return {ids.begin(), ids.end()};
  }
  auto PickTm() -> std::tm {
    // A capture time of the samples, or an import time, shifted by up to one day.
    int64_t seconds = 0;
    if (Chance(0.5)) {
      seconds = Pick(SpecialImportTimes());
    } else {
      const auto& samples = CaptureSamples();
      const auto& sample  = samples[static_cast<size_t>(Uniform(1, 6))];
      std::tm     parsed{};
      const auto  text = *sample.capture_at;
      parsed.tm_year   = std::stoi(text.substr(0, 4)) - 1900;
      parsed.tm_mon    = std::stoi(text.substr(5, 2)) - 1;
      parsed.tm_mday   = std::stoi(text.substr(8, 2));
      parsed.tm_hour   = std::stoi(text.substr(11, 2));
      parsed.tm_min    = std::stoi(text.substr(14, 2));
      parsed.tm_sec    = std::stoi(text.substr(17, 2));
      if (Chance(0.5)) {
        return parsed;
      }
      seconds = std::chrono::duration_cast<std::chrono::seconds>(
                    (std::chrono::sys_days{std::chrono::year_month_day{
                         std::chrono::year{parsed.tm_year + 1900},
                         std::chrono::month{static_cast<unsigned>(parsed.tm_mon + 1)},
                         std::chrono::day{static_cast<unsigned>(parsed.tm_mday)}}})
                        .time_since_epoch())
                    .count() +
                parsed.tm_hour * 3600 + parsed.tm_min * 60 + parsed.tm_sec;
    }
    seconds += Uniform(-86400, 86400);
    const auto time = std::chrono::sys_seconds{std::chrono::seconds{seconds}};
    const auto day  = std::chrono::floor<std::chrono::days>(time);
    const std::chrono::year_month_day ymd{day};
    const std::chrono::hh_mm_ss       hms{time - day};
    std::tm                           value{};
    value.tm_year = static_cast<int>(ymd.year()) - 1900;
    value.tm_mon  = static_cast<int>(static_cast<unsigned>(ymd.month())) - 1;
    value.tm_mday = static_cast<int>(static_cast<unsigned>(ymd.day()));
    value.tm_hour = static_cast<int>(hms.hours().count());
    value.tm_min  = static_cast<int>(hms.minutes().count());
    value.tm_sec  = static_cast<int>(hms.seconds().count());
    return value;
  }

  // -- Random filters --------------------------------------------------------------------------

  auto PickNameNeedle() -> std::wstring {
    if (Chance(0.4) || model_.files.empty()) {
      return Pick(std::vector<std::wstring>{L"_", L"%", L"IMG", L"风景", L"a_b", L"0", L"Ab"});
    }
    const auto& name  = PickFile().name;
    const auto  begin = Uniform(0, static_cast<int64_t>(name.size()) - 1);
    const auto length = Uniform(1, std::min<int64_t>(4, static_cast<int64_t>(name.size()) - begin));
    return name.substr(static_cast<size_t>(begin), static_cast<size_t>(length));
  }

  auto Condition(FilterField field, CompareOp op, FilterValue value,
                 std::optional<FilterValue> second = std::nullopt) -> FilterNode {
    return FilterNode{.type_      = FilterNode::Type::Condition,
                      .op_        = FilterOp::AND,
                      .children_  = {},
                      .condition_ = FieldCondition{.field_        = field,
                                                   .op_           = op,
                                                   .value_        = std::move(value),
                                                   .second_value_ = std::move(second)}};
  }

  auto RandomNumericCondition(FilterField field, const std::vector<int64_t>& values) -> FilterNode {
    const std::vector<CompareOp> ops{CompareOp::EQUALS,        CompareOp::NOT_EQUALS,
                                     CompareOp::GREATER_THAN,  CompareOp::LESS_THAN,
                                     CompareOp::GREATER_EQUAL, CompareOp::LESS_EQUAL,
                                     CompareOp::BETWEEN};
    const auto                   op = Pick(ops);
    if (op == CompareOp::BETWEEN) {
      return Condition(field, op, Pick(values), FilterValue{Pick(values)});
    }
    return Condition(field, op, Pick(values));
  }

  auto RandomCondition() -> FilterNode {
    const std::vector<CompareOp> text_ops{CompareOp::CONTAINS, CompareOp::NOT_CONTAINS,
                                          CompareOp::STARTS_WITH, CompareOp::ENDS_WITH};
    const std::vector<CompareOp> time_ops{CompareOp::GREATER_THAN, CompareOp::LESS_THAN,
                                          CompareOp::GREATER_EQUAL, CompareOp::LESS_EQUAL,
                                          CompareOp::BETWEEN};
    switch (Uniform(0, 7)) {
      case 0:
        return RandomNumericCondition(FilterField::Rating, {0, 1, 2, 3, 4, 5});
      case 1:
        return RandomNumericCondition(FilterField::ExifISO, {100, 400, 800, 3200, 12800});
      case 2:
        return Condition(FilterField::FileId, CompareOp::EQUALS,
                         static_cast<int64_t>(PickFile().file_id));
      case 3:
        return Condition(FilterField::FileName, Pick(text_ops), PickNameNeedle());
      case 4:
        return Condition(FilterField::FileExtension, CompareOp::ENDS_WITH,
                         Pick(std::vector<std::wstring>{L".JPG", L".CR3", L".DNG", L".NEF"}));
      case 5: {
        const std::vector<CompareOp> ops{CompareOp::EQUALS, CompareOp::NOT_EQUALS,
                                         CompareOp::CONTAINS};
        const auto                   op = Pick(ops);
        if (op == CompareOp::CONTAINS) {
          return Condition(FilterField::ExifCameraModel, op,
                           Pick(std::vector<std::wstring>{L"anon", L"Z", L"r5", L" "}));
        }
        return Condition(FilterField::ExifCameraModel, op, conv::FromBytes(Pick(CameraModels())));
      }
      default: {
        const auto field = Uniform(6, 7) == 6 ? FilterField::CaptureDate : FilterField::ImportDate;
        const auto op    = Pick(time_ops);
        if (op == CompareOp::BETWEEN) {
          auto low  = PickTm();
          auto high = PickTm();
          if (TmText(low) > TmText(high) && Chance(0.8)) {
            std::swap(low, high);
          }
          return Condition(field, op, low, FilterValue{high});
        }
        return Condition(field, op, PickTm());
      }
    }
  }

  auto RandomBucket() -> FilterNode {
    switch (Uniform(0, 7)) {
      case 0: {
        auto labels = CameraModels();
        labels.push_back("(unknown)");
        return sleeve_filter::BuildCameraModelBucketFilter(conv::FromBytes(Pick(labels)));
      }
      case 1: {
        auto labels = Lenses();
        labels.push_back("(unknown)");
        return sleeve_filter::BuildLensBucketFilter(conv::FromBytes(Pick(labels)));
      }
      case 2:
        return sleeve_filter::BuildRatingBucketFilter(std::to_wstring(Uniform(0, 5)));
      case 3: {
        if (Chance(0.25)) {
          return sleeve_filter::BuildCaptureDateUnknownFilter();
        }
        const auto day = PickFile().CaptureDate().value_or("2026-06-07");
        return sleeve_filter::BuildCaptureDateBucketFilter(conv::FromBytes(day));
      }
      case 4: {
        if (Chance(0.2)) {
          return sleeve_filter::BuildImportDateUnknownFilter();
        }
        const std::string zone = Chance(0.5) ? kNewYork : kShanghai;
        return sleeve_filter::BuildImportDateBucketFilter(
            conv::FromBytes(LocalDay(PickFile().added_time, zone)), zone);
      }
      case 5:
      case 6:
        return sleeve_filter::BuildColorLabelFilter(
            static_cast<ColorLabel>(Uniform(0, kColorLabelCount - 1)));
      default:
        return sleeve_filter::BuildDeleteMarkFilter();
    }
  }

  auto RandomFilter(int depth) -> FilterNode {
    if (depth == 0 || Chance(0.35)) {
      return Chance(0.5) ? RandomCondition() : RandomBucket();
    }
    const auto op = Pick(std::vector<FilterOp>{FilterOp::AND, FilterOp::OR, FilterOp::NOT});
    FilterNode node{.type_ = FilterNode::Type::Logical, .op_ = op, .children_ = {}};
    const auto children = Uniform(1, op == FilterOp::NOT ? 2 : 3);
    for (int64_t i = 0; i < children; ++i) {
      node.children_.push_back(RandomFilter(depth - 1));
    }
    return node;
  }

  auto RandomOptions() -> AlbumQueryOptions {
    const std::vector<AlbumQueryOptions> options{
        // Options whose order and groups the model computes.
        {},
        {.sort_field_ = AlbumSortField::kRating, .sort_direction_ = SortDirection::kDescending},
        {.sort_field_ = AlbumSortField::kRating, .sort_direction_ = SortDirection::kAscending},
        {.group_field_ = AlbumGroupField::kRating},
        {.group_field_ = AlbumGroupField::kCameraModel},
        {.sort_field_     = AlbumSortField::kRating,
         .sort_direction_ = SortDirection::kDescending,
         .group_field_    = AlbumGroupField::kCameraModel},
        {.group_field_ = AlbumGroupField::kLens},
        {.group_field_ = AlbumGroupField::kCaptureDay},
        // Options checked only for agreement between the reads.
        {.sort_field_ = AlbumSortField::kCaptureTime},
        {.sort_field_ = AlbumSortField::kImportTime, .sort_direction_ = SortDirection::kDescending},
        {.sort_field_     = AlbumSortField::kCameraModel,
         .sort_direction_ = SortDirection::kDescending},
        {.group_field_ = AlbumGroupField::kImportDay, .local_day_time_zone_ = kNewYork},
        {.sort_field_          = AlbumSortField::kImportTime,
         .group_field_         = AlbumGroupField::kImportDay,
         .local_day_time_zone_ = kShanghai},
        {.group_field_ = AlbumGroupField::kEditDay, .local_day_time_zone_ = kNewYork},
        {.sort_field_ = AlbumSortField::kEditTime, .sort_direction_ = SortDirection::kDescending},
        {.sort_field_ = AlbumSortField::kCaptureTime, .group_field_ = AlbumGroupField::kCaptureDay},
    };
    return Pick(options);
  }

  auto RandomScope() -> sl_element_id_t {
    if (model_.albums.empty() || Chance(0.6)) {
      return 0;
    }
    auto it = model_.albums.begin();
    std::advance(it, Uniform(0, static_cast<int64_t>(model_.albums.size()) - 1));
    return it->first;
  }

  // -- Writes ----------------------------------------------------------------------------------

  /// Create @p count files as the import does (Image row, file element, history root).
  void AddFiles(int count) {
    std::vector<SyntheticImageSpec> specs;
    std::vector<ModelFile>          files;
    for (int i = 0; i < count; ++i) {
      const auto& capture = CaptureSamples()[static_cast<size_t>(
          Uniform(0, static_cast<int64_t>(CaptureSamples().size()) - 1))];
      ModelFile   file;
      file.name =
          Pick(NamePrefixes()) + std::format(L"{:04}", next_name_++) + Pick(NameExtensions());
      file.camera     = Pick(CameraModels());
      file.lens       = Pick(Lenses());
      file.capture_at = capture.capture_at;
      const auto iso  = Pick(IsoValues());
      file.iso        = iso > 0 ? std::optional<int64_t>{static_cast<int64_t>(iso)} : std::nullopt;
      file.rating     = static_cast<int>(Uniform(0, 5));
      file.added_time =
          Chance(0.3) ? Pick(SpecialImportTimes()) : 1767225600 + Uniform(0, 365 * 86400);
      specs.push_back(SyntheticImageSpec{.file_name_   = file.name,
                                         .image_path_  = L"D:/library/" + file.name,
                                         .make_        = "Make",
                                         .model_       = file.camera,
                                         .lens_        = file.lens,
                                         .date_time_   = capture.exif_text,
                                         .iso_         = iso,
                                         .rating_      = file.rating,
                                         .is_raw_file_ = false,
                                         .added_time_ = static_cast<std::time_t>(file.added_time)});
      files.push_back(std::move(file));
    }
    SyntheticLibraryBuilder builder(*project_);
    const auto              ids = builder.AddFiles(specs);
    ASSERT_EQ(ids.size(), files.size());
    for (const auto id : ids) {
      pipelines_->InitializeImageRoot(id, CreateDefaultPipelineDocument(), nullptr);
    }
    // The image ids are identities that the pool assigned; read them from the bindings.
    const auto rows = Browse().ReadAlbumFileRows(ids);
    ASSERT_EQ(rows.size(), ids.size());
    for (size_t i = 0; i < ids.size(); ++i) {
      ASSERT_EQ(rows[i].file_id_, ids[i]);
      files[i].file_id  = ids[i];
      files[i].image_id = rows[i].image_id_;
      model_.files.emplace(ids[i], std::move(files[i]));
    }
  }

  /// Change one image in the pool and write it, as ImageController does (Write_NoSync, then one
  /// SyncWithStorage). ImagePoolService::Write is not used: it fails on every call (#811).
  void WriteImage(image_id_t image_id, std::function<void(std::shared_ptr<Image>)> change) {
    auto image_pool = project_->GetImagePoolService();
    image_pool->Write_NoSync<void>(image_id, std::move(change));
    const auto status = image_pool->SyncWithStorage();
    ASSERT_TRUE(status.failed_images_.empty()) << "image " << image_id;
  }

  void SetRating(sl_element_id_t id, int rating) {
    auto& file = model_.files.at(id);
    WriteImage(file.image_id, [rating](const std::shared_ptr<Image>& image) {
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
    file.rating = rating;
  }

  void SetColorLabel(sl_element_id_t id, ColorLabel code, bool on) {
    auto& file = model_.files.at(id);
    WriteImage(file.image_id,
               [code, on](const std::shared_ptr<Image>& image) { image->SetColorLabel(code, on); });
    file.color_labels = WithColorLabel(file.color_labels, code, on);
  }

  void SetDeleteMark(sl_element_id_t id, bool on) {
    auto& file = model_.files.at(id);
    WriteImage(file.image_id,
               [on](const std::shared_ptr<Image>& image) { image->SetDeleteMark(on); });
    file.delete_mark = on;
  }

  void CreateAlbum() {
    const auto name   = std::format(L"Album {}", next_album_++);
    const auto folder = Browse().CreateFolder(L"/", name);
    ASSERT_TRUE(folder.has_value());
    model_.albums.emplace(folder->folder_id_, ModelAlbum{.name = name, .files = {}});
  }

  void LinkToAlbum(sl_element_id_t album, const std::vector<sl_element_id_t>& ids) {
    const auto result = Browse().LinkFilesToFolder(ids, album);
    ASSERT_TRUE(result.failed_element_ids_.empty());
    model_.albums.at(album).files.insert(ids.begin(), ids.end());
  }

  void RemoveFromAlbum(sl_element_id_t album, const std::vector<sl_element_id_t>& ids) {
    auto&                        members = model_.albums.at(album).files;
    std::vector<sl_element_id_t> present;
    for (const auto id : ids) {
      if (members.contains(id)) {
        present.push_back(id);
      }
    }
    if (present.empty()) {
      return;
    }
    const auto result = Browse().DeleteFilesInFolderByElementIds(album, present);
    ASSERT_TRUE(result.failed_element_ids_.empty());
    for (const auto id : present) {
      members.erase(id);
    }
  }

  void DeleteAlbum(sl_element_id_t album) {
    ASSERT_TRUE(Browse().DeleteFolder(std::filesystem::path(L"/") / model_.albums.at(album).name));
    model_.albums.erase(album);
  }

  /// Delete files from the library with the steps of ImageController's delete batch: the file
  /// elements, then their histories, then their Image rows.
  void DeleteFromLibrary(const std::vector<sl_element_id_t>& ids) {
    const auto result = Browse().DeleteFilesInFolderByElementIds(0, ids);
    ASSERT_TRUE(result.failed_element_ids_.empty());
    ASSERT_EQ(result.deleted_files_.size(), ids.size());
    std::vector<image_id_t> image_ids;
    for (const auto id : ids) {
      image_ids.push_back(model_.files.at(id).image_id);
    }
    pipelines_->DeletePipelines(ids);
    auto image_pool = project_->GetImagePoolService();
    image_pool->RemoveBatch(image_ids);
    image_pool->SyncWithStorage();

    for (const auto id : ids) {
      model_.files.erase(id);
      for (auto& [album_id, album] : model_.albums) {
        album.files.erase(id);
      }
      std::erase(model_.edit_order, id);
    }
  }

  /// Paste an exposure adjustment as a new Version: the library Paste of
  /// AdjustmentTransferApplyCoordinator (stored history, private copy, one persist).
  void PasteEdit(sl_element_id_t id) {
    const auto base = pipelines_->LoadHistorySnapshot(id);
    ASSERT_TRUE(base.graph_ && base.root_);
    CommitGraph graph  = *base.graph_;
    const auto  pasted = AdjustmentTransferService::PasteAsRootRelativeVersion(
        graph, base.root_->document,
        test::MakeExposureTransferPackage(0.25 * static_cast<double>(Uniform(1, 8))), "Pasted");
    ASSERT_TRUE(pasted.pasted) << pasted.error;
    ASSERT_NE(pipelines_->PersistHistory(base, graph), nullptr);
    model_.files.at(id).edited = true;
    std::erase(model_.edit_order, id);
    model_.edit_order.push_back(id);
  }

  /// One random library operation; returns its description for the failure trace.
  auto ApplyRandomOperation() -> std::string {
    const auto file_count = static_cast<int64_t>(model_.files.size());
    switch (Uniform(0, 10)) {
      case 0: {
        const auto count = static_cast<int>(Uniform(1, 6));
        AddFiles(count);
        return std::format("add {} files", count);
      }
      case 1:
      case 2: {
        const auto ids    = PickFiles(5);
        const auto rating = static_cast<int>(Uniform(0, 5));
        for (const auto id : ids) {
          SetRating(id, rating);
        }
        return std::format("rate {} files {}", ids.size(), rating);
      }
      case 3: {
        const auto ids  = PickFiles(5);
        const auto code = static_cast<ColorLabel>(Uniform(0, kColorLabelCount - 1));
        const bool on   = Chance(0.7);
        for (const auto id : ids) {
          SetColorLabel(id, code, on);
        }
        return std::format("color label {} {} on {} files", static_cast<int>(code), on, ids.size());
      }
      case 4: {
        const auto ids = PickFiles(4);
        const bool on  = Chance(0.6);
        for (const auto id : ids) {
          SetDeleteMark(id, on);
        }
        return std::format("Delete mark {} on {} files", on, ids.size());
      }
      case 5:
        CreateAlbum();
        return "create album";
      case 6: {
        if (model_.albums.empty()) {
          CreateAlbum();
        }
        auto album_it = model_.albums.begin();
        std::advance(album_it, Uniform(0, static_cast<int64_t>(model_.albums.size()) - 1));
        const auto album = album_it->first;
        const auto ids   = PickFiles(8);
        LinkToAlbum(album, ids);
        return std::format("link {} files to album {}", ids.size(), album);
      }
      case 7: {
        if (model_.albums.empty()) {
          return "no album to change";
        }
        const auto album = model_.albums.begin()->first;
        const auto ids   = PickFiles(6);
        RemoveFromAlbum(album, ids);
        return std::format("remove files from album {}", album);
      }
      case 8: {
        if (model_.albums.size() < 2 || !Chance(0.5)) {
          return "keep albums";
        }
        const auto album = model_.albums.rbegin()->first;
        DeleteAlbum(album);
        return std::format("delete album {}", album);
      }
      case 9: {
        if (file_count < 8) {
          return "keep files";
        }
        const auto ids = PickFiles(3);
        DeleteFromLibrary(ids);
        return std::format("delete {} files from the library", ids.size());
      }
      default: {
        const auto id = PickFile().file_id;
        PasteEdit(id);
        return std::format("paste edit on file {}", id);
      }
    }
  }

  // -- Checks ----------------------------------------------------------------------------------

  void CheckRows(const std::vector<AlbumQueryRow>& rows) {
    for (const auto& row : rows) {
      const auto it = model_.files.find(row.photo_.file_id_);
      ASSERT_NE(it, model_.files.end()) << "row of unknown file " << row.photo_.file_id_;
      const auto& file = it->second;
      SCOPED_TRACE(std::format("file {}", file.file_id));
      EXPECT_EQ(row.photo_.image_id_, file.image_id);
      EXPECT_EQ(conv::FromBytes(row.photo_.file_name_), file.name);
      EXPECT_EQ(row.photo_.camera_model_, file.camera);
      EXPECT_EQ(row.photo_.lens_, file.lens);
      EXPECT_EQ(row.photo_.capture_date_, file.CaptureDate().value_or(""));
      EXPECT_EQ(row.photo_.rating_, file.rating);
      EXPECT_EQ(row.photo_.color_labels_, file.color_labels);
      EXPECT_EQ(row.photo_.delete_mark_, file.delete_mark);
      EXPECT_EQ(row.photo_.added_time_, std::optional<int64_t>{file.added_time});
      EXPECT_EQ(row.edited_at_ns_.has_value(), file.edited);
      EXPECT_EQ(row.edit_version_name_.empty(), !file.edited);
    }
  }

  void CheckStatistics(const FolderStatsView& stats, const std::vector<const ModelFile*>& matches,
                       const AlbumQueryOptions& options) {
    std::map<std::string, int>        cameras, lenses, ratings, dates, import_days;
    std::array<int, kColorLabelCount> color_counts{};
    int                               delete_count = 0;
    for (const auto* file : matches) {
      ++cameras[file->CameraLabel()];
      ++lenses[file->LensLabel()];
      ++ratings[std::to_string(file->rating)];
      ++dates[file->CaptureDate().value_or("")];
      if (!options.local_day_time_zone_.empty()) {
        ++import_days[LocalDay(file->added_time, options.local_day_time_zone_)];
      }
      for (int code = 0; code < kColorLabelCount; ++code) {
        color_counts[static_cast<size_t>(code)] +=
            HasColorLabel(file->color_labels, static_cast<ColorLabel>(code)) ? 1 : 0;
      }
      delete_count += file->delete_mark ? 1 : 0;
    }
    EXPECT_EQ(stats.total_photo_count_, static_cast<int>(matches.size()));
    EXPECT_EQ(BucketMap(stats.camera_stats_), cameras);
    EXPECT_EQ(BucketMap(stats.lens_stats_), lenses);
    EXPECT_EQ(BucketMap(stats.rating_stats_), ratings);
    EXPECT_EQ(BucketMap(stats.date_stats_), dates);
    if (!options.local_day_time_zone_.empty()) {
      EXPECT_EQ(BucketMap(stats.import_date_stats_), import_days);
    }
    EXPECT_EQ(stats.color_label_counts_, color_counts);
    EXPECT_EQ(stats.delete_mark_count_, delete_count);
  }

  /// One query: the model match set, then every read of the query against the model and
  /// against the other reads.
  void CheckQuery(sl_element_id_t scope, const std::optional<FilterNode>& filter,
                  const AlbumQueryOptions& options) {
    SCOPED_TRACE(std::format("scope {} sort {} {} group {} zone '{}'", scope,
                             static_cast<int>(options.sort_field_),
                             static_cast<int>(options.sort_direction_),
                             static_cast<int>(options.group_field_), options.local_day_time_zone_));
    const auto                   matches = ExpectedMatches(model_, scope, filter);
    const auto                   full    = Browse().ReadAlbumQuery(scope, filter, options, "",
                                                                   AlbumQueryRead{.offset_ = 0,
                                                                                  .limit_ = kMaxAlbumQueryPageRows,
                                                                                  .read_groups_ = true,
                                                                                  .read_statistics_ = true,
                                                                                  .read_occurrence_file_ids_ = true});
    const auto                   count   = static_cast<int64_t>(matches.size());

    std::vector<sl_element_id_t> stream;
    for (const auto& row : full.rows_) {
      stream.push_back(row.photo_.file_id_);
    }
    std::vector<sl_element_id_t> expected_ids;
    for (const auto* file : matches) {
      expected_ids.push_back(file->file_id);
    }
    auto sorted_stream = stream;
    std::sort(sorted_stream.begin(), sorted_stream.end());
    if (sorted_stream != expected_ids) {
      std::string difference;
      for (const auto& [id, file] : model_.files) {
        const bool read     = std::binary_search(sorted_stream.begin(), sorted_stream.end(), id);
        const bool expected = std::binary_search(expected_ids.begin(), expected_ids.end(), id);
        if (read != expected) {
          difference +=
              std::format("\n  {} {}", read ? "read only:" : "model only:", DescribeFile(file));
        }
      }
      FAIL() << "the read selects other files than the model\n  filter: "
             << (filter.has_value() ? DescribeFilter(*filter) : "(none)") << difference;
    }
    ASSERT_EQ(full.unique_file_count_, count);
    ASSERT_EQ(full.occurrence_count_, count);
    ASSERT_EQ(full.occurrence_file_ids_, stream);
    CheckRows(full.rows_);
    ASSERT_TRUE(full.statistics_.has_value());
    CheckStatistics(*full.statistics_, matches, options);

    // Groups partition the occurrence stream in order. The flat mode reads no group.
    if (options.group_field_ == AlbumGroupField::kNone) {
      EXPECT_TRUE(full.groups_.empty());
    }
    int64_t next_occurrence = options.group_field_ == AlbumGroupField::kNone ? count : 0;
    for (const auto& group : full.groups_) {
      EXPECT_EQ(group.first_occurrence_, next_occurrence) << "group " << KeyText(group.key_);
      for (int64_t i = 0; i < group.photo_count_ && next_occurrence + i < count; ++i) {
        EXPECT_EQ(KeyText(full.rows_[static_cast<size_t>(next_occurrence + i)].group_key_),
                  KeyText(group.key_))
            << "occurrence " << next_occurrence + i;
      }
      next_occurrence += group.photo_count_;
    }
    EXPECT_EQ(next_occurrence, count);

    // Order and groups that the model computes.
    if (const auto groups = ExpectedGroups(matches, options); groups.has_value()) {
      std::vector<sl_element_id_t> expected_stream;
      for (const auto& group : *groups) {
        expected_stream.insert(expected_stream.end(), group.files.begin(), group.files.end());
      }
      EXPECT_EQ(stream, expected_stream) << "photo order";
      if (options.group_field_ != AlbumGroupField::kNone) {
        ASSERT_EQ(full.groups_.size(), groups->size());
        for (size_t i = 0; i < groups->size(); ++i) {
          EXPECT_EQ(KeyText(full.groups_[i].key_), KeyText((*groups)[i].key))
              << KeyText(full.groups_[i].key_) << " at " << i;
          EXPECT_EQ(full.groups_[i].photo_count_, static_cast<int64_t>((*groups)[i].files.size()));
        }
      }
    }

    // Pages put together are the full stream.
    const auto                   page_size = Uniform(1, 17);
    std::vector<sl_element_id_t> paged;
    for (int64_t offset = 0; offset < count; offset += page_size) {
      const auto page = Browse().ReadAlbumQuery(
          scope, filter, options, "", AlbumQueryRead{.offset_ = offset, .limit_ = page_size});
      EXPECT_EQ(page.unique_file_count_, count);
      EXPECT_EQ(page.first_occurrence_, offset);
      for (const auto& row : page.rows_) {
        paged.push_back(row.photo_.file_id_);
      }
    }
    EXPECT_EQ(paged, stream) << "pages of " << page_size;

    // File positions and file id ranges agree with the stream.
    for (int i = 0; i < 3 && count > 0; ++i) {
      const auto index   = Uniform(0, count - 1);
      const auto file_id = stream[static_cast<size_t>(index)];
      const auto position =
          Browse().ReadAlbumFilePosition(scope, filter, options, "", file_id, std::nullopt);
      ASSERT_TRUE(position.has_value()) << "file " << file_id;
      EXPECT_EQ(position->occurrence_index_, index) << "file " << file_id;
      EXPECT_EQ(KeyText(position->group_key_),
                KeyText(full.rows_[static_cast<size_t>(index)].group_key_));

      const auto begin = Uniform(0, count - 1);
      const auto end   = Uniform(begin, count);
      const auto ids   = Browse().ReadAlbumFileIds(scope, filter, options, "", begin, end);
      EXPECT_EQ(ids, std::vector<sl_element_id_t>(stream.begin() + begin, stream.begin() + end))
          << "range " << begin << ".." << end;
    }
    const auto not_matched =
        std::find_if(model_.files.begin(), model_.files.end(), [&](const auto& entry) {
          return !std::binary_search(expected_ids.begin(), expected_ids.end(), entry.first);
        });
    if (not_matched != model_.files.end()) {
      EXPECT_FALSE(
          Browse()
              .ReadAlbumFilePosition(scope, filter, options, "", not_matched->first, std::nullopt)
              .has_value())
          << "file " << not_matched->first << " does not match";
    }

    // A stats bar click inside the match set selects the bucket count of the match set.
    if (count > 0) {
      const auto& stats = *full.statistics_;
      const auto  pick  = Uniform(0, 2);
      const auto& buckets =
          pick == 0 ? stats.camera_stats_ : (pick == 1 ? stats.rating_stats_ : stats.date_stats_);
      const auto& bucket = Pick(buckets);
      const auto  click =
          pick == 0   ? sleeve_filter::BuildCameraModelBucketFilter(conv::FromBytes(bucket.label_))
           : pick == 1 ? sleeve_filter::BuildRatingBucketFilter(conv::FromBytes(bucket.label_))
           : bucket.label_.empty()
               ? sleeve_filter::BuildCaptureDateUnknownFilter()
               : sleeve_filter::BuildCaptureDateBucketFilter(conv::FromBytes(bucket.label_));
      const auto merged  = MergeFilterNodes(filter, click);
      const auto clicked = Browse().ReadAlbumQuery(scope, merged, options, "",
                                                   AlbumQueryRead{.offset_ = 0, .limit_ = 0});
      EXPECT_EQ(clicked.unique_file_count_, bucket.count_) << "bucket '" << bucket.label_ << "'";
    }

    // f AND NOT f is never TRUE.
    if (filter.has_value()) {
      FilterNode negated{
          .type_ = FilterNode::Type::Logical, .op_ = FilterOp::NOT, .children_ = {*filter}};
      FilterNode contradiction{.type_     = FilterNode::Type::Logical,
                               .op_       = FilterOp::AND,
                               .children_ = {*filter, negated}};
      const auto empty = Browse().ReadAlbumQuery(scope, contradiction, options, "",
                                                 AlbumQueryRead{.offset_ = 0, .limit_ = 0});
      EXPECT_EQ(empty.unique_file_count_, 0);
    }
  }

  void CheckRandomQueries(int query_count) {
    for (int i = 0; i < query_count && !HasFatalFailure(); ++i) {
      std::optional<FilterNode> filter;
      if (!Chance(0.15)) {
        filter = RandomFilter(3);
      }
      CheckQuery(RandomScope(), filter, RandomOptions());
    }
  }

  /// Whole-project reads: overview, folder counts, last edited file.
  void CheckProjectReads() {
    const auto overview = Browse().ReadProjectOverview();
    EXPECT_EQ(overview.photo_count_, model_.files.size());
    uint64_t                   edited = 0;
    std::optional<std::string> earliest;
    std::optional<std::string> latest;
    for (const auto& [id, file] : model_.files) {
      edited += file.edited ? 1 : 0;
      if (const auto day = file.CaptureDate(); day.has_value()) {
        earliest = earliest.has_value() ? std::min(*earliest, *day) : *day;
        latest   = latest.has_value() ? std::max(*latest, *day) : *day;
      }
    }
    EXPECT_EQ(overview.edited_photo_count_, edited);
    EXPECT_EQ(overview.earliest_capture_date_, earliest);
    EXPECT_EQ(overview.latest_capture_date_, latest);

    const auto counts = Browse().ReadFolderImageCounts();
    EXPECT_EQ(counts.project_photo_count_, model_.files.size());
    for (const auto& [album_id, album] : model_.albums) {
      const auto it = counts.folder_photo_counts_.find(album_id);
      if (album.files.empty()) {
        EXPECT_EQ(it, counts.folder_photo_counts_.end()) << "empty album " << album_id;
      } else {
        ASSERT_NE(it, counts.folder_photo_counts_.end()) << "album " << album_id;
        EXPECT_EQ(it->second, album.files.size()) << "album " << album_id;
      }
    }

    const auto last = Browse().ReadLastEditedFile();
    if (model_.edit_order.empty()) {
      EXPECT_FALSE(last.has_value());
    } else {
      ASSERT_TRUE(last.has_value());
      EXPECT_EQ(last->file_id_, model_.edit_order.back());
    }
  }

  void CheckAll(int query_count) {
    test::ExpectStorageInvariants(*project_->GetStorage());
    if (HasFatalFailure()) {
      return;
    }
    CheckProjectReads();
    CheckRandomQueries(query_count);
  }

  std::filesystem::path                dir_;
  std::unique_ptr<ProjectService>      project_;
  std::unique_ptr<PipelineMgmtService> pipelines_;
  std::mt19937_64                      rng_;
  LibraryModel                         model_;
  int                                  next_name_  = 0;
  int                                  next_album_ = 0;
};

TEST_P(LibraryReferenceModelTest, RandomOperationsKeepQueriesEqualToTheReferenceModel) {
  SCOPED_TRACE(std::format("random seed {}", GetParam()));
  AddFiles(kInitialFileCount);
  ASSERT_FALSE(HasFatalFailure());
  CheckAll(kQueriesPerStep * 2);

  for (int step = 0; step < kOperationCount && !HasFatalFailure(); ++step) {
    const auto operation = ApplyRandomOperation();
    SCOPED_TRACE(std::format("step {}: {}", step, operation));
    if (HasFatalFailure()) {
      break;
    }
    CheckAll(kQueriesPerStep);
  }
  if (HasFatalFailure()) {
    return;
  }

  // The stored rows alone give the same results.
  OpenProject();
  SCOPED_TRACE("after the project is opened again");
  CheckAll(kQueriesPerStep * 4);
}

INSTANTIATE_TEST_SUITE_P(FixedSeeds, LibraryReferenceModelTest,
                         ::testing::Values(uint64_t{1}, uint64_t{2}, uint64_t{3},
                                           uint64_t{20261110}));

auto ViolatedRules(Storage& storage) -> std::multiset<std::string> {
  std::multiset<std::string> rules;
  for (const auto& violation : test::FindStorageInvariantViolations(storage)) {
    rules.insert(violation.rule);
  }
  return rules;
}

// #805: the check reports the rows that break a rule, one violation per row.
TEST(StorageInvariantCheckTest, ReportsFilesWithoutHistoryRootAndDanglingFolderContent) {
  TimeProvider::Refresh();
  const test::TestTempDirectory temp_dir;
  const auto&                   dir = temp_dir.Path();
  {
    ProjectService      project(dir / "library.db", dir / "library.json");
    PipelineMgmtService pipelines(project.GetStorage());
    auto&               storage = *project.GetStorage();
    EXPECT_TRUE(ViolatedRules(storage).empty()) << "an empty project breaks no rule";

    SyntheticLibraryBuilder builder(project);
    const auto              ids = builder.AddFiles(
        {SyntheticImageSpec{
                          .file_name_ = L"a.jpg", .image_path_ = L"D:/a.jpg", .is_raw_file_ = false},
                      SyntheticImageSpec{
                          .file_name_ = L"b.jpg", .image_path_ = L"D:/b.jpg", .is_raw_file_ = false}});
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ViolatedRules(storage),
              (std::multiset<std::string>{"file has a history root", "file has a history root"}));

    for (const auto id : ids) {
      pipelines.InitializeImageRoot(id, CreateDefaultPipelineDocument(), nullptr);
    }
    test::ExpectStorageInvariants(storage);

    {
      auto guard = StorageTestAccess::GetDatabase(storage).GetConnectionGuard();
      auto lock  = guard.Lock();
      duckorm::execute(guard.conn_,
                       duckorm::expr::raw("INSERT INTO FolderContent VALUES (0, 999999)"));
    }
    EXPECT_EQ(ViolatedRules(storage),
              (std::multiset<std::string>{"folder content row names an existing element"}));
  }
}

}  // namespace
}  // namespace alcedo
