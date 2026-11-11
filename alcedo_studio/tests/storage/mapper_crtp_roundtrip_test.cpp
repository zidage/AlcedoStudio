//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <algorithm>
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
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/image/image_mapper.hpp"
#include "storage/mapper/sleeve/edit_history/version_ref_mapper.hpp"
#include "storage/store/database.hpp"
#include "type/type.hpp"
#include "support/test_temp_directory.hpp"

using namespace alcedo;

namespace {

class ImageMapperCrtpFixture : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    db_path_  = temp_dir_ / "mapper.db";
    database_ = std::make_unique<Database>(db_path_);
    guard_    = std::make_unique<ConnectionGuard>(database_->GetConnectionGuard());
    mapper_   = std::make_unique<ImageMapper>(guard_->conn_);
  }

  void TearDown() override {
    mapper_.reset();
    guard_.reset();
    database_.reset();
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

TEST_F(ImageMapperCrtpFixture, RemoveWhereSqlFragmentDeletesMatchingImageRow) {
  auto keep = std::make_shared<Image>(image_id_t{1}, std::filesystem::path(L"D:/a.dng"), L"a.dng",
                                      ImageType::DNG);
  auto drop = std::make_shared<Image>(image_id_t{2}, std::filesystem::path(L"D:/b.dng"), L"b.dng",
                                      ImageType::DNG);
  mapper_->Insert(keep);
  mapper_->Insert(drop);

  const auto where =
      duckorm::expr::eq(duckorm::expr::col("id"), duckorm::expr::lit(static_cast<int64_t>(2)));
  mapper_->RemoveWhere(where);

  auto remaining = mapper_->GetByPredicate(duckorm::expr::raw("TRUE"));
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

TEST_F(ImageMapperCrtpFixture, GetByIdReturnsNulloptForMissingKey) {
  mapper_->Insert(std::make_shared<Image>(image_id_t{21}, std::filesystem::path(L"D:/c.dng"),
                                          L"c.dng", ImageType::DNG));

  EXPECT_FALSE(mapper_->GetById(image_id_t{22}).has_value());
  const auto stored = mapper_->GetById(image_id_t{21});
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->id, 21u);
  ASSERT_NE(stored->file_name, nullptr);
  EXPECT_EQ(*stored->file_name, "c.dng");
}

TEST_F(ImageMapperCrtpFixture, GetImageByPathBindsPathWithQuote) {
  const std::filesystem::path quoted(L"D:/O'Brien/frame.dng");
  mapper_->Insert(std::make_shared<Image>(image_id_t{31}, quoted, L"frame.dng", ImageType::DNG));
  mapper_->Insert(std::make_shared<Image>(image_id_t{32}, std::filesystem::path(L"D:/other.dng"),
                                          L"other.dng", ImageType::DNG));

  const auto by_path = mapper_->GetImageByPath(quoted);
  ASSERT_EQ(by_path.size(), 1u);
  EXPECT_EQ(by_path[0]->image_id_, 31u);
  const auto by_name = mapper_->GetImageByName(L"other.dng");
  ASSERT_EQ(by_name.size(), 1u);
  EXPECT_EQ(by_name[0]->image_id_, 32u);
}

TEST_F(ImageMapperCrtpFixture, BatchInsertJoinsCallerTransaction) {
  const std::vector<std::shared_ptr<Image>> images = {
      std::make_shared<Image>(image_id_t{41}, std::filesystem::path(L"D:/d.dng"), L"d.dng",
                              ImageType::DNG),
      std::make_shared<Image>(image_id_t{42}, std::filesystem::path(L"D:/e.dng"), L"e.dng",
                              ImageType::DNG)};
  const auto count_images = [this] {
    return duckorm::scalar<int64_t>(guard_->conn_, duckorm::expr::raw("SELECT COUNT(*) FROM Image"))
        .value_or(-1);
  };
  {
    // A batch that opened its own transaction would throw here: DuckDB has no nested BEGIN.
    duckorm::Transaction transaction(guard_->conn_);
    mapper_->InsertBatch(images);
    EXPECT_EQ(count_images(), 2);
    // No commit: the destructor rolls the caller transaction back.
  }
  EXPECT_EQ(count_images(), 0);

  {
    duckorm::Transaction transaction(guard_->conn_);
    mapper_->InsertBatch(images);
    transaction.commit();
  }
  EXPECT_EQ(count_images(), 2);
}

namespace {

class VersionRefMapperKeyFixture : public ImageMapperCrtpFixture {
 protected:
  void InsertVersionRef(const std::string& version_id) {
    VersionRefMapperParams row;
    row.version_id       = std::make_unique<std::string>(version_id);
    row.element_id       = 5;
    row.display_name     = std::make_unique<std::string>("Version " + version_id);
    row.head_commit_hash = std::make_unique<std::string>("");
    VersionRefMapper(guard_->conn_).InsertParams(row);
  }

  auto StoredVersionIds() -> std::vector<std::string> {
    std::vector<std::string> ids;
    for (const auto& row : VersionRefMapper(guard_->conn_).GetWhere(duckorm::expr::raw("TRUE"))) {
      ids.push_back(*row.version_id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
};

}  // namespace

TEST_F(VersionRefMapperKeyFixture, RemoveByIdBindsTextKeyWithQuote) {
  InsertVersionRef("a");
  InsertVersionRef("a'b");
  InsertVersionRef("b");

  // As text pasted into `version_id='{}'`, the key `a'b` ends the literal after `a`.
  VersionRefMapper(guard_->conn_).RemoveById(std::string("a'b"));

  EXPECT_EQ(StoredVersionIds(), (std::vector<std::string>{"a", "b"}));
  EXPECT_FALSE(VersionRefMapper(guard_->conn_).GetById(std::string("a'b")).has_value());
  const auto kept = VersionRefMapper(guard_->conn_).GetById(std::string("a"));
  ASSERT_TRUE(kept.has_value());
  EXPECT_EQ(*kept->display_name, "Version a");
}
