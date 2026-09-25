//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// LoadPackagedDuckDbExtension: the test target copies vss and fts next to the executable,
// the way the app ships them.

#include "storage/store/duckdb_extension.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"

namespace alcedo {
namespace {

void SetEnvironment(const char* name, const std::string& value) {
#ifdef _WIN32
  _putenv_s(name, value.c_str());
#else
  if (value.empty()) {
    unsetenv(name);
  } else {
    setenv(name, value.c_str(), 1);
  }
#endif
}

class DuckDbExtensionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(duckdb_open(nullptr, &db_), DuckDBSuccess);
    ASSERT_EQ(duckdb_connect(db_, &conn_), DuckDBSuccess);
  }
  void TearDown() override {
    SetEnvironment("ALCEDO_DUCKDB_NOSUCHEXT_EXTENSION", "");
    duckdb_disconnect(&conn_);
    duckdb_close(&db_);
  }

  auto ExtensionIsLoaded(const char* name) -> bool {
    auto query = duckorm::expr::raw(
        "SELECT COUNT(*) FROM duckdb_extensions() WHERE loaded AND extension_name = ");
    query.append(duckorm::expr::param(std::string(name)));
    return duckorm::select_int64(conn_, query).value_or(0) == 1;
  }

  duckdb_database   db_   = nullptr;
  duckdb_connection conn_ = nullptr;
};

TEST_F(DuckDbExtensionTest, PackagedExtensionsLoadFromTheExecutableDirectory) {
  std::string error;
  ASSERT_TRUE(LoadPackagedDuckDbExtension(conn_, "fts", &error)) << error;
  EXPECT_TRUE(ExtensionIsLoaded("fts"));
  ASSERT_TRUE(LoadPackagedDuckDbExtension(conn_, "vss", &error)) << error;
  EXPECT_TRUE(ExtensionIsLoaded("vss"));

  // The loaded extension works on another connection of the same database.
  duckdb_connection other = nullptr;
  ASSERT_EQ(duckdb_connect(db_, &other), DuckDBSuccess);
  duckorm::execute(other, duckorm::expr::raw("CREATE TABLE Doc (id BIGINT PRIMARY KEY, body "
                                             "VARCHAR); INSERT INTO Doc VALUES (1, 'lighthouse');"));
  // A PRAGMA is expanded before earlier statements of the same string run, so it goes alone.
  duckorm::execute(other, duckorm::expr::raw("PRAGMA create_fts_index('Doc', 'id', 'body');"));
  EXPECT_EQ(duckorm::select_int64(
                other, duckorm::expr::raw("SELECT COUNT(*) FROM Doc WHERE "
                                          "fts_main_Doc.match_bm25(id, 'lighthouses') IS NOT NULL")),
            1);
  duckdb_disconnect(&other);
}

TEST_F(DuckDbExtensionTest, MissingExtensionReportsEachFailedAttemptWithoutDownloading) {
  // A file named by the environment variable is tried first; this one is not an extension.
  const auto bogus = std::filesystem::temp_directory_path() / "nosuchext.duckdb_extension";
  std::ofstream(bogus) << "not an extension";
  SetEnvironment("ALCEDO_DUCKDB_NOSUCHEXT_EXTENSION", bogus.string());

  std::string error;
  EXPECT_FALSE(LoadPackagedDuckDbExtension(conn_, "nosuchext", &error));
  EXPECT_NE(error.find("Packaged extension load failed from " + bogus.generic_string()),
            std::string::npos)
      << error;
  EXPECT_NE(error.find("Extension load failed by name"), std::string::npos) << error;
  EXPECT_EQ(duckorm::select_string(conn_, duckorm::expr::raw(
                                              "SELECT current_setting('autoinstall_known_extensions')")),
            std::optional<std::string>("false"));

  std::error_code ec;
  std::filesystem::remove(bogus, ec);
}

}  // namespace
}  // namespace alcedo
