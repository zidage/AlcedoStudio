//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/node_cache.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <vector>

#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_element_factory.hpp"
#include "type/type.hpp"

namespace alcedo {
namespace {

/// A file element @p id with the flag @p flag; no cache holds it.
auto MakeFile(sl_element_id_t id, SyncFlag flag = SyncFlag::SYNCED)
    -> std::shared_ptr<SleeveElement> {
  auto element = SleeveElementFactory::CreateElement(ElementType::FILE, id, L"file");
  element->SetSyncFlag(flag);
  return element;
}

auto MakeFolder(sl_element_id_t id, SyncFlag flag = SyncFlag::SYNCED)
    -> std::shared_ptr<SleeveElement> {
  auto element = SleeveElementFactory::CreateElement(ElementType::FOLDER, id, L"folder");
  element->SetSyncFlag(flag);
  return element;
}

auto DirtyIds(const NodeCache& cache) -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> ids;
  for (const auto& element : cache.DirtyElements()) {
    ids.push_back(element->element_id_);
  }
  return ids;
}

TEST(NodeCacheTest, InsertFindEraseAcrossPageBoundaries) {
  NodeCache                                   cache;
  const std::vector<sl_element_id_t>          ids = {1, 4095, 4096, 4097, 1'000'000};
  std::vector<std::shared_ptr<SleeveElement>> elements;
  for (const auto id : ids) {
    elements.push_back(MakeFile(id));
    cache.Insert(elements.back());
  }

  EXPECT_EQ(cache.Size(), ids.size());
  // Pages 0, 1, and 244: ids 4095 and 4096 lie on the two sides of a page boundary.
  EXPECT_EQ(cache.PageCount(), 3u);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    EXPECT_EQ(cache.Find(ids[i]), elements[i]) << "id " << ids[i];
    EXPECT_TRUE(cache.Contains(ids[i]));
  }
  EXPECT_EQ(cache.Find(0), nullptr);
  EXPECT_EQ(cache.Find(4094), nullptr);
  EXPECT_EQ(cache.Find(4098), nullptr);
  EXPECT_EQ(cache.Find(999'999), nullptr);
  EXPECT_EQ(cache.Find(2'000'000), nullptr);

  for (const auto id : ids) {
    cache.Erase(id);
    EXPECT_EQ(cache.Find(id), nullptr) << "id " << id;
  }
  EXPECT_EQ(cache.Size(), 0u);
  EXPECT_EQ(cache.PageCount(), 0u);
}

TEST(NodeCacheTest, PageIsReleasedWhenItsLastElementIsErased) {
  NodeCache cache;
  cache.Insert(MakeFile(1));
  cache.Insert(MakeFile(4096));
  cache.Insert(MakeFile(8191));
  ASSERT_EQ(cache.PageCount(), 2u);

  cache.Erase(4096);
  EXPECT_EQ(cache.PageCount(), 2u) << "id 8191 still uses page 1";
  cache.Erase(8191);
  EXPECT_EQ(cache.PageCount(), 1u);
  EXPECT_EQ(cache.Find(8191), nullptr);
  EXPECT_NE(cache.Find(1), nullptr);

  cache.Insert(MakeFile(4100));
  EXPECT_EQ(cache.PageCount(), 2u) << "an insert into a released range allocates the page again";
  EXPECT_NE(cache.Find(4100), nullptr);
}

TEST(NodeCacheTest, FlagChangeMovesElementIntoAndOutOfDirtyList) {
  NodeCache cache;
  auto      created = MakeFile(10, SyncFlag::UNSYNC);
  auto      stored  = MakeFile(11, SyncFlag::SYNCED);
  cache.Insert(created);
  cache.Insert(stored);
  EXPECT_EQ(DirtyIds(cache), (std::vector<sl_element_id_t>{10}));

  // UNSYNC -> SYNCED: the commit of the insert.
  created->SetSyncFlag(SyncFlag::SYNCED);
  EXPECT_EQ(cache.DirtyCount(), 0u);

  // SYNCED -> MODIFIED, and a second MarkModified keeps one list entry.
  stored->MarkModified();
  stored->MarkModified();
  EXPECT_EQ(stored->SyncState(), SyncFlag::MODIFIED);
  EXPECT_EQ(DirtyIds(cache), (std::vector<sl_element_id_t>{11}));

  // MODIFIED -> DELETED stays in the list once.
  stored->SetSyncFlag(SyncFlag::DELETED);
  created->MarkModified();
  EXPECT_EQ(DirtyIds(cache), (std::vector<sl_element_id_t>{11, 10}))
      << "the list keeps the order in which the elements became dirty";

  // MODIFIED -> SYNCED leaves the list; the deleted element leaves with Erase.
  created->SetSyncFlag(SyncFlag::SYNCED);
  cache.Erase(11);
  EXPECT_EQ(cache.DirtyCount(), 0u);
  EXPECT_EQ(cache.Size(), 1u);

  // A ref count that drops to zero sets DELETED through SetSyncFlag.
  created->IncrementRefCount();
  created->DecrementRefCount();
  EXPECT_EQ(DirtyIds(cache), (std::vector<sl_element_id_t>{10}));
}

TEST(NodeCacheTest, EraseOfAnElementWithAnUnwrittenChangeThrowsAndKeepsIt) {
  NodeCache cache;
  auto      modified = MakeFile(5, SyncFlag::MODIFIED);
  auto      created  = MakeFile(6, SyncFlag::UNSYNC);
  cache.Insert(modified);
  cache.Insert(created);

  EXPECT_THROW(cache.Erase(5), std::logic_error);
  EXPECT_THROW(cache.Erase(6), std::logic_error);
  EXPECT_EQ(cache.Find(5), modified);
  EXPECT_EQ(cache.Find(6), created);
  EXPECT_EQ(cache.DirtyCount(), 2u);
}

TEST(NodeCacheTest, InsertOfADifferentElementWithAnOccupiedIdThrows) {
  NodeCache cache;
  auto      first = MakeFile(7);
  cache.Insert(first);
  cache.Insert(first);
  EXPECT_EQ(cache.Size(), 1u) << "inserting the held element changes nothing";

  EXPECT_THROW(cache.Insert(MakeFile(7)), std::logic_error);
  EXPECT_EQ(cache.Find(7), first);
  EXPECT_THROW(cache.Insert(nullptr), std::invalid_argument);

  NodeCache other;
  EXPECT_THROW(other.Insert(first), std::logic_error) << "an element is in one cache";
  EXPECT_EQ(other.Size(), 0u);
}

TEST(NodeCacheTest, ElementOutlivesItsCacheAndChangesItsFlagOnly) {
  std::shared_ptr<SleeveElement> kept;
  {
    NodeCache cache;
    kept = MakeFile(3, SyncFlag::MODIFIED);
    cache.Insert(kept);
    ASSERT_EQ(cache.DirtyCount(), 1u);
  }
  kept->SetSyncFlag(SyncFlag::SYNCED);
  kept->MarkModified();
  EXPECT_EQ(kept->SyncState(), SyncFlag::MODIFIED);

  NodeCache next;
  next.Insert(kept);
  EXPECT_EQ(next.DirtyCount(), 1u) << "the detached element can enter a new cache";
}

TEST(NodeCacheTest, ForEachFolderVisitsFoldersOnly) {
  NodeCache cache;
  cache.Insert(MakeFolder(0));
  cache.Insert(MakeFile(1));
  cache.Insert(MakeFolder(5000, SyncFlag::DELETED));
  cache.Insert(MakeFile(5001));
  cache.Insert(MakeFolder(9000));

  std::vector<sl_element_id_t> visited;
  cache.ForEachFolder([&visited](SleeveFolder& folder) { visited.push_back(folder.element_id_); });
  std::sort(visited.begin(), visited.end());
  EXPECT_EQ(visited, (std::vector<sl_element_id_t>{0, 5000, 9000}));

  cache.Erase(5000);
  visited.clear();
  cache.ForEachFolder([&visited](SleeveFolder& folder) { visited.push_back(folder.element_id_); });
  std::sort(visited.begin(), visited.end());
  EXPECT_EQ(visited, (std::vector<sl_element_id_t>{0, 9000}));
}

}  // namespace
}  // namespace alcedo
