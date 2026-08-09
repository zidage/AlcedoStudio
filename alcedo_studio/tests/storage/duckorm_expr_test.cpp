//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"

using duckorm::SqlFragment;
namespace expr = duckorm::expr;

TEST(DuckormExprTest, EscapeStringDoublesSingleQuotes) {
  EXPECT_EQ(expr::escape_string("plain"), "plain");
  EXPECT_EQ(expr::escape_string("O'Brien"), "O''Brien");
  EXPECT_EQ(expr::escape_string("a''b"), "a''''b");
  EXPECT_EQ(expr::escape_string(""), "");
}

TEST(DuckormExprTest, LitStringWrapsAndEscapesQuotes) {
  const auto frag = expr::lit("O'Brien");
  EXPECT_EQ(frag.sql_, "'O''Brien'");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(DuckormExprTest, NestedAndOrBuildsParenthesizedSqlWithoutStringGlueInCaller) {
  const auto camera = expr::eq(expr::col("json_extract(i.metadata, '$.Model')"), expr::lit("D850"));
  const auto iso    = expr::between(expr::col("json_extract(i.metadata, '$.ISO')::INT"),
                                    expr::lit(int64_t{100}), expr::lit(int64_t{800}));
  const auto rating = expr::eq(expr::col("json_extract(i.metadata, '$.Rating')"), expr::lit(int64_t{5}));

  const auto nested = expr::and_({expr::or_({camera, rating}), iso});

  EXPECT_EQ(nested.sql_,
            "(((json_extract(i.metadata, '$.Model') = 'D850') OR "
            "(json_extract(i.metadata, '$.Rating') = 5)) AND "
            "(json_extract(i.metadata, '$.ISO')::INT BETWEEN 100 AND 800))");
  EXPECT_TRUE(nested.binds_.empty());
}

TEST(DuckormExprTest, LikeAndExistsComposeFragments) {
  const auto like_frag =
      expr::like(expr::col("UPPER(i.file_name)"), expr::lit("%.NEF"));
  EXPECT_EQ(like_frag.sql_, "(UPPER(i.file_name) LIKE '%.NEF')");

  const auto exists_frag =
      expr::exists(expr::raw("SELECT 1 FROM SemanticLabel sl WHERE sl.image_id = i.id"));
  EXPECT_EQ(exists_frag.sql_,
            "EXISTS (SELECT 1 FROM SemanticLabel sl WHERE sl.image_id = i.id)");
}

TEST(DuckormExprTest, LikeEscapeAddsEscapeClauseAndKeepsWildcardsLiteral) {
  const auto pattern = expr::lit("%" + expr::escape_like_pattern("100%_dune~") + "%");
  EXPECT_EQ(pattern.sql_, "'%100~%~_dune~~%'");

  const auto frag =
      expr::like_escape(expr::col("LOWER(COALESCE(e.element_name, ''))"), pattern);
  EXPECT_EQ(frag.sql_,
            "(LOWER(COALESCE(e.element_name, '')) LIKE '%100~%~_dune~~%' ESCAPE '~')");
  EXPECT_TRUE(frag.binds_.empty());
}

TEST(DuckormExprTest, EscapeLikePatternUsesCustomEscapeCharacter) {
  EXPECT_EQ(expr::escape_like_pattern("a%b_c", '!'), "a!%b!_c");
  EXPECT_EQ(expr::escape_like_pattern("plain"), "plain");
  EXPECT_EQ(expr::escape_like_pattern(""), "");
}

TEST(DuckormExprTest, ParamKeepsBindValuesInOrder) {
  const auto frag = expr::and_({expr::eq(expr::col("id"), expr::param(int64_t{7})),
                                expr::eq(expr::col("name"), expr::param("x'y"))});
  EXPECT_EQ(frag.sql_, "((id = ?) AND (name = ?))");
  ASSERT_EQ(frag.binds_.size(), 2u);
  EXPECT_EQ(std::get<int64_t>(frag.binds_[0]), 7);
  EXPECT_EQ(std::get<std::string>(frag.binds_[1]), "x'y");
}

TEST(DuckormExprTest, NotAndIsNullComposeUnaryPredicates) {
  EXPECT_EQ(expr::not_(expr::eq(expr::col("a"), expr::lit(int64_t{1}))).sql_, "(NOT (a = 1))");
  EXPECT_EQ(expr::is_null(expr::col("b")).sql_, "(b IS NULL)");
  EXPECT_EQ(expr::is_not_null(expr::col("c")).sql_, "(c IS NOT NULL)");
}

TEST(DuckormExprTest, EmptyAndOrYieldEmptyFragment) {
  EXPECT_TRUE(expr::and_(std::span<const SqlFragment>{}).empty());
  EXPECT_TRUE(expr::or_(std::span<const SqlFragment>{}).empty());
}

TEST(DuckormExprTest, SqlFragmentAppendKeepsBindOrderAcrossRawAndParam) {
  SqlFragment fragment;
  fragment.append(expr::raw("(name = "));
  fragment.append(expr::param(std::string("x' OR 1=1 --")));
  fragment.append(expr::raw(" AND id = "));
  fragment.append(expr::param(int64_t{42}));
  fragment.append(expr::raw(")"));

  EXPECT_EQ(fragment.sql_, "(name = ? AND id = ?)");
  ASSERT_EQ(fragment.binds_.size(), 2u);
  EXPECT_EQ(std::get<std::string>(fragment.binds_[0]), "x' OR 1=1 --");
  EXPECT_EQ(std::get<int64_t>(fragment.binds_[1]), 42);
}
