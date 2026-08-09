//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "image/image.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/image/image_mapper.hpp"
#include "storage/store/database.hpp"
#include "type/type.hpp"

using namespace alcedo;

namespace {

class ImageMapperCrtpFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    // Match CommitGraphPersistenceTests: unique temp file; Database opens and inits tables.
    const auto stamp =
        std::to_string(std::chrono::high_resolution_clock::now().time_since_epoch().count());
    db_path_ = std::filesystem::temp_directory_path() / ("mapper_crtp_" + stamp + ".db");
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
    database_ = std::make_unique<Database>(db_path_);
    guard_    = std::make_unique<ConnectionGuard>(database_->GetConnectionGuard());
    mapper_   = std::make_unique<ImageMapper>(guard_->conn_);
  }

  void TearDown() override {
    mapper_.reset();
    guard_.reset();
    database_.reset();
    std::error_code ec;
    std::filesystem::remove(db_path_, ec);
  }

  std::filesystem::path            db_path_;
  std::unique_ptr<Database>        database_;
  std::unique_ptr<ConnectionGuard> guard_;
  std::unique_ptr<ImageMapper>     mapper_;
};

}  // namespace

TEST_F(ImageMapperCrtpFixture, ToParamsFromParamsRoundTripsImageIdentityAndPath) {
  auto source = std::make_shared<Image>(
      image_id_t{42}, std::filesystem::path(L"C:/photos/sample.dng"), L"sample.dng",
      ImageType::DNG);

  auto params = ImageMapper::ToParams(source);
  EXPECT_EQ(params.id, 42u);
  ASSERT_NE(params.image_path, nullptr);
  ASSERT_NE(params.file_name, nullptr);
  EXPECT_NE(params.image_path->find("sample.dng"), std::string::npos);
  EXPECT_EQ(*params.file_name, "sample.dng");
  EXPECT_EQ(params.type, static_cast<uint32_t>(ImageType::DNG));

  auto recovered = ImageMapper::FromParams(std::move(params));
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->image_id_, 42u);
  EXPECT_EQ(recovered->image_name_, L"sample.dng");
  EXPECT_EQ(recovered->image_type_, ImageType::DNG);
}

TEST_F(ImageMapperCrtpFixture, InsertAndGetByPredicateSqlFragmentSelectsInsertedImage) {
  auto source = std::make_shared<Image>(
      image_id_t{7}, std::filesystem::path(L"D:/library/frame.nef"), L"frame.nef", ImageType::DNG);
  mapper_->Insert(source);

  const auto where =
      duckorm::expr::eq(duckorm::expr::col("id"), duckorm::expr::lit(static_cast<int64_t>(7)));
  auto rows = mapper_->GetByPredicate(where);
  ASSERT_EQ(rows.size(), 1u);
  EXPECT_EQ(rows[0]->image_id_, 7u);
  EXPECT_EQ(rows[0]->image_name_, L"frame.nef");
}

TEST_F(ImageMapperCrtpFixture, RemoveByClauseSqlFragmentDeletesMatchingImageRow) {
  auto keep = std::make_shared<Image>(image_id_t{1}, std::filesystem::path(L"D:/a.dng"), L"a.dng",
                                      ImageType::DNG);
  auto drop = std::make_shared<Image>(image_id_t{2}, std::filesystem::path(L"D:/b.dng"), L"b.dng",
                                      ImageType::DNG);
  mapper_->Insert(keep);
  mapper_->Insert(drop);

  const auto where =
      duckorm::expr::eq(duckorm::expr::col("id"), duckorm::expr::lit(static_cast<int64_t>(2)));
  mapper_->RemoveByClause(where);

  auto remaining = mapper_->GetByPredicate(std::string("1=1"));
  ASSERT_EQ(remaining.size(), 1u);
  EXPECT_EQ(remaining[0]->image_id_, 1u);
}

TEST_F(ImageMapperCrtpFixture, MapperSelectByPredicateRejectsInjectPayloadAsLiteralMatchOnly) {
  auto source = std::make_shared<Image>(
      image_id_t{11}, std::filesystem::path(L"D:/library/safe.dng"), L"safe.dng", ImageType::DNG);
  mapper_->Insert(source);

  const auto inject_where = duckorm::expr::eq(
      duckorm::expr::col("file_name"), duckorm::expr::param(std::string("x' OR 1=1 --")));
  EXPECT_EQ(inject_where.sql_, "(file_name = ?)");
  ASSERT_EQ(inject_where.binds_.size(), 1u);
  EXPECT_EQ(std::get<std::string>(inject_where.binds_[0]), "x' OR 1=1 --");

  auto inject_rows = mapper_->GetByPredicate(inject_where);
  EXPECT_TRUE(inject_rows.empty());

  const auto keep_where =
      duckorm::expr::eq(duckorm::expr::col("id"), duckorm::expr::param(int64_t{11}));
  auto keep_rows = mapper_->GetByPredicate(keep_where);
  ASSERT_EQ(keep_rows.size(), 1u);
  EXPECT_EQ(keep_rows[0]->image_id_, 11u);
  EXPECT_EQ(keep_rows[0]->image_name_, L"safe.dng");
}
