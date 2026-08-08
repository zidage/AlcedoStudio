//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"

using namespace alcedo;

namespace {

auto CompileSql(const FilterNode& node) -> std::string {
  return FilterSQLCompiler::Compile(node).sql_;
}

}  // namespace

TEST(SleeveFilterFactoryTest, CameraModelBucketEqualsUsesCoalescedBucketColumn) {
  const auto node = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");
  EXPECT_EQ(CompileSql(node),
            "(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = "
            "'Nikon D850')");
}

TEST(SleeveFilterFactoryTest, CameraModelBucketEscapesQuoteInsideLabel) {
  const auto node = sleeve_filter::BuildCameraModelBucketFilter(L"O'Brien");
  EXPECT_EQ(CompileSql(node),
            "(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = "
            "'O''Brien')");
}

TEST(SleeveFilterFactoryTest, LensBucketEqualsUsesCoalescedBucketColumn) {
  const auto node = sleeve_filter::BuildLensBucketFilter(L"Synthetic 50mm");
  EXPECT_EQ(CompileSql(node),
            "(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Lens'), ''), '(unknown)') = "
            "'Synthetic 50mm')");
}

TEST(SleeveFilterFactoryTest, CaptureDateBucketEqualsUsesDateCastColumn) {
  const auto node = sleeve_filter::BuildCaptureDateBucketFilter(L"2026-05-25");
  EXPECT_EQ(CompileSql(node),
            "(TRY_CAST(json_extract(i.metadata, '$.DateTimeString') AS DATE)::VARCHAR = "
            "'2026-05-25')");
}

TEST(SleeveFilterFactoryTest, CaptureDateUnknownMatchesNullOrEmptyString) {
  const auto node = sleeve_filter::BuildCaptureDateUnknownFilter();
  EXPECT_EQ(CompileSql(node),
            "((json_extract_string(i.metadata, '$.DateTimeString') IS NULL) OR "
            "(json_extract_string(i.metadata, '$.DateTimeString') = ''))");
}

TEST(SleeveFilterFactoryTest, RatingBucketNumericLabelBecomesIntEquality) {
  const auto node = sleeve_filter::BuildRatingBucketFilter(L"4");
  EXPECT_EQ(CompileSql(node), "(json_extract(i.metadata, '$.Rating')::INT = 4)");
}

TEST(SleeveFilterFactoryTest, RatingBucketUnknownLabelBecomesNullCheck) {
  const auto node = sleeve_filter::BuildRatingBucketFilter(L"(unknown)");
  EXPECT_EQ(CompileSql(node), "(json_extract(i.metadata, '$.Rating') IS NULL)");
}

TEST(SleeveFilterFactoryTest, SemanticLabelExistsEscapesModelKeyAndAliases) {
  const std::vector<std::string> aliases{"landscape", "\u98CE\u666F"};
  const auto node = sleeve_filter::BuildSemanticLabelExistsFilter("mobileclip-test", aliases);
  EXPECT_EQ(CompileSql(node),
            "EXISTS (SELECT 1 FROM SemanticImageLabel sl WHERE ((sl.file_id = e.id) AND "
            "(sl.model_key = 'mobileclip-test') AND ((LOWER(sl.label) = LOWER('landscape')) OR "
            "(LOWER(sl.label) = LOWER('\u98CE\u666F')))))");
}

TEST(SleeveFilterFactoryTest, SemanticLabelExistsWithEmptyModelKeyYieldsFalse) {
  const std::vector<std::string> aliases{"landscape"};
  const auto                     node = sleeve_filter::BuildSemanticLabelExistsFilter("", aliases);
  EXPECT_EQ(CompileSql(node), "1 = 0");
}

TEST(SleeveFilterFactoryTest, MergeFilterNodesCombinesTwoTreesUnderAndRoot) {
  const auto left   = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");
  const auto right  = sleeve_filter::BuildCaptureDateBucketFilter(L"2026-05-25");

  const auto merged = MergeFilterNodes(left, right);
  ASSERT_TRUE(merged.has_value());
  EXPECT_EQ(CompileSql(*merged),
            "((COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = "
            "'Nikon D850') AND (TRY_CAST(json_extract(i.metadata, '$.DateTimeString') AS DATE)::"
            "VARCHAR = '2026-05-25'))");
}

TEST(SleeveFilterFactoryTest, MergeFilterNodesReturnsSinglePresentSide) {
  const auto left           = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");

  const auto with_left_only = MergeFilterNodes(left, std::nullopt);
  ASSERT_TRUE(with_left_only.has_value());
  EXPECT_EQ(with_left_only->type_, FilterNode::Type::Condition);

  const auto with_right_only = MergeFilterNodes(std::nullopt, left);
  ASSERT_TRUE(with_right_only.has_value());
  EXPECT_EQ(with_right_only->type_, FilterNode::Type::Condition);

  const auto both_absent = MergeFilterNodes(std::nullopt, std::nullopt);
  EXPECT_FALSE(both_absent.has_value());
}

TEST(SleeveFilterFactoryTest, CompileFilterWhereRoundTripsNodeToUtf8Text) {
  const auto node  = sleeve_filter::BuildCameraModelBucketFilter(L"O'Brien");
  const auto where = CompileFilterWhere(node);
  ASSERT_TRUE(where.has_value());
  EXPECT_EQ(*where,
            L"(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = "
            L"'O''Brien')");
}

TEST(SleeveFilterFactoryTest, CompileFilterWhereReturnsNulloptForEmptyTree) {
  const FilterNode empty_tree{
      FilterNode::Type::Logical, FilterOp::AND, {}, std::nullopt, std::nullopt};
  EXPECT_FALSE(CompileFilterWhere(empty_tree).has_value());
  EXPECT_FALSE(CompileFilterWhere(std::nullopt).has_value());
}
