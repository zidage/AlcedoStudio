//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Statement execution, typed rows, single-value reads, transactions, and the appender of
// duckorm, run against an in-memory DuckDB database.

#include <duckdb.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/duckorm/duckdb_select.hpp"
#include "storage/mapper/duckorm/duckdb_statement.hpp"

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
  EXPECT_EQ(duckorm::scalar<double>(conn_, expr::raw("SELECT score FROM Item")), 2.5);
  // An INTEGER column widens into int64_t.
  EXPECT_EQ(duckorm::select_int64(conn_, expr::raw("SELECT 1 FROM Item")), 1);
  EXPECT_THROW((void)duckorm::select_int64(conn_, expr::raw("SELECT nope FROM Item")),
               std::runtime_error);
}

TEST_F(DuckormStatementTest, ScalarRejectsColumnTypeAndColumnCountMismatch) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name, score) VALUES (7, 'a', 2.5)"));
  try {
    (void)duckorm::select_string(conn_, expr::raw("SELECT score FROM Item"));
    FAIL() << "a DOUBLE column must not read as text";
  } catch (const duckorm::StatementError& error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("column 0 'score' has type DOUBLE"), std::string::npos) << message;
    EXPECT_NE(message.find("std::string"), std::string::npos) << message;
    // The SQL text goes to the log and to sql_head(), never into the message a user sees.
    EXPECT_EQ(message.find("SELECT score"), std::string::npos) << message;
    EXPECT_EQ(error.sql_head(), "SELECT score FROM Item");
  }
  EXPECT_THROW((void)duckorm::scalar<int64_t>(conn_, expr::raw("SELECT id, name FROM Item")),
               std::runtime_error);
  EXPECT_THROW((void)duckorm::scalar<int64_t>(conn_, expr::raw("SELECT SUM(id) FROM Item")),
               std::runtime_error);  // HUGEINT does not fit int64_t
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
  EXPECT_EQ(duckorm::scalar<float>(conn_, expr::raw("SELECT vec[3] FROM Item WHERE id = 2")), 3.5F);

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

namespace clause = duckorm::clause;

struct ItemBonusRow {
  int64_t item_id = 0;
  int64_t bonus   = 0;
  using Columns   = duckorm::RowColumns<&ItemBonusRow::item_id, &ItemBonusRow::bonus>;
};

// GROUPING() is BIGINT, so the grouping id reads as int64_t.
struct GroupingRow {
  int64_t                    grouping_id = 0;
  std::optional<std::string> name;
  int64_t                    count = 0;
  using Columns =
      duckorm::RowColumns<&GroupingRow::grouping_id, &GroupingRow::name, &GroupingRow::count>;
};

struct IdRow {
  int64_t id    = 0;
  using Columns = duckorm::RowColumns<&IdRow::id>;
};

TEST_F(DuckormStatementTest, SelectClausesKeepBindsInSqlOrder) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name, score) VALUES (1, 'a', 1.0), "
                                    "(2, 'b', 2.0), (3, 'a', 3.0), (4, 'c', 4.0), (5, 'a', 5.0)"));
  // Binds in the projection, the source, the predicate, and the page must reach their own
  // placeholders: 10 (projection), 'a' (join), 1.5 (predicate), then LIMIT 2 OFFSET 1.
  auto bonus = expr::raw("id + ");
  bonus.append(expr::param(int64_t{10}));
  const std::vector<duckorm::SqlFragment> projection{clause::as(expr::col("id"), "item_id"),
                                                     clause::as(std::move(bonus), "bonus")};
  auto                                    source = expr::raw("FROM Item JOIN (SELECT ");
  source.append(expr::param("a"));
  source.sql_.append(" AS wanted) w ON Item.name = w.wanted");
  auto statement = clause::select_query(projection, source);
  statement.append(clause::where(expr::gt(expr::col("score"), expr::param(1.5))));
  statement.append(clause::order_by(std::vector<duckorm::OrderTerm>{
      {expr::col("id"), duckorm::OrderDirection::kDescending, duckorm::NullPlacement::kLast}}));
  statement.append(clause::limit_offset(2, 1, 1000));
  ASSERT_EQ(statement.binds_.size(), 5u);

  const auto rows = duckorm::query<ItemBonusRow>(conn_, statement);
  ASSERT_EQ(rows.size(), 1u);  // rows 5 and 3 match; offset 1 leaves 3
  EXPECT_EQ(rows[0].item_id, 3);
  EXPECT_EQ(rows[0].bonus, 13);
}

TEST_F(DuckormStatementTest, GroupingSetsDistinguishUnknownFromTotal) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name) VALUES (1, 'a'), (2, NULL), "
                                    "(3, 'a'), (4, NULL), (5, 'b')"));
  const std::vector<std::vector<duckorm::SqlFragment>> sets{{expr::col("name")}, {}};
  auto                                                 statement =
      clause::select_query(std::vector{clause::as(expr::raw("GROUPING(name)"), "g"),
                                       expr::col("name"), clause::as(clause::count_all(), "c")},
                           expr::raw("FROM Item"));
  statement.append(clause::grouping_sets(sets));
  statement.append(clause::order_by(std::vector<duckorm::OrderTerm>{
      {expr::col("g"), duckorm::OrderDirection::kAscending, duckorm::NullPlacement::kLast},
      {expr::col("name"), duckorm::OrderDirection::kAscending, duckorm::NullPlacement::kLast}}));

  const auto rows = duckorm::query<GroupingRow>(conn_, statement);
  ASSERT_EQ(rows.size(), 4u);
  // a, b, the real NULL-key group (GROUPING 0), then the total (GROUPING 1, also NULL name).
  EXPECT_EQ(rows[2].grouping_id, 0);
  EXPECT_EQ(rows[2].name, std::nullopt);
  EXPECT_EQ(rows[2].count, 2);
  EXPECT_EQ(rows[3].grouping_id, 1);
  EXPECT_EQ(rows[3].name, std::nullopt);
  EXPECT_EQ(rows[3].count, 5);
}

TEST_F(DuckormStatementTest, OrderByWritesExplicitNullPlacementForBothDirections) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, score) VALUES (1, 2.0), (2, NULL), "
                                    "(3, 1.0)"));
  for (const auto direction :
       {duckorm::OrderDirection::kAscending, duckorm::OrderDirection::kDescending}) {
    auto statement = clause::select_query(std::vector{expr::col("id")}, expr::raw("FROM Item"));
    statement.append(clause::order_by(std::vector<duckorm::OrderTerm>{
        {expr::col("score"), direction, duckorm::NullPlacement::kLast}}));
    EXPECT_NE(statement.sql_.find("NULLS LAST"), std::string::npos);
    const auto rows = duckorm::query<IdRow>(conn_, statement);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[2].id, 2);  // the NULL score is last
  }
}

TEST(DuckormClauseTest, ClauseBuildersRejectInvalidInput) {
  const auto source = expr::raw("FROM Item");
  EXPECT_THROW(clause::select_query(std::vector<duckorm::SqlFragment>{}, source),
               std::invalid_argument);
  EXPECT_THROW(clause::select_query(std::vector{expr::col("id")}, duckorm::SqlFragment{}),
               std::invalid_argument);
  EXPECT_THROW(clause::select_query(std::vector{duckorm::SqlFragment{}}, source),
               std::invalid_argument);
  EXPECT_THROW(clause::as(expr::col("id"), "bad alias"), std::invalid_argument);
  EXPECT_THROW(clause::as(expr::col("id"), "1st"), std::invalid_argument);
  EXPECT_THROW(clause::group_by(std::vector<duckorm::SqlFragment>{}), std::invalid_argument);
  EXPECT_THROW(clause::grouping_sets(std::vector<std::vector<duckorm::SqlFragment>>{{}}),
               std::invalid_argument);
  EXPECT_THROW(clause::order_by(std::vector<duckorm::OrderTerm>{}), std::invalid_argument);
  EXPECT_THROW(clause::order_by(std::vector<duckorm::OrderTerm>{
                   {expr::col("id"), static_cast<duckorm::OrderDirection>(9),
                    duckorm::NullPlacement::kLast}}),
               std::invalid_argument);
  EXPECT_THROW(clause::order_by(std::vector<duckorm::OrderTerm>{
                   {expr::col("id"), duckorm::OrderDirection::kAscending,
                    static_cast<duckorm::NullPlacement>(9)}}),
               std::invalid_argument);
  EXPECT_THROW(clause::limit_offset(0, 0, 1000), std::invalid_argument);
  EXPECT_THROW(clause::limit_offset(1001, 0, 1000), std::invalid_argument);
  EXPECT_THROW(clause::limit_offset(10, -1, 1000), std::invalid_argument);
  EXPECT_TRUE(clause::where(duckorm::SqlFragment{}).empty());
}

// --- Typed rows (storage plan S1) ------------------------------------------------------------

struct NameCountRow {
  std::string name;
  int64_t     count = 0;
  using Columns     = duckorm::RowColumns<&NameCountRow::name, &NameCountRow::count>;
};

struct OptionalNameRow {
  int64_t                    id = 0;
  std::optional<std::string> name;
  std::optional<double>      score;
  using Columns =
      duckorm::RowColumns<&OptionalNameRow::id, &OptionalNameRow::name, &OptionalNameRow::score>;
};

struct BlobRow {
  int64_t                   id = 0;
  std::vector<std::uint8_t> data;
  using Columns = duckorm::RowColumns<&BlobRow::id, &BlobRow::data>;
};

struct NarrowRow {
  float        value = 0.0F;
  std::uint8_t level = 0;
  int16_t      small = 0;
  using Columns      = duckorm::RowColumns<&NarrowRow::value, &NarrowRow::level, &NarrowRow::small>;
};

struct DateRow {
  std::chrono::sys_days day;
  using Columns = duckorm::RowColumns<&DateRow::day>;
};

auto ThrownMessage(const std::function<void()>& run) -> std::string {
  try {
    run();
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

/// The sql_head() of the StatementError that @p run throws; "<none>" without one.
auto ThrownSqlHead(const std::function<void()>& run) -> std::string {
  try {
    run();
  } catch (const duckorm::StatementError& error) {
    return error.sql_head();
  }
  return "<none>";
}

TEST_F(DuckormStatementTest, QueryDecodesRowsIntoMemberPointerColumns) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name) VALUES (1, 'b'), (2, 'a'), "
                                    "(3, 'b'), (4, 'b')"));
  const auto rows = duckorm::query<NameCountRow>(
      conn_, Statement("SELECT name, COUNT(*) FROM Item WHERE id <= ? GROUP BY name ORDER BY name",
                       {int64_t{3}}));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].name, "a");
  EXPECT_EQ(rows[0].count, 1);
  EXPECT_EQ(rows[1].name, "b");
  EXPECT_EQ(rows[1].count, 2);

  EXPECT_TRUE(
      duckorm::query<NameCountRow>(conn_, expr::raw("SELECT name, id FROM Item WHERE id > 9"))
          .empty());
}

struct NameCountTotalRow : NameCountRow {
  int64_t total = 0;
  using Columns = duckorm::AppendRowColumns<NameCountRow::Columns, &NameCountTotalRow::total>;
};

TEST_F(DuckormStatementTest, DerivedRowReadsBaseColumnsThenOwnColumns) {
  static_assert(NameCountTotalRow::Columns::kCount == 3);
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name) VALUES (1, 'b'), (2, 'a'), "
                                    "(3, 'b')"));
  const auto rows = duckorm::query<NameCountTotalRow>(
      conn_, expr::raw("SELECT name, COUNT(*), SUM(COUNT(*)) OVER ()::BIGINT FROM Item "
                       "GROUP BY name ORDER BY name"));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].name, "a");
  EXPECT_EQ(rows[0].count, 1);
  EXPECT_EQ(rows[0].total, 3);
  EXPECT_EQ(rows[1].name, "b");
  EXPECT_EQ(rows[1].count, 2);
  EXPECT_EQ(rows[1].total, 3);

  // The base columns come first: a total column in the place of the name column is rejected.
  const auto message = ThrownMessage([&] {
    (void)duckorm::query<NameCountTotalRow>(conn_, expr::raw("SELECT id, id, id FROM Item"));
  });
  EXPECT_NE(message.find("column 0 'id' has type BIGINT"), std::string::npos) << message;
  EXPECT_NE(message.find("member type std::string"), std::string::npos) << message;
}

TEST_F(DuckormStatementTest, QueryRejectsColumnCountMismatch) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name) VALUES (1, 'a')"));
  const auto message = ThrownMessage([&] {
    (void)duckorm::query<NameCountRow>(conn_, expr::raw("SELECT name, id, score FROM Item"));
  });
  EXPECT_NE(message.find("returns 3 columns; the row type lists 2"), std::string::npos) << message;
  // The count is checked before any row is read, so an empty result fails the same way.
  EXPECT_THROW((void)duckorm::query<NameCountRow>(
                   conn_, expr::raw("SELECT name, id, score FROM Item WHERE FALSE")),
               std::runtime_error);
}

TEST_F(DuckormStatementTest, QueryRejectsColumnTypeMismatch) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name) VALUES (1, 'a')"));
  const auto message = ThrownMessage(
      [&] { (void)duckorm::query<NameCountRow>(conn_, expr::raw("SELECT id, name FROM Item")); });
  EXPECT_NE(message.find("column 0 'id' has type BIGINT"), std::string::npos) << message;
  EXPECT_NE(message.find("member type std::string"), std::string::npos) << message;

  // A VARCHAR column into the int64_t member names column 1.
  const auto second = ThrownMessage(
      [&] { (void)duckorm::query<NameCountRow>(conn_, expr::raw("SELECT name, name FROM Item")); });
  EXPECT_NE(second.find("column 1 'name' has type VARCHAR"), std::string::npos) << second;
  EXPECT_NE(second.find("member type int64_t"), std::string::npos) << second;
}

TEST_F(DuckormStatementTest, NullCellReadsAsNulloptForOptionalMember) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name, score) VALUES (1, NULL, NULL), "
                                    "(2, 'b', 0.5)"));
  const auto rows = duckorm::query<OptionalNameRow>(
      conn_, expr::raw("SELECT id, name, score FROM Item ORDER BY id"));
  ASSERT_EQ(rows.size(), 2u);
  EXPECT_EQ(rows[0].name, std::nullopt);
  EXPECT_EQ(rows[0].score, std::nullopt);
  EXPECT_EQ(rows[1].name, std::optional<std::string>("b"));
  EXPECT_EQ(rows[1].score, std::optional<double>(0.5));

  const auto literal_null =
      duckorm::query<OptionalNameRow>(conn_, expr::raw("SELECT 1, NULL::VARCHAR, NULL::DOUBLE"));
  ASSERT_EQ(literal_null.size(), 1u);
  EXPECT_EQ(literal_null[0].name, std::nullopt);
}

TEST_F(DuckormStatementTest, NullCellThrowsForPlainMember) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name) VALUES (1, 'a'), (2, NULL)"));
  const auto message = ThrownMessage([&] {
    (void)duckorm::query<NameCountRow>(conn_, expr::raw("SELECT name, id FROM Item ORDER BY id"));
  });
  EXPECT_NE(message.find("column 0 'name' is NULL in row 1"), std::string::npos) << message;
  EXPECT_NE(ThrownMessage([&] {
              (void)duckorm::query<NameCountRow>(conn_, expr::raw("SELECT NULL::VARCHAR, 1"));
            }).find("is NULL in row 0"),
            std::string::npos);
}

TEST_F(DuckormStatementTest, BlobBindAndReadKeepsBytes) {
  duckorm::execute(conn_, expr::raw("CREATE TABLE Packed (id BIGINT PRIMARY KEY, data BLOB)"));
  std::vector<std::uint8_t> bytes(256);
  for (size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(i);
  }
  // A Statement bind of a span, and a SqlFragment bind through expr::param.
  {
    duckorm::Statement insert(conn_, "INSERT INTO Packed VALUES (?, ?)");
    insert.Bind(1, int64_t{1});
    insert.Bind(2, std::span<const std::uint8_t>(bytes));
    EXPECT_EQ(insert.Execute().RowsChanged(), 1u);
    insert.Bind(1, int64_t{2});
    insert.Bind(2, std::span<const std::uint8_t>());
    EXPECT_EQ(insert.Execute().RowsChanged(), 1u);
  }
  auto insert = expr::raw("INSERT INTO Packed VALUES (3, ");
  insert.append(expr::param(std::span<const std::uint8_t>(bytes).subspan(250)));
  insert.append(expr::raw(")"));
  EXPECT_EQ(duckorm::execute_count(conn_, insert), 1u);

  const auto rows =
      duckorm::query<BlobRow>(conn_, expr::raw("SELECT id, data FROM Packed ORDER BY id"));
  ASSERT_EQ(rows.size(), 3u);
  EXPECT_EQ(rows[0].data, bytes);
  EXPECT_TRUE(rows[1].data.empty());
  EXPECT_EQ(rows[2].data, std::vector<std::uint8_t>(bytes.begin() + 250, bytes.end()));
  EXPECT_EQ(duckorm::select_int64(conn_, expr::raw("SELECT octet_length(data) FROM Packed WHERE "
                                                   "id = 1")),
            256);
}

TEST_F(DuckormStatementTest, FloatAndUtinyintColumnsDecode) {
  duckorm::execute(conn_, expr::raw("CREATE TABLE Narrow (value FLOAT, level UTINYINT, small "
                                    "SMALLINT)"));
  duckorm::Statement insert(conn_, "INSERT INTO Narrow VALUES (?, ?, ?)");
  insert.Bind(1, 0.375F);
  insert.Bind(2, std::uint8_t{255});
  insert.Bind(3, int16_t{-7});
  (void)insert.Execute();

  const auto row = duckorm::query_one<NarrowRow>(conn_, expr::raw("SELECT * FROM Narrow"));
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(row->value, 0.375F);
  EXPECT_EQ(row->level, 255);
  EXPECT_EQ(row->small, -7);
  // FLOAT widens into double; UTINYINT widens into uint32_t and into int16_t.
  EXPECT_EQ(duckorm::scalar<double>(conn_, expr::raw("SELECT value FROM Narrow")), 0.375);
  EXPECT_EQ(duckorm::scalar<uint32_t>(conn_, expr::raw("SELECT level FROM Narrow")), 255u);
  EXPECT_EQ(duckorm::scalar<int16_t>(conn_, expr::raw("SELECT level FROM Narrow")), 255);
  // Narrowing and signedness changes are rejected: DOUBLE into float, SMALLINT into uint16_t,
  // BIGINT into int32_t.
  EXPECT_THROW((void)duckorm::scalar<float>(conn_, expr::raw("SELECT value::DOUBLE FROM Narrow")),
               std::runtime_error);
  EXPECT_THROW((void)duckorm::scalar<uint16_t>(conn_, expr::raw("SELECT small FROM Narrow")),
               std::runtime_error);
  EXPECT_THROW((void)duckorm::scalar<int32_t>(conn_, expr::raw("SELECT 7::BIGINT")),
               std::runtime_error);
}

TEST_F(DuckormStatementTest, DateColumnDecodesAndBindsAsSysDays) {
  using namespace std::chrono;
  const sys_days     day = year{2026} / November / 9;
  duckorm::Statement select(conn_, "SELECT ?::DATE + 1");
  select.Bind(1, day);
  EXPECT_EQ(select.Execute().Get<sys_days>(0, 0), day + days{1});

  const auto row = duckorm::query_one<DateRow>(conn_, expr::raw("SELECT DATE '1969-12-31'"));
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(row->day, sys_days{year{1969} / December / 31});
}

TEST_F(DuckormStatementTest, ExecuteCountReportsChangedRows) {
  const auto insert = Statement("INSERT INTO Item (id, name) VALUES (?, ?) ON CONFLICT DO NOTHING",
                                {int64_t{1}, std::string("a")});
  EXPECT_EQ(duckorm::execute_count(conn_, insert), 1u);
  EXPECT_EQ(duckorm::execute_count(conn_, insert), 0u);  // a stored key writes nothing
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id) VALUES (2), (3)"));
  EXPECT_EQ(duckorm::execute_count(
                conn_, Statement("UPDATE Item SET score = ? WHERE id > ?", {1.5, int64_t{1}})),
            2u);
  EXPECT_EQ(duckorm::execute_count(conn_, expr::raw("DELETE FROM Item")), 3u);
}

TEST_F(DuckormStatementTest, QueryOneReturnsNulloptForNoRowAndThrowsForTwoRows) {
  duckorm::execute(conn_, expr::raw("INSERT INTO Item (id, name) VALUES (1, 'a'), (2, 'b')"));
  EXPECT_EQ(
      duckorm::query_one<NameCountRow>(conn_, expr::raw("SELECT name, id FROM Item WHERE id = 9")),
      std::nullopt);
  const auto row = duckorm::query_one<NameCountRow>(
      conn_, Statement("SELECT name, id FROM Item WHERE id = ?", {int64_t{2}}));
  ASSERT_TRUE(row.has_value());
  EXPECT_EQ(row->name, "b");
  const auto message = ThrownMessage([&] {
    (void)duckorm::query_one<NameCountRow>(conn_, expr::raw("SELECT name, id FROM Item"));
  });
  EXPECT_NE(message.find("returns 2 rows"), std::string::npos) << message;
}

TEST_F(DuckormStatementTest, StatementRebindsAndReportsPrepareAndBindErrors) {
  duckorm::Statement insert(conn_, "INSERT INTO Item (id, name, ok) VALUES (?, ?, ?)");
  EXPECT_EQ(insert.ParameterCount(), 3u);
  for (int64_t id = 1; id <= 3; ++id) {
    insert.Bind(1, id);
    insert.Bind(2, std::optional<std::string>{});  // NULL
    insert.Bind(3, id % 2 == 1);
    EXPECT_EQ(insert.Execute().RowsChanged(), 1u);
  }
  EXPECT_EQ(CountItems(), 3);
  EXPECT_EQ(duckorm::select_int64(conn_, expr::raw("SELECT COUNT(*) FROM Item WHERE ok AND name "
                                                   "IS NULL")),
            2);
  // A C string binds as VARCHAR, never as bool.
  duckorm::Statement by_name(conn_, "SELECT COUNT(*) FROM Item WHERE name = ?");
  by_name.Bind(1, "x");
  EXPECT_EQ(by_name.Execute().Get<int64_t>(0, 0), 0);

  EXPECT_THROW(insert.Bind(4, int64_t{1}), std::runtime_error);
  const auto prepare       = [&] { duckorm::Statement broken(conn_, "SELECT FROM WHERE nothing"); };
  const auto prepare_error = ThrownMessage(prepare);
  EXPECT_FALSE(prepare_error.empty());
  EXPECT_EQ(prepare_error.find("[SQL:"), std::string::npos) << prepare_error;
  EXPECT_EQ(ThrownSqlHead(prepare), "SELECT FROM WHERE nothing");
  // The SQL head stops at 120 bytes.
  const std::string long_sql = "SELECT nope" + std::string(200, ' ') + "FROM Item";
  EXPECT_EQ(ThrownSqlHead([&] { duckorm::Statement broken(conn_, long_sql); }),
            long_sql.substr(0, 120));
}

}  // namespace
