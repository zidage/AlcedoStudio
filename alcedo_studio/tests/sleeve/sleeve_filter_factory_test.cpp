//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <variant>

#include "image/color_label.hpp"
#include "sleeve/sleeve_filter/filter_combo.hpp"
#include "sleeve/sleeve_filter/filter_factory.hpp"
#include "storage/query/filter_compiler.hpp"

using namespace alcedo;

namespace {

auto Compile(const FilterNode& node) -> duckorm::SqlFragment {
  return FilterSQLCompiler::Compile(node);
}

}  // namespace

TEST(SleeveFilterFactoryTest, CameraModelBucketEqualsUsesCoalescedBucketColumnAndBind) {
  const auto frag = Compile(sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850"));
  EXPECT_EQ(frag.sql_, "(COALESCE(NULLIF(i.camera_model, ''), '(unknown)') = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "Nikon D850");
}

TEST(SleeveFilterFactoryTest, CameraModelBucketBindsQuoteInsideLabelWithoutEmbedding) {
  const auto frag = Compile(sleeve_filter::BuildCameraModelBucketFilter(L"O'Brien"));
  EXPECT_EQ(frag.sql_, "(COALESCE(NULLIF(i.camera_model, ''), '(unknown)') = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "O'Brien");
}

TEST(SleeveFilterFactoryTest, BucketLabelKeepsInjectPayloadOnlyInBind) {
  const auto frag = Compile(sleeve_filter::BuildCameraModelBucketFilter(L"x' OR 1=1 --"));
  EXPECT_EQ(frag.sql_, "(COALESCE(NULLIF(i.camera_model, ''), '(unknown)') = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "x' OR 1=1 --");
  EXPECT_EQ(frag.sql_.find("OR 1=1"), std::string::npos);
  EXPECT_EQ(frag.sql_.find("--"), std::string::npos);
}

TEST(SleeveFilterFactoryTest, LensBucketEqualsUsesCoalescedBucketColumnAndBind) {
  const auto frag = Compile(sleeve_filter::BuildLensBucketFilter(L"Synthetic 50mm"));
  EXPECT_EQ(frag.sql_, "(COALESCE(NULLIF(i.lens, ''), '(unknown)') = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "Synthetic 50mm");
}

TEST(SleeveFilterFactoryTest, CaptureDateBucketEqualsUsesDateCastColumnAndBind) {
  const auto frag = Compile(sleeve_filter::BuildCaptureDateBucketFilter(L"2026-05-25"));
  EXPECT_EQ(frag.sql_, "(CAST(i.capture_date AS VARCHAR) = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "2026-05-25");
}

TEST(SleeveFilterFactoryTest, CaptureDateUnknownMatchesNullCaptureDateColumn) {
  const auto frag = Compile(sleeve_filter::BuildCaptureDateUnknownFilter());
  EXPECT_EQ(frag.sql_, "(i.capture_date IS NULL)");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(SleeveFilterFactoryTest, RatingBucketNumericLabelBecomesIntEqualityBind) {
  const auto frag = Compile(sleeve_filter::BuildRatingBucketFilter(L"4"));
  EXPECT_EQ(frag.sql_, "(i.rating = ?)");
  ASSERT_EQ(frag.binds_.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(frag.binds_[0]), 4);
}

TEST(SleeveFilterFactoryTest, RatingBucketUnknownLabelBecomesNullCheck) {
  const auto frag = Compile(sleeve_filter::BuildRatingBucketFilter(L"(unknown)"));
  EXPECT_EQ(frag.sql_, "(i.rating IS NULL)");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(SleeveFilterFactoryTest, SemanticLabelBucketBindsModelKeyAndComparesTheLabelKey) {
  const auto frag =
      Compile(sleeve_filter::BuildSemanticLabelBucketFilter("mobileclip-test", "landscape"));
  EXPECT_EQ(frag.sql_,
            "EXISTS (SELECT 1 FROM SemanticImageLabel sl WHERE ((sl.file_id = e.id) AND "
            "((sl.model_key = ?) AND TRIM(sl.label) <> '') AND "
            "(LOWER(TRIM(sl.label)) = LOWER(TRIM(?)))))");
  ASSERT_EQ(frag.binds_.size(), 2u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "mobileclip-test");
  EXPECT_EQ(std::get<std::string>(frag.binds_[1]), "landscape");
}

TEST(SleeveFilterFactoryTest, SemanticLabelBucketWithEmptyModelKeyYieldsFalse) {
  const auto frag = Compile(sleeve_filter::BuildSemanticLabelBucketFilter("", "landscape"));
  EXPECT_EQ(frag.sql_, "1 = 0");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(SleeveFilterFactoryTest, BucketBuildersReturnStatsBucketDomainValues) {
  const auto camera = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");
  ASSERT_EQ(camera.type_, FilterNode::Type::StatsBucket);
  ASSERT_TRUE(camera.stats_bucket_.has_value());
  EXPECT_EQ(camera.stats_bucket_->dimension, StatsDimensionId::kCameraModel);
  EXPECT_EQ(std::get<std::string>(camera.stats_bucket_->key), "Nikon D850");
  EXPECT_FALSE(camera.library_search_.has_value());

  const auto rating = sleeve_filter::BuildRatingBucketFilter(L"3");
  EXPECT_EQ(std::get<int64_t>(rating.stats_bucket_->key), 3);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(
      sleeve_filter::BuildRatingBucketFilter(L"(unknown)").stats_bucket_->key));

  const auto import_day = sleeve_filter::BuildImportDateBucketFilter(L"2026-06-01", "UTC");
  EXPECT_EQ(import_day.stats_bucket_->dimension, StatsDimensionId::kImportDay);
  EXPECT_EQ(std::get<std::string>(import_day.stats_bucket_->key), "2026-06-01");
  EXPECT_EQ(import_day.stats_bucket_->time_zone, "UTC");

  const auto label = sleeve_filter::BuildColorLabelFilter(ColorLabel::kBlue);
  EXPECT_EQ(std::get<ColorLabel>(label.stats_bucket_->key), ColorLabel::kBlue);
}

TEST(SleeveFilterFactoryTest, ColorLabelBucketTestsTheCodeBit) {
  const auto frag = Compile(sleeve_filter::BuildColorLabelFilter(ColorLabel::kGreen));
  EXPECT_EQ(frag.sql_, "(((i.color_labels >> 3) & 1) = 1)");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(SleeveFilterFactoryTest, DeleteMarkBucketReadsTheMarkColumn) {
  const auto frag = Compile(sleeve_filter::BuildDeleteMarkFilter());
  EXPECT_EQ(frag.sql_, "i.delete_mark");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(SleeveFilterFactoryTest, ImportDayBucketIsAHalfOpenRangeOnTheImportTime) {
  const auto frag =
      Compile(sleeve_filter::BuildImportDateBucketFilter(L"2026-03-08", "America/New_York"));
  EXPECT_EQ(frag.sql_,
            "((e.added_time >= timezone('UTC', timezone(CAST(? AS VARCHAR), CAST(? AS "
            "TIMESTAMP)))) AND (e.added_time < timezone('UTC', timezone(CAST(? AS VARCHAR), "
            "CAST(? AS TIMESTAMP)))))");
  ASSERT_EQ(frag.binds_.size(), 4u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "America/New_York");
  EXPECT_EQ(std::get<std::string>(frag.binds_[1]), "2026-03-08 00:00:00");
  EXPECT_EQ(std::get<std::string>(frag.binds_[3]), "2026-03-09 00:00:00");
}

TEST(SleeveFilterFactoryTest, ImportDayUnknownMatchesNullImportTime) {
  const auto frag = Compile(sleeve_filter::BuildImportDateUnknownFilter());
  EXPECT_EQ(frag.sql_, "(e.added_time IS NULL)");
}

TEST(SleeveFilterFactoryTest, StatsBucketWithUnknownDimensionFailsToCompile) {
  FilterNode node{FilterNode::Type::StatsBucket, FilterOp::AND, {}, std::nullopt, std::nullopt};
  node.stats_bucket_ = StatsBucketCondition{.dimension = static_cast<StatsDimensionId>(200),
                                            .key       = std::string("x")};
  EXPECT_THROW((void)Compile(node), std::invalid_argument);
}

TEST(SleeveFilterFactoryTest, StatsBucketWithKeyOfAnotherDimensionFailsToCompile) {
  FilterNode node{FilterNode::Type::StatsBucket, FilterOp::AND, {}, std::nullopt, std::nullopt};
  node.stats_bucket_ =
      StatsBucketCondition{.dimension = StatsDimensionId::kRating, .key = std::string("4")};
  EXPECT_THROW((void)Compile(node), std::invalid_argument);
}

TEST(SleeveFilterFactoryTest, MergeFilterNodesCombinesTwoTreesUnderAndRootWithBinds) {
  const auto left  = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");
  const auto right = sleeve_filter::BuildCaptureDateBucketFilter(L"2026-05-25");

  const auto merged = MergeFilterNodes(left, right);
  ASSERT_TRUE(merged.has_value());
  const auto frag = Compile(*merged);
  EXPECT_EQ(frag.sql_,
            "((COALESCE(NULLIF(i.camera_model, ''), '(unknown)') = ?) AND "
            "(CAST(i.capture_date AS VARCHAR) = ?))");
  ASSERT_EQ(frag.binds_.size(), 2u);
  EXPECT_EQ(std::get<std::string>(frag.binds_[0]), "Nikon D850");
  EXPECT_EQ(std::get<std::string>(frag.binds_[1]), "2026-05-25");
}

TEST(SleeveFilterFactoryTest, MergeFilterNodesReturnsSinglePresentSide) {
  const auto left = sleeve_filter::BuildCameraModelBucketFilter(L"Nikon D850");

  const auto with_left_only = MergeFilterNodes(left, std::nullopt);
  ASSERT_TRUE(with_left_only.has_value());
  EXPECT_EQ(with_left_only->type_, FilterNode::Type::StatsBucket);

  const auto with_right_only = MergeFilterNodes(std::nullopt, left);
  ASSERT_TRUE(with_right_only.has_value());
  EXPECT_EQ(with_right_only->type_, FilterNode::Type::StatsBucket);

  const auto both_absent = MergeFilterNodes(std::nullopt, std::nullopt);
  EXPECT_FALSE(both_absent.has_value());
}

TEST(SleeveFilterFactoryTest, CompileFilterPredicateKeepsBindsForBucketNode) {
  const auto node = sleeve_filter::BuildCameraModelBucketFilter(L"O'Brien");
  const auto where = CompileFilterPredicate(node);
  ASSERT_TRUE(where.has_value());
  EXPECT_EQ(where->sql_, "(COALESCE(NULLIF(i.camera_model, ''), '(unknown)') = ?)");
  ASSERT_EQ(where->binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(where->binds_[0]), "O'Brien");
}

TEST(SleeveFilterFactoryTest, CompileFilterPredicateReturnsNulloptForEmptyTree) {
  const FilterNode empty_tree{
      FilterNode::Type::Logical, FilterOp::AND, {}, std::nullopt, std::nullopt};
  EXPECT_FALSE(CompileFilterPredicate(empty_tree).has_value());
  EXPECT_FALSE(CompileFilterPredicate(std::nullopt).has_value());
}
