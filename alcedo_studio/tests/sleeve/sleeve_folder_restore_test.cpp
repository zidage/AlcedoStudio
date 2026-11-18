// Copyright 2026 Alcedo Studio contributors
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_element/sleeve_folder.hpp"

namespace alcedo {
namespace {

TEST(SleeveFolderRestoreTest, RestoresOrderNamesAndFlagsWithoutChangingChildReferences) {
  SleeveFolder folder(0, L"Root");
  const auto   stale  = std::make_shared<SleeveFile>(1, L"Stale");
  const auto   first  = std::make_shared<SleeveFile>(2, L"First");
  const auto   second = std::make_shared<SleeveFile>(3, L"Second");
  folder.AddElementToMap(stale);
  folder.CreateIndex({stale}, 42);
  folder.sync_flag_  = SyncFlag::SYNCED;
  first->ref_count_  = 7;
  second->ref_count_ = 9;
  first->sync_flag_  = SyncFlag::SYNCED;
  second->sync_flag_ = SyncFlag::MODIFIED;
  second->pinned_    = true;
  const std::vector<sl_element_id_t> ids{3, 2, 3};
  folder.RestoreContents(ids, [&](sl_element_id_t id) { return id == 2 ? first : second; });
  EXPECT_EQ(folder.ListElements(), (std::vector<sl_element_id_t>{3, 2}));
  EXPECT_EQ(folder.GetElementIdByName(L"First"), 2);
  EXPECT_EQ(folder.GetElementIdByName(L"Second"), 3);
  EXPECT_FALSE(folder.Contains(L"Stale"));
  EXPECT_FALSE(folder.HasFilterIndex(42));
  EXPECT_TRUE(folder.ContentAddedSinceSync().empty());
  EXPECT_TRUE(folder.ContentRemovedSinceSync().empty());
  EXPECT_TRUE(folder.ChildrenLoaded());
  EXPECT_TRUE(folder.pinned_);
  EXPECT_EQ(folder.sync_flag_, SyncFlag::SYNCED);
  EXPECT_EQ(first->ref_count_, 7);
  EXPECT_EQ(second->ref_count_, 9);
  EXPECT_EQ(first->sync_flag_, SyncFlag::SYNCED);
  EXPECT_EQ(second->sync_flag_, SyncFlag::MODIFIED);
}

TEST(SleeveFolderRestoreTest, EmptyCommittedContentClearsPendingMembershipAndFilterIndices) {
  SleeveFolder folder(0, L"Root");
  const auto   child = std::make_shared<SleeveFile>(1, L"Child");
  folder.AddElementToMap(child);
  folder.CreateIndex({child}, 42);
  const auto references = child->ref_count_;
  folder.RestoreContents({}, [](sl_element_id_t) -> std::shared_ptr<SleeveElement> {
    ADD_FAILURE() << "Empty membership must not resolve a child";
    return nullptr;
  });
  EXPECT_TRUE(folder.ListElements().empty());
  EXPECT_FALSE(folder.Contains(L"Child"));
  EXPECT_FALSE(folder.HasFilterIndex(42));
  EXPECT_TRUE(folder.ContentAddedSinceSync().empty());
  EXPECT_TRUE(folder.ContentRemovedSinceSync().empty());
  EXPECT_EQ(child->ref_count_, references);
}

TEST(SleeveFolderRestoreTest, InvalidChildrenAndNameConflictsReportRestorationErrors) {
  SleeveFolder                       folder(0, L"Root");
  const std::vector<sl_element_id_t> ids{1, 2};
  const auto                         first  = std::make_shared<SleeveFile>(1, L"Same");
  const auto                         second = std::make_shared<SleeveFile>(2, L"Same");
  EXPECT_THROW(folder.RestoreContents(ids, [](sl_element_id_t) { return nullptr; }),
               std::runtime_error);
  EXPECT_THROW(folder.RestoreContents(ids, [&](sl_element_id_t) { return second; }),
               std::runtime_error);
  first->sync_flag_ = SyncFlag::DELETED;
  EXPECT_THROW(folder.RestoreContents(ids, [&](sl_element_id_t) { return first; }),
               std::runtime_error);
  first->sync_flag_ = SyncFlag::SYNCED;
  EXPECT_THROW(
      folder.RestoreContents(ids, [&](sl_element_id_t id) { return id == 1 ? first : second; }),
      std::runtime_error);
  second->element_name_ = L"Distinct";
  folder.RestoreContents(ids, [&](sl_element_id_t id) { return id == 1 ? first : second; });
  EXPECT_EQ(folder.ListElements(), ids);
  EXPECT_EQ(folder.GetElementIdByName(L"Distinct"), 2);
}

TEST(SleeveFolderRestoreTest, LargeFoldersResolveEachInputOnceAndKeepExactMembership) {
  for (const sl_element_id_t count : {2000, 8000, 32000}) {
    SCOPED_TRACE(count);
    SleeveFolder                             folder(0, L"Root");
    std::vector<std::shared_ptr<SleeveFile>> children;
    std::vector<sl_element_id_t>             ids;
    children.reserve(count);
    ids.reserve(count);
    for (sl_element_id_t i = 0; i < count; ++i) {
      auto child        = std::make_shared<SleeveFile>(i + 1, std::format(L"Photo {}", i));
      child->ref_count_ = 17;
      child->sync_flag_ = SyncFlag::SYNCED;
      children.push_back(std::move(child));
      ids.push_back(i + 1);
    }
    sl_element_id_t resolutions = 0;
    const auto      started     = std::chrono::steady_clock::now();
    folder.RestoreContents(ids, [&](sl_element_id_t id) {
      ++resolutions;
      return children.at(id - 1);
    });
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    RecordProperty(std::format("restore_{}_microseconds", count), std::to_string(elapsed));
    std::cout << "[ MEASUREMENT ] Restored " << count << " children in " << elapsed << " us\n";
    ASSERT_EQ(folder.ListElements(), ids);
    EXPECT_EQ(resolutions, count);
    for (const auto& child : children) {
      EXPECT_EQ(folder.GetElementIdByName(child->element_name_), child->element_id_);
      EXPECT_EQ(child->ref_count_, 17);
      EXPECT_EQ(child->sync_flag_, SyncFlag::SYNCED);
    }
  }
}

}  // namespace
}  // namespace alcedo
