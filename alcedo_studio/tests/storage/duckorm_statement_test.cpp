//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statement execution, single-value reads, transactions, and the appender of duckorm, run
// against an in-memory DuckDB database.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"

namespace {

namespace expr = duckorm::expr;

class DuckormStatementTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(duckdb_open(nullptr, &db_), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(db_, &conn_), DuckDBSuccess);
    duckorm::execute(conn_,
                     expr::raw("CREATE TABLE Item (id BIGINT PRIMARY KEY, name VARCHAR, "
                               "score DOUBLE, ok BOOLEAN, vec FLOAT[3]);"));
  }
  void TearDown() override {
    duckdb_disconnect(&conn_);
    duckdb_close(&db_);
  }

  auto CountItems() -> int64_t {
    return duckorm::select_int64(conn_, expr::raw("SELECT COUNT(*) FROM Item")).value_or(-1);
  }

  duckdb_database   db_   = nullptr;
  duckdb_connection conn_ = nullptr;
};

auto Statement(const char* sql, std::vector<duckorm::BindValue> binds = {})
    -> duckorm::SqlFragment {
  return duckorm::SqlFragment{sql, std::move(binds)};
}

TEST_F(DuckormStatementTest, ExecuteRunsBoundAndUnboundStatementsAndThrowsDuckDbError) {
  duckorm::execute(conn_, Statement("INSERT INTO Item (id, name) VALUES (?, ?)",
                                    {int64_t{1}, std::string("it's")}));
  // Several statements without binds run in one call.
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id) VALUES (2); INSERT INTO Item (id) "
                                    "VALUES (3);"));
  EXPECT_EQ(CountItems(), 3);
  EXPECT_EQ(duckorm::select_string(conn_, Statement("SELECT name FROM Item WHERE id = ?",
                                                    {int64_t{1}})),
            std::optional<std::string>("it's"));

  try {
    duckorm::execute(conn_, expr::raw("INSERT INTO Item (id) VALUES (1);"));
    FAIL() << "a duplicate key must throw";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("Duplicate key"), std::string::npos) << error.what();
  }
  EXPECT_THROW(duckorm::execute(conn_, Statement("DELETE FROM Missing WHERE id = ?", {int64_t{1}})),
               std::runtime_error);
}

TEST_F(DuckormStatementTest, ScalarSelectsReturnNulloptForNoRowAndForNull) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name, score) VALUES (7, NULL, 2.5)"));
  EXPECT_EQ(duckorm::select_int64(conn_, expr::raw("SELECT id FROM Item")), 7);
  EXPECT_EQ(duckorm::select_int64(conn_, expr::raw("SELECT id FROM Item WHERE id = 8")),
            std::nullopt);
  EXPECT_EQ(duckorm::select_string(conn_, expr::raw("SELECT name FROM Item")), std::nullopt);
  EXPECT_EQ(duckorm::select_string(conn_, expr::raw("SELECT score FROM Item")),
            std::optional<std::string>("2.5"));
  EXPECT_THROW((void)duckorm::select_int64(conn_, expr::raw("SELECT nope FROM Item")),
               std::runtime_error);
}

TEST_F(DuckormStatementTest, SelectByQueryBindsValuesAndDecodesRows) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name, score) VALUES (1, 'a', 0.5), "
                                    "(2, 'b', NULL), (3, 'c', 1.5)"));
  constexpr std::array<duckorm::DuckFieldDesc, 3> fields = {
      duckorm::DuckFieldDesc{"id", duckorm::DuckDBType::INT64, 0},
      duckorm::DuckFieldDesc{"name", duckorm::DuckDBType::VARCHAR, 0},
      duckorm::DuckFieldDesc{"score", duckorm::DuckDBType::NULLABLE_DOUBLE, 0}};
  const auto rows = duckorm::select_by_query(
      conn_, fields, fields.size(),
      Statement("SELECT id, name, score FROM Item WHERE id >= ? ORDER BY id", {int64_t{2}}));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(std::get<int64_t>(rows[0][0]), 2);
  EXPECT_EQ(*std::get<std::unique_ptr<std::string>>(rows[0][1]), "b");
  EXPECT_EQ(std::get<std::optional<double>>(rows[0][2]), std::nullopt);
  EXPECT_EQ(std::get<std::optional<double>>(rows[1][2]), 1.5);
}

TEST_F(DuckormStatementTest, CellReadersReturnTextBooleanAndEmptyForNull) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name, ok) VALUES (1, 'a', TRUE), "
                                    "(2, NULL, FALSE), (3, 'c', NULL)"));
  constexpr std::array<duckorm::DuckFieldDesc, 2> fields = {
      duckorm::DuckFieldDesc{"name", duckorm::DuckDBType::NULLABLE_STRING, 0},
      duckorm::DuckFieldDesc{"ok", duckorm::DuckDBType::BOOLEAN, 0}};
  auto query = expr::raw("SELECT name, ok FROM Item WHERE ");
  query.append(expr::column_eq("id", int64_t{1}));
  auto rows = duckorm::select_by_query(conn_, fields, fields.size(), query);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(duckorm::cell_text(rows[0][0]), "a");
  EXPECT_TRUE(duckorm::cell_bool(rows[0][1]));

  rows = duckorm::select_by_query(conn_, fields, fields.size(),
                                  expr::raw("SELECT name, ok FROM Item WHERE id > 1 ORDER BY id"));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(duckorm::cell_text(rows[0][0]), "");  // NULL
  EXPECT_FALSE(duckorm::cell_bool(rows[0][1]));
  EXPECT_FALSE(duckorm::cell_bool(rows[1][1]));  // NULL

  const auto by_name = expr::column_eq("name", std::string("c"));
  EXPECT_EQ(by_name.sql_, "(name = ?)");
  ASSERT_EQ(by_name.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(by_name.binds_[0]), "c");
}

TEST_F(DuckormStatementTest, TransactionCommitsOnlyWhenCommitIsCalled) {
  {
    duckorm::Transaction transaction(conn_);
    duckorm::execute(conn_, expr::raw("INSERT INTO Item (id) VALUES (1)"));
    transaction.commit();
  }
  EXPECT_EQ(CountItems(), 1);

  try {
    duckorm::Transaction transaction(conn_);
    duckorm::execute(conn_, expr::raw("INSERT INTO Item (id) VALUES (2)"));
    duckorm::execute(conn_, expr::raw("INSERT INTO Item (id) VALUES (1)"));  // duplicate key
    transaction.commit();
    FAIL() << "the duplicate key must throw";
  } catch (const std::runtime_error&) {
  }
  // Row 2 was rolled back, and the connection accepts a new transaction.
  EXPECT_EQ(CountItems(), 1);
  {
    duckorm::Transaction transaction(conn_);
    duckorm::execute(conn_, expr::raw("INSERT INTO Item (id) VALUES (3)"));
  }  // no commit: rolled back
  EXPECT_EQ(CountItems(), 1);
}

TEST_F(DuckormStatementTest, AppenderWritesEveryValueTypeInsideTheTransaction) {
  constexpr std::array<const char*, 5> columns = {"id", "name", "score", "ok", "vec"};
  const std::array<float, 3>           vec     = {0.25F, -1.0F, 3.5F};
  {
    duckorm::Transaction transaction(conn_);
    duckorm::Appender    appender(conn_, "Item", columns);
    appender.append_uint32(1);
    appender.append_varchar("first");
    appender.append_double(0.5);
    appender.append_bool(true);
    appender.append_float_array(vec);
    appender.end_row();
    appender.append_int32(2);
    appender.append_null();
    appender.append_null();
    appender.append_bool(false);
    appender.append_float_array(vec);
    appender.end_row();
    appender.flush();
    transaction.commit();
  }
  EXPECT_EQ(CountItems(), 2);
  EXPECT_EQ(duckorm::select_string(conn_, expr::raw("SELECT name FROM Item WHERE id = 1")),
            std::optional<std::string>("first"));
  EXPECT_EQ(duckorm::select_string(conn_, expr::raw("SELECT name FROM Item WHERE id = 2")),
            std::nullopt);
  EXPECT_EQ(duckorm::select_string(conn_, expr::raw("SELECT vec[3] FROM Item WHERE id = 2")),
            std::optional<std::string>("3.5"));

  // A wrong array size fails with the appender's error.
  duckorm::Appender          appender(conn_, "Item", columns);
  const std::array<float, 2> short_vec = {1.0F, 2.0F};
  appender.append_uint32(3);
  appender.append_varchar("short");
  appender.append_double(1.0);
  appender.append_bool(true);
  EXPECT_THROW(
      {
        appender.append_float_array(short_vec);
        appender.end_row();
        appender.flush();
      },
      std::runtime_error);
  EXPECT_THROW(duckorm::Appender(conn_, "Missing", columns), std::runtime_error);
}

TEST(DuckormExprListTest, InListWritesIntegerLiteralsAndFalseForAnEmptyList) {
  const std::array<uint32_t, 3> ids = {4, 10, 7};
  const auto in = expr::in_list(expr::col("file_id"), std::span<const uint32_t>(ids));
  EXPECT_EQ(in.sql_, "file_id IN (4, 10, 7)");
  EXPECT_TRUE(in.binds_.empty());
  EXPECT_EQ(expr::in_list(expr::col("file_id"), std::span<const uint32_t>()).sql_, "FALSE");
}

TEST(DuckormExprListTest, FloatArrayLiteralKeepsEveryFloatDigit) {
  const std::array<float, 3> values = {0.1F, -2.0F, 1.0e-7F};
  EXPECT_EQ(expr::lit_float_array(values).sql_, "[0.100000001,-2,1.00000001e-07]::FLOAT[3]");
}

}  // namespace
