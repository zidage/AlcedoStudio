//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM5.1: the MaskAsset table of the project database (put once per key, zlib, verified read).

#include "storage/store/mask_asset/mask_asset_store.hpp"

#include <duckdb.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "edit/mask/mask_asset.hpp"
#include "storage/store/database.hpp"
#include "support/test_temp_directory.hpp"

namespace alcedo {
namespace {

/// 1024 x 768 R8 coverage with two filled rectangles on zero, like a two-region subject mask.
auto MakeTwoRegionAsset(float bounds_x = 0.125f) -> MaskAsset {
  constexpr std::uint32_t kWidth  = 1024;
  constexpr std::uint32_t kHeight = 768;
  MaskAsset               asset;
  asset.descriptor.extent           = {kWidth, kHeight};
  asset.descriptor.reference_bounds = NormalizedRect{bounds_x, 0.25f, 0.5f, 0.375f};
  asset.pixels.assign(static_cast<std::size_t>(kWidth) * kHeight, 0);
  const auto fill = [&asset](std::uint32_t x0, std::uint32_t y0, std::uint32_t x1,
                             std::uint32_t y1) {
    for (std::uint32_t y = y0; y < y1; ++y) {
      for (std::uint32_t x = x0; x < x1; ++x) {
        asset.pixels[static_cast<std::size_t>(y) * kWidth + x] = 255;
      }
    }
  };
  fill(64, 32, 400, 500);
  fill(600, 300, 980, 740);
  asset.key = MakeMaskAssetKey(asset.descriptor, asset.pixels);
  return asset;
}

auto QueryInt64(Database& database, const std::string& sql) -> std::int64_t {
  auto          guard = database.GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  if (duckdb_query(guard.conn_, sql.c_str(), &result) != DuckDBSuccess) {
    const std::string error = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(error);
  }
  const auto value = duckdb_value_int64(&result, 0, 0);
  duckdb_destroy_result(&result);
  return value;
}

void ExecuteSql(Database& database, const std::string& sql) {
  auto              guard = database.GetConnectionGuard();
  auto              lock  = guard.Lock();
  duckdb_result     result;
  const auto        state = duckdb_query(guard.conn_, sql.c_str(), &result);
  const std::string error = state == DuckDBSuccess ? "" : duckdb_result_error(&result);
  duckdb_destroy_result(&result);
  if (state != DuckDBSuccess) {
    throw std::runtime_error(error);
  }
}

/// Flip one byte in the middle of the stored `data` of @p key with a prepared UPDATE.
void ChangeStoredDataByte(Database& database, const MaskAssetKey& key) {
  auto          guard = database.GetConnectionGuard();
  auto          lock  = guard.Lock();
  duckdb_result result;
  const auto    select =
      "SELECT data FROM MaskAsset WHERE asset_key = '" + std::string{key.Value()} + "';";
  ASSERT_EQ(duckdb_query(guard.conn_, select.c_str(), &result), DuckDBSuccess);
  duckdb_blob               blob = duckdb_value_blob(&result, 0, 0);
  std::vector<std::uint8_t> bytes(static_cast<const std::uint8_t*>(blob.data),
                                  static_cast<const std::uint8_t*>(blob.data) + blob.size);
  duckdb_free(blob.data);
  duckdb_destroy_result(&result);
  ASSERT_GT(bytes.size(), 16u);
  bytes[bytes.size() / 2] ^= 0x5Au;

  duckdb_prepared_statement statement = nullptr;
  ASSERT_EQ(
      duckdb_prepare(guard.conn_, "UPDATE MaskAsset SET data = ? WHERE asset_key = ?;", &statement),
      DuckDBSuccess);
  duckdb_bind_blob(statement, 1, bytes.data(), static_cast<idx_t>(bytes.size()));
  const std::string key_text{key.Value()};
  duckdb_bind_varchar(statement, 2, key_text.c_str());
  ASSERT_EQ(duckdb_execute_prepared(statement, &result), DuckDBSuccess);
  EXPECT_EQ(duckdb_rows_changed(&result), 1u);
  duckdb_destroy_result(&result);
  duckdb_destroy_prepare(&statement);
}

auto ReadError(Database& database, const MaskAssetKey& key) -> std::string {
  auto           guard = database.GetConnectionGuard();
  auto           lock  = guard.Lock();
  MaskAssetStore store(guard.conn_);
  try {
    (void)store.Read(key);
  } catch (const std::runtime_error& error) {
    return error.what();
  }
  return {};
}

class MaskAssetStoreTest : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    db_path_  = temp_dir_ / "mask_assets.db";
    database_ = std::make_unique<Database>(db_path_);
  }

  void TearDown() override {
    database_.reset();
  }

  auto Put(const MaskAsset& asset) -> bool {
    auto           guard = database_->GetConnectionGuard();
    auto           lock  = guard.Lock();
    MaskAssetStore store(guard.conn_);
    return store.Put(asset);
  }

  auto Read(const MaskAssetKey& key) -> MaskAsset {
    auto           guard = database_->GetConnectionGuard();
    auto           lock  = guard.Lock();
    MaskAssetStore store(guard.conn_);
    return store.Read(key);
  }

  auto RowCount() -> std::int64_t {
    return QueryInt64(*database_, "SELECT COUNT(*) FROM MaskAsset;");
  }

  std::filesystem::path     db_path_;
  std::unique_ptr<Database> database_;
};

TEST_F(MaskAssetStoreTest, MaskAssetPutThenReadReturnsVerifiedPixels) {
  const auto asset = MakeTwoRegionAsset();
  EXPECT_TRUE(Put(asset));

  const auto read = Read(asset.key);
  EXPECT_EQ(read.key, asset.key);
  EXPECT_EQ(read.descriptor, asset.descriptor);
  EXPECT_EQ(read.pixels, asset.pixels);

  // The zlib stream of long 0 and 255 runs is much smaller than the raw pixels.
  const auto stored_bytes =
      QueryInt64(*database_, "SELECT octet_length(data) FROM MaskAsset WHERE asset_key = '" +
                                 std::string{asset.key.Value()} + "';");
  EXPECT_LT(stored_bytes, static_cast<std::int64_t>(asset.pixels.size()) / 20);
  EXPECT_EQ(QueryInt64(*database_, "SELECT pixel_byte_count FROM MaskAsset;"),
            static_cast<std::int64_t>(asset.pixels.size()));
}

TEST_F(MaskAssetStoreTest, MaskAssetPutOfExistingKeyWritesNothing) {
  const auto asset = MakeTwoRegionAsset();
  EXPECT_TRUE(Put(asset));
  EXPECT_FALSE(Put(asset));
  EXPECT_EQ(RowCount(), 1);
  EXPECT_EQ(Read(asset.key).pixels, asset.pixels);
}

TEST_F(MaskAssetStoreTest, MaskAssetPutRejectsWrongKey) {
  auto asset = MakeTwoRegionAsset();
  asset.key  = MaskAssetKey{std::string(32, 'f')};
  EXPECT_THROW((void)Put(asset), std::runtime_error);

  auto short_pixels = MakeTwoRegionAsset();
  short_pixels.pixels.pop_back();
  EXPECT_THROW((void)Put(short_pixels), std::runtime_error);
  EXPECT_EQ(RowCount(), 0);
}

TEST_F(MaskAssetStoreTest, MaskAssetReadReportsMissingAndCorruptRows) {
  const MaskAssetKey unknown{std::string(32, 'e')};
  EXPECT_EQ(ReadError(*database_, unknown),
            "Mask asset " + std::string(32, 'e') + " is missing from the project");

  const auto asset  = MakeTwoRegionAsset();
  const auto prefix = "Mask asset " + std::string{asset.key.Value()} + " is corrupt: ";
  ASSERT_TRUE(Put(asset));
  ChangeStoredDataByte(*database_, asset.key);
  // zlib checks the Adler-32 of the stream, so a changed data byte fails the inflate before the
  // key check.
  EXPECT_EQ(ReadError(*database_, asset.key).rfind(prefix, 0), 0u);

  const auto other = MakeTwoRegionAsset(0.25f);
  ASSERT_TRUE(Put(other));
  ExecuteSql(*database_, "UPDATE MaskAsset SET bounds_x = 0.375 WHERE asset_key = '" +
                             std::string{other.key.Value()} + "';");
  EXPECT_EQ(ReadError(*database_, other.key),
            "Mask asset " + std::string{other.key.Value()} + " is corrupt: key mismatch");

  ExecuteSql(*database_, "UPDATE MaskAsset SET pixel_byte_count = 12 WHERE asset_key = '" +
                             std::string{other.key.Value()} + "';");
  EXPECT_EQ(ReadError(*database_, other.key)
                .rfind("Mask asset " + std::string{other.key.Value()} + " is corrupt: ", 0),
            0u);
}

TEST_F(MaskAssetStoreTest, DeleteKeysNotInRemovesOnlyUnlistedRows) {
  const auto kept    = MakeTwoRegionAsset(0.125f);
  const auto removed = MakeTwoRegionAsset(0.25f);
  ASSERT_TRUE(Put(kept));
  ASSERT_TRUE(Put(removed));

  auto           guard = database_->GetConnectionGuard();
  auto           lock  = guard.Lock();
  MaskAssetStore store(guard.conn_);
  EXPECT_EQ(store.ListKeys().size(), 2u);
  EXPECT_EQ(store.DeleteKeysNotIn({kept.key}), 1u);
  EXPECT_EQ(store.ListKeys(), std::vector<MaskAssetKey>{kept.key});
  EXPECT_EQ(store.DeleteKeysNotIn({kept.key}), 0u);
  EXPECT_EQ(store.Read(kept.key).pixels, kept.pixels);
}

}  // namespace
}  // namespace alcedo
