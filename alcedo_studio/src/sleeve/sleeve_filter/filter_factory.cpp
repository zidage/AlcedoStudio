//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_filter/filter_factory.hpp"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "utils/string/convert.hpp"

namespace alcedo::sleeve_filter {
namespace {

auto MakeStatsBucketNode(StatsBucketCondition condition) -> FilterNode {
  FilterNode node{FilterNode::Type::StatsBucket, FilterOp::AND, {}, std::nullopt, std::nullopt};
  node.stats_bucket_ = std::move(condition);
  return node;
}

auto MakeStatsBucketNode(StatsDimensionId dimension, StatsBucketKey key) -> FilterNode {
  return MakeStatsBucketNode(StatsBucketCondition{.dimension = dimension, .key = std::move(key)});
}

}  // namespace

auto BuildCameraModelBucketFilter(const std::wstring& label) -> FilterNode {
  return MakeStatsBucketNode(StatsDimensionId::kCameraModel, conv::ToBytes(label));
}

auto BuildLensBucketFilter(const std::wstring& label) -> FilterNode {
  return MakeStatsBucketNode(StatsDimensionId::kLens, conv::ToBytes(label));
}

auto BuildCaptureDateBucketFilter(const std::wstring& date_yyyy_mm_dd) -> FilterNode {
  return MakeStatsBucketNode(StatsDimensionId::kCaptureDate, conv::ToBytes(date_yyyy_mm_dd));
}

auto BuildCaptureDateUnknownFilter() -> FilterNode {
  return MakeStatsBucketNode(StatsDimensionId::kCaptureDate, std::monostate{});
}

auto BuildImportDateBucketFilter(const std::wstring& date_yyyy_mm_dd, const std::string& time_zone)
    -> FilterNode {
  if (time_zone.empty()) {
    throw std::invalid_argument("Import-day filter: the time zone is empty");
  }
  std::string day = conv::ToBytes(date_yyyy_mm_dd);
  (void)ParseStatsBucketDay(day);
  return MakeStatsBucketNode(StatsBucketCondition{
      .dimension = StatsDimensionId::kImportDay, .key = std::move(day), .time_zone = time_zone});
}

auto BuildImportDateUnknownFilter() -> FilterNode {
  return MakeStatsBucketNode(StatsDimensionId::kImportDay, std::monostate{});
}

auto BuildRatingBucketFilter(const std::wstring& label) -> FilterNode {
  const std::string narrow = conv::ToBytes(label);
  size_t            pos    = 0;
  int               value  = 0;
  try {
    value = std::stoi(narrow, &pos);
  } catch (...) {
    pos = 0;
  }
  if (pos == narrow.size() && pos > 0) {
    return MakeStatsBucketNode(StatsDimensionId::kRating, int64_t{value});
  }
  // Non-numeric bucket (for example "(unknown)"): the photos without a rating.
  return MakeStatsBucketNode(StatsDimensionId::kRating, std::monostate{});
}

auto BuildColorLabelFilter(ColorLabel code) -> FilterNode {
  return MakeStatsBucketNode(StatsDimensionId::kColorLabel, code);
}

auto BuildDeleteMarkFilter() -> FilterNode {
  return MakeStatsBucketNode(StatsDimensionId::kDeleteMark, std::monostate{});
}

auto BuildSemanticLabelBucketFilter(const std::string& model_key, const std::string& label_key)
    -> FilterNode {
  return MakeStatsBucketNode(StatsBucketCondition{.dimension = StatsDimensionId::kSemanticLabel,
                                                  .key       = label_key,
                                                  .semantic_model_key = model_key});
}

}  // namespace alcedo::sleeve_filter
