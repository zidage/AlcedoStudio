//  Copyright 2025-2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "image/color_label.hpp"
#include "sleeve/sleeve_filter/library_search_request.hpp"
#include "type/type.hpp"

namespace alcedo {
enum class FilterOp { AND, OR, NOT };

/**
 * @brief Filterable fields in Image metadata
 *
 */
enum class FilterField {
  ExifCameraModel,
  ExifFocalLength,
  ExifAperture,
  ExifISO,
  CaptureDate,
  ImportDate,
  FileName,
  FileExtension,
  ImageSize,
  Rating,
  ImagePath,
  SemanticTags,
  FileId,  ///< The file element id; EQUALS with an int64_t value selects one file.
};

enum class CompareOp {
  EQUALS,
  NOT_EQUALS,
  CONTAINS,
  NOT_CONTAINS,
  GREATER_THAN,
  LESS_THAN,
  GREATER_EQUAL,
  LESS_EQUAL,
  STARTS_WITH,
  ENDS_WITH,
  BETWEEN,
  REGEX
};

using FilterValue = std::variant<std::monostate, int64_t, double, bool, std::wstring, std::tm>;

struct FieldCondition {
  FilterField                field_;
  CompareOp                  op_;
  FilterValue                value_;
  std::optional<FilterValue> second_value_ = std::nullopt;  // Used for BETWEEN condition
};

/**
 * @brief Dimensions of the library statistics. Storage defines the bucket expression and the
 *        bucket predicate of each one (storage/query/stats_dimension.hpp).
 */
enum class StatsDimensionId : std::uint8_t {
  kCaptureDate,
  kCameraModel,
  kLens,
  kRating,
  kImportDay,
  kColorLabel,
  kDeleteMark,
  kSemanticLabel,
};

/**
 * @brief Key of one bucket of a stats dimension.
 *
 * - std::monostate: the bucket of the photos without a value (no capture date, no import time,
 *   no rating, no camera model or lens text). For kDeleteMark it is the one bucket, the photos
 *   with the Delete mark.
 * - std::string: the bucket label as the stats read returns it: a `YYYY-MM-DD` day (capture
 *   date, import day), a camera model or lens label (the unknown label included), or a semantic
 *   label key.
 * - int64_t: a rating.
 * - ColorLabel: a color label code.
 */
using StatsBucketKey = std::variant<std::monostate, std::string, int64_t, ColorLabel>;

/// The photos of one stats bucket: a stats bar click.
struct StatsBucketCondition {
  StatsDimensionId dimension;
  StatsBucketKey   key;
  /// kImportDay: IANA zone of the local day, the zone of the import-day buckets.
  std::string      time_zone{};
  /// kSemanticLabel: the active semantic model; an empty key selects no photo.
  std::string      semantic_model_key{};
};

struct FilterNode {
  enum class Type { Logical, Condition, StatsBucket, LibrarySearch } type_;

  // For Logical nodes
  FilterOp                op_;
  std::vector<FilterNode> children_;

  // For Condition nodes
  std::optional<FieldCondition> condition_;

  // For StatsBucket nodes
  std::optional<StatsBucketCondition>   stats_bucket_{};

  // For LibrarySearch nodes
  std::optional<LibrarySearchCondition> library_search_{};
};

/**
 * @brief Read a stats bucket day label.
 *
 * @param text Day in `YYYY-MM-DD` form.
 * @return The calendar day.
 * @throws std::invalid_argument when @p text is not a valid `YYYY-MM-DD` day.
 */
[[nodiscard]] auto ParseStatsBucketDay(std::string_view text) -> std::chrono::year_month_day;

/**
 * @brief Merge two optional filter trees under one AND root.
 *
 * @param left Left filter (for example the stats-bar filter).
 * @param right Right filter (for example the active search filter).
 * @return The merged tree, or the single present side, or std::nullopt when
 * both sides are absent.
 */
[[nodiscard]] auto MergeFilterNodes(const std::optional<FilterNode>& left,
                                    const std::optional<FilterNode>& right)
    -> std::optional<FilterNode>;

class FilterCombo {
 public:
  filter_id_t filter_id_;

 private:
  FilterNode root_;

 public:
  FilterCombo() = default;
  FilterCombo(const filter_id_t id, const FilterNode& root) : filter_id_(id), root_(root) {}

  const FilterNode& GetRoot() const { return root_; }

  void SetRoot(const FilterNode& root) { root_ = root; }
};
};  // namespace alcedo
