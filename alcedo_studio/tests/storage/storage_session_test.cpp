//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// StorageSession: the session holds the database lock for its whole lifetime, and the image root
// write stores the history root and the element pipeline JSON together or not at all.

#include "storage/store/storage_session.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <future>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "../support/storage_test_access.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "sleeve/storage.hpp"
#include "support/test_temp_directory.hpp"

namespace alcedo {
namespace {

class StorageSessionTest : public ::testing::Test {
 protected:
  test::TestTempDirectory temp_dir_;
  void SetUp() override {
    db_path_ = temp_dir_ / "project.db";
    storage_ = std::make_unique<Storage>(db_path_);
  }

  void TearDown() override {
    storage_.reset();
  }

  /// True when a thread other than the caller can take the database lock now.
  auto OtherThreadCanTakeDatabaseLock() -> bool {
    auto guard = StorageTestAccess::GetDatabase(*storage_).GetConnectionGuard();
    return std::async(std::launch::async,
                      [&guard]() {
                        std::unique_lock lock(*guard.db_lock_, std::try_to_lock);
                        return lock.owns_lock();
                      })
        .get();
  }

  static auto EncodeRoots(std::initializer_list<sl_element_id_t> ids)
      -> std::vector<EncodedRootPipeline> {
    std::vector<EncodedRootPipeline> roots;
    for (const auto id : ids) {
      roots.push_back(
          CommitGraphStore::EncodeRootPipeline(id, CreateDefaultPipelineDocument(), std::nullopt));
    }
    return roots;
  }

  std::filesystem::path    db_path_;
  std::unique_ptr<Storage> storage_;
};

TEST_F(StorageSessionTest, SessionHoldsDatabaseLockForItsLifetime) {
  ASSERT_TRUE(OtherThreadCanTakeDatabaseLock());
  {
    auto session = storage_->OpenSession();
    EXPECT_FALSE(OtherThreadCanTakeDatabaseLock());
    // The lock is recursive: the thread of the session can still use the other stores.
    EXPECT_FALSE(storage_->GetElementStore().GetPipelineJsonByElementId(1).has_value());
    EXPECT_FALSE(OtherThreadCanTakeDatabaseLock());
  }
  EXPECT_TRUE(OtherThreadCanTakeDatabaseLock());
}

TEST_F(StorageSessionTest, ImageRootWriteStoresRootAndElementPipelineJson) {
  const auto roots = EncodeRoots({41, 42});
  {
    auto session = storage_->OpenSession();
    session.InsertImageRoots(roots, {R"({"image":41})", R"({"image":42})"});
  }

  auto session = storage_->OpenSession();
  for (const auto& root : roots) {
    const auto graph = session.CommitGraph().LoadGraph(root.element_id);
    ASSERT_TRUE(graph.has_value()) << root.element_id;
    EXPECT_EQ(graph->GetRootId(), root.root_id);
    EXPECT_TRUE(session.CommitGraph()
                    .GetRootSerializedPipelineState(root.element_id, root.root_id)
                    .has_value());
  }
  const auto json_41 = storage_->GetElementStore().GetPipelineJsonByElementId(41);
  ASSERT_TRUE(json_41.has_value());
  EXPECT_EQ(json_41->at("image").get<int>(), 41);
  const auto json_42 = storage_->GetElementStore().GetPipelineJsonByElementId(42);
  ASSERT_TRUE(json_42.has_value());
  EXPECT_EQ(json_42->at("image").get<int>(), 42);
}

TEST_F(StorageSessionTest, ImageRootWriteOfAStoredRootWritesNoPipelineJson) {
  {
    auto session = storage_->OpenSession();
    session.InsertImageRoots(EncodeRoots({51}), {R"({"image":51})"});
  }

  auto session = storage_->OpenSession();
  EXPECT_THROW(session.InsertImageRoots(EncodeRoots({52, 51}), {R"({"image":52})", R"({})"}),
               std::runtime_error);
  EXPECT_FALSE(session.CommitGraph().GetImageEditState(52).has_value());
  EXPECT_FALSE(storage_->GetElementStore().GetPipelineJsonByElementId(52).has_value());
  const auto json_51 = storage_->GetElementStore().GetPipelineJsonByElementId(51);
  ASSERT_TRUE(json_51.has_value());
  EXPECT_EQ(json_51->at("image").get<int>(), 51);
}

TEST_F(StorageSessionTest, ImageRootWriteRejectsAPipelineJsonCountThatDiffersFromTheRoots) {
  auto session = storage_->OpenSession();
  EXPECT_THROW(session.InsertImageRoots(EncodeRoots({61, 62}), {R"({"image":61})"}),
               std::invalid_argument);
  EXPECT_FALSE(session.CommitGraph().GetImageEditState(61).has_value());
  EXPECT_FALSE(storage_->GetElementStore().GetPipelineJsonByElementId(61).has_value());
}

}  // namespace
}  // namespace alcedo
