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

auto Compile(const FilterNode& node) -> duckorm::SqlFragment {
  return FilterSQLCompiler::Compile(node);
}

}  // namespace

TEST(SleeveFilterFactoryTest, CameraModelBucketEqualsUsesCoalescedBucketColumnAndBind) {
  const auto frag = Compile(sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850"));
  EXPECT_EQ(frag.sql_,
            "(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "Nikon D850");
}

TEST(SleeveFilterFactoryTest, CameraModelBucketBindsQuoteInsideLabelWithoutEmbedding) {
  const auto frag = Compile(sleeve_filter::BuildCameraModelBucketFilter(L"O'Brien"));
  EXPECT_EQ(frag.sql_,
            "(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "O'Brien");
}

TEST(SleeveFilterFactoryTest, BucketLabelKeepsInjectPayloadOnlyInBind) {
  const auto frag = Compile(sleeve_filter::BuildCameraModelBucketFilter(L"x' OR 1=1 --"));
  EXPECT_EQ(frag.sql_,
            "(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "x' OR 1=1 --");
  EXPECT_EQ(frag.sql_.find("OR 1=1"), std::string::npos);
  EXPECT_EQ(frag.sql_.find("--"), std::string::npos);
}

TEST(SleeveFilterFactoryTest, LensBucketEqualsUsesCoalescedBucketColumnAndBind) {
  const auto frag = Compile(sleeve_filter::BuildLensBucketFilter(L"Synthetic 50mm"));
  EXPECT_EQ(frag.sql_,
            "(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Lens'), ''), '(unknown)') = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "Synthetic 50mm");
}

TEST(SleeveFilterFactoryTest, CaptureDateBucketEqualsUsesDateCastColumnAndBind) {
  const auto frag = Compile(sleeve_filter::BuildCaptureDateBucketFilter(L"2026-05-25"));
  EXPECT_EQ(frag.sql_,
            "(TRY_CAST(json_extract(i.metadata, '$.DateTimeString') AS DATE)::VARCHAR = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "2026-05-25");
}

TEST(SleeveFilterFactoryTest, CaptureDateUnknownMatchesNullOrEmptyStringWithBind) {
  const auto frag = Compile(sleeve_filter::BuildCaptureDateUnknownFilter());
  EXPECT_EQ(frag.sql_,
            "((json_extract_string(i.metadata, '$.DateTimeString') IS NULL) OR "
            "(json_extract_string(i.metadata, '$.DateTimeString') = ?))");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "");
}

TEST(SleeveFilterFactoryTest, RatingBucketNumericLabelBecomesIntEqualityBind) {
  const auto frag = Compile(sleeve_filter::BuildRatingBucketFilter(L"4"));
  EXPECT_EQ(frag.sql_, "(json_extract(i.metadata, '$.Rating')::INT = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(frag.binds_[0]), 4);
}

TEST(SleeveFilterFactoryTest, RatingBucketUnknownLabelBecomesNullCheck) {
  const auto frag = Compile(sleeve_filter::BuildRatingBucketFilter(L"(unknown)"));
  EXPECT_EQ(frag.sql_, "(json_extract(i.metadata, '$.Rating') IS NULL)");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(SleeveFilterFactoryTest, SemanticLabelExistsBindsModelKeyAndAliases) {
  const std::vector<std::string> aliases{"landscape", "\u98CE\u666F"};
  const auto frag =
      Compile(sleeve_filter::BuildSemanticLabelExistsFilter("mobileclip-test", aliases));
  EXPECT_EQ(frag.sql_,
            "EXISTS (SELECT 1 FROM SemanticImageLabel sl WHERE ((sl.file_id = e.id) AND "
            "(sl.model_key = ?) AND ((LOWER(sl.label) = LOWER(?)) OR "
            "(LOWER(sl.label) = LOWER(?)))))");
  ASSERT_EQ(frag.binds_.size(), 3u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "mobileclip-test");
  EXPECT_EQ(std::get<std::string>(frag.binds_[1]), "landscape");
  EXPECT_EQ(std::get<std::string>(frag.binds_[2]), "\u98CE\u666F");
}

TEST(SleeveFilterFactoryTest, SemanticLabelExistsWithEmptyModelKeyYieldsFalse) {
  const std::vector<std::string> aliases{"landscape"};
  const auto frag = Compile(sleeve_filter::BuildSemanticLabelExistsFilter("", aliases));
  EXPECT_EQ(frag.sql_, "1 = 0");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(SleeveFilterFactoryTest, MergeFilterNodesCombinesTwoTreesUnderAndRootWithBinds) {
  const auto left  = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");
  const auto right = sleeve_filter::BuildCaptureDateBucketFilter(L"2026-05-25");

  const auto merged = MergeFilterNodes(left, right);
  ASSERT_TRUE(merged.has_value());
  const auto frag = Compile(*merged);
  EXPECT_EQ(frag.sql_,
            "((COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = "
            "?) AND (TRY_CAST(json_extract(i.metadata, '$.DateTimeString') AS DATE)::"
            "VARCHAR = ?))");
  ASSERT_EQ(frag.binds_.size(), 2u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "Nikon D850");
  EXPECT_EQ(std::get<std::string>(frag.binds_[1]), "2026-05-25");
}

TEST(SleeveFilterFactoryTest, MergeFilterNodesReturnsSinglePresentSide) {
  const auto left = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");

  const auto with_left_only = MergeFilterNodes(left, std::nullopt);
  ASSERT_TRUE(with_left_only.has_value());
  EXPECT_EQ(with_left_only->type_, FilterNode::Type::Condition);

  const auto with_right_only = MergeFilterNodes(std::nullopt, left);
  ASSERT_TRUE(with_right_only.has_value());
  EXPECT_EQ(with_right_only->type_, FilterNode::Type::Condition);

  const auto both_absent = MergeFilterNodes(std::nullopt, std::nullopt);
  EXPECT_FALSE(both_absent.has_value());
}

TEST(SleeveFilterFactoryTest, CompileFilterPredicateKeepsBindsForBucketNode) {
  const auto node = sleeve_filter::BuildCameraModelBucketFilter(L"O'Brien");
  const auto where = CompileFilterPredicate(node);
  ASSERT_TRUE(where.has_value());
  EXPECT_EQ(where->sql_,
            "(COALESCE(NULLIF(json_extract_string(i.metadata, '$.Model'), ''), '(unknown)') = ?)");
  ASSERT_EQ(where->binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(where->binds_[0]), "O'Brien");
}

TEST(SleeveFilterFactoryTest, CompileFilterPredicateReturnsNulloptForEmptyTree) {
  const FilterNode empty_tree{
      FilterNode::Type::Logical, FilterOp::AND, {}, std::nullopt, std::nullopt};
  EXPECT_FALSE(CompileFilterPredicate(empty_tree).has_value());
  EXPECT_FALSE(CompileFilterPredicate(std::nullopt).has_value());
}
