//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <string>

#include "sleeve/sleeve_filter/filter_combo.hpp"

using namespace alcedo;

TEST(SleeveFilterCompileTest, EqualsCameraModelUsesImageAliasAndBoundParam) {
  FieldCondition cond{
      .field_ = FilterField::ExifCameraModel,
      .op_    = CompareOp::EQUALS,
      .value_ = std::wstring(L"Canon EOS 5D Mark IV"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "(json_extract(i.metadata, '$.Model') = ?)");
  ASSERT_EQ(sql.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(sql.binds_[0]), "Canon EOS 5D Mark IV");
}

TEST(SleeveFilterCompileTest, NestedAndOrUsesExprFragmentsWithBinds) {
  FieldCondition cond1{
      .field_ = FilterField::ExifCameraModel,
      .op_    = CompareOp::EQUALS,
      .value_ = std::wstring(L"Nikon D850"),
  };
  FilterNode node1{FilterNode::Type::Condition, {}, {}, std::move(cond1), std::nullopt};

  FieldCondition cond2{
      .field_ = FilterField::FileExtension,
      .op_    = CompareOp::ENDS_WITH,
      .value_ = std::wstring(L".NEF"),
  };
  FilterNode node2{FilterNode::Type::Condition, {}, {}, std::move(cond2), std::nullopt};

  FilterNode root{FilterNode::Type::Logical, FilterOp::AND, {node1, node2}, {}, std::nullopt};

  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_,
            "((json_extract(i.metadata, '$.Model') = ?) AND "
            "(UPPER(i.file_name) LIKE ?))");
  ASSERT_EQ(sql.binds_.size(), 2u);
  EXPECT_EQ(std::get<std::string>(sql.binds_[0]), "Nikon D850");
  EXPECT_EQ(std::get<std::string>(sql.binds_[1]), "%.NEF");
}

TEST(SleeveFilterCompileTest, BindsQuoteInsideStringFilterValueWithoutEmbedding) {
  FieldCondition cond{
      .field_ = FilterField::ExifCameraModel,
      .op_    = CompareOp::CONTAINS,
      .value_ = std::wstring(L"O'Brien"),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "(json_extract(i.metadata, '$.Model') LIKE ?)");
  ASSERT_EQ(sql.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(sql.binds_[0]), "%O'Brien%");
}

TEST(SleeveFilterCompileTest, RawSQLBridgeNodeKeepsTrustedText) {
  FilterNode root{FilterNode::Type::RawSQL, {}, {}, std::nullopt,
                  std::wstring(L"(e.id > 0 AND i.image_path IS NOT NULL)")};
  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "(e.id > 0 AND i.image_path IS NOT NULL)");
  EXPECT_TRUE(sql.binds_.empty());
}

TEST(SleeveFilterCompileTest, BetweenIsoUsesImageMetadataAliasAndBinds) {
  FieldCondition cond{
      .field_        = FilterField::ExifISO,
      .op_           = CompareOp::BETWEEN,
      .value_        = int64_t(100),
      .second_value_ = int64_t(800),
  };
  FilterNode root{FilterNode::Type::Condition, {}, {}, std::move(cond), std::nullopt};

  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "(json_extract(i.metadata, '$.ISO')::INT BETWEEN ? AND ?)");
  ASSERT_EQ(sql.binds_.size(), 2u);
  EXPECT_EQ(std::get<int64_t>(sql.binds_[0]), 100);
  EXPECT_EQ(std::get<int64_t>(sql.binds_[1]), 800);
}

TEST(SleeveFilterCompileTest, ImportDateAndFileNameUseElementAliasWithBinds) {
  FieldCondition name_cond{
      .field_ = FilterField::FileName,
      .op_    = CompareOp::EQUALS,
      .value_ = std::wstring(L"shot.nef"),
  };
  FilterNode name_node{FilterNode::Type::Condition, {}, {}, std::move(name_cond), std::nullopt};

  FieldCondition import_cond{
      .field_ = FilterField::ImportDate,
      .op_    = CompareOp::GREATER_THAN,
      .value_ = std::wstring(L"2025-01-01"),
  };
  FilterNode import_node{FilterNode::Type::Condition, {}, {}, std::move(import_cond),
                         std::nullopt};

  FilterNode root{FilterNode::Type::Logical, FilterOp::AND, {name_node, import_node}, {},
                  std::nullopt};
  const auto sql = FilterSQLCompiler::Compile(root);
  EXPECT_EQ(sql.sql_, "((e.element_name = ?) AND (e.added_time > ?))");
  ASSERT_EQ(sql.binds_.size(), 2u);
  EXPECT_EQ(std::get<std::string>(sql.binds_[0]), "shot.nef");
  EXPECT_EQ(std::get<std::string>(sql.binds_[1]), "2025-01-01");
}
