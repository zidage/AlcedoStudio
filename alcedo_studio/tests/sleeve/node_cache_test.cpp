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

/// The ids of @p ids that the cache holds.
auto HeldIds(const NodeCache& cache, const std::vector<sl_element_id_t>& ids)
    -> std::vector<sl_element_id_t> {
  std::vector<sl_element_id_t> held;
  for (const auto id : ids) {
    if (cache.Contains(id)) {
      held.push_back(id);
    }
  }
  return held;
}

// Sleeve Node Cache Plan SN7: the cache keeps at most the limit of clean file elements, and the
// least recently used of them leave first.
TEST(NodeCacheTest, ReadingMoreFilesThanTheLimitKeepsTheCacheAtTheLimit) {
  NodeCache cache;
  EXPECT_EQ(cache.Limit(), NodeCache::kCleanFileLimit);
  cache.SetLimit(3);
  std::vector<sl_element_id_t>              ids = {1, 2, 3, 4, 4096, 5000};
  std::vector<std::weak_ptr<SleeveElement>> observed;
  for (const auto id : ids) {
    auto element = MakeFile(id);
    observed.push_back(element);
    cache.Insert(std::move(element));
  }
  ASSERT_EQ(cache.CleanFileCount(), ids.size());
  EXPECT_EQ(cache.Size(), ids.size()) << "SetLimit evicts nothing";

  // A Find makes the element the most recently used: id 1 then outlives ids 2, 3, and 4.
  ASSERT_NE(cache.Find(1), nullptr);
  EXPECT_EQ(cache.EvictToLimit(), 3u);

  EXPECT_EQ(cache.Size(), 3u);
  EXPECT_EQ(cache.CleanFileCount(), 3u);
  EXPECT_EQ(HeldIds(cache, ids), (std::vector<sl_element_id_t>{1, 4096, 5000}));
  EXPECT_TRUE(observed[1].expired()) << "an evicted element that no caller holds is freed";
  EXPECT_TRUE(observed[2].expired());
  EXPECT_TRUE(observed[3].expired());
  EXPECT_EQ(cache.PageCount(), 2u) << "page 1 still holds ids 4096 and 5000";

  // A store load of an evicted id puts a new element into the free slot.
  cache.Insert(MakeFile(2));
  EXPECT_EQ(cache.Size(), 4u);
  EXPECT_EQ(cache.EvictToLimit(), 1u);
  EXPECT_EQ(HeldIds(cache, ids), (std::vector<sl_element_id_t>{1, 2, 5000}))
      << "id 4096 is now the least recently used";
  EXPECT_EQ(cache.EvictToLimit(), 0u) << "a cache at the limit evicts nothing";
}

TEST(NodeCacheTest, ElementHeldByACallerStaysAndKeepsItsIdentity) {
  NodeCache cache;
  cache.SetLimit(1);
  auto held = MakeFile(10);
  cache.Insert(held);
  cache.Insert(MakeFile(11));
  cache.Insert(MakeFile(12));

  // Id 10 is the least recently used, but the caller holds it: it moves to the back.
  EXPECT_EQ(cache.EvictToLimit(), 2u);
  EXPECT_EQ(HeldIds(cache, {10, 11, 12}), (std::vector<sl_element_id_t>{10}));
  EXPECT_EQ(cache.Find(10), held) << "a second Find gives the held object";

  // When the callers hold more clean files than the limit, the walk ends and evicts nothing.
  auto other = MakeFile(13);
  cache.Insert(other);
  EXPECT_EQ(cache.EvictToLimit(), 0u);
  EXPECT_EQ(cache.Size(), 2u);
  EXPECT_EQ(cache.Find(13), other);

  // After the caller lets the element go, the next eviction frees it.
  std::weak_ptr<SleeveElement> observed = held;
  held.reset();
  EXPECT_EQ(cache.EvictToLimit(), 1u);
  EXPECT_TRUE(observed.expired());
  EXPECT_EQ(HeldIds(cache, {10, 13}), (std::vector<sl_element_id_t>{13}));
}

TEST(NodeCacheTest, ModifiedElementStaysUntilSync) {
  NodeCache cache;
  cache.SetLimit(0);
  cache.Insert(MakeFile(20));
  cache.Insert(MakeFile(21, SyncFlag::UNSYNC));
  cache.Insert(MakeFile(22, SyncFlag::DELETED));
  cache.Find(20)->MarkModified();
  EXPECT_EQ(cache.CleanFileCount(), 0u);
  EXPECT_EQ(cache.DirtyCount(), 3u);

  EXPECT_EQ(cache.EvictToLimit(), 0u);
  EXPECT_EQ(DirtyIds(cache), (std::vector<sl_element_id_t>{21, 22, 20}))
      << "a dirty element stays and keeps its place in the dirty list";

  // The commit of the sync: the written files become clean and can then leave.
  cache.Find(20)->SetSyncFlag(SyncFlag::SYNCED);
  cache.Find(21)->SetSyncFlag(SyncFlag::SYNCED);
  cache.Erase(22);
  EXPECT_EQ(cache.DirtyCount(), 0u);
  EXPECT_EQ(cache.CleanFileCount(), 2u);
  EXPECT_EQ(cache.EvictToLimit(), 2u);
  EXPECT_EQ(cache.Size(), 0u);
  EXPECT_EQ(cache.PageCount(), 0u);
}

TEST(NodeCacheTest, FoldersStayResident) {
  NodeCache cache;
  cache.SetLimit(0);
  cache.Insert(MakeFolder(0));
  cache.Insert(MakeFolder(30));
  cache.Insert(MakeFile(31));
  cache.Insert(MakeFolder(32, SyncFlag::MODIFIED));
  cache.Find(32)->SetSyncFlag(SyncFlag::SYNCED);
  EXPECT_EQ(cache.CleanFileCount(), 1u) << "a clean folder is in no list";

  EXPECT_EQ(cache.EvictToLimit(), 1u);
  EXPECT_EQ(HeldIds(cache, {0, 30, 31, 32}), (std::vector<sl_element_id_t>{0, 30, 32}));
  std::vector<sl_element_id_t> visited;
  cache.ForEachFolder([&visited](SleeveFolder& folder) { visited.push_back(folder.element_id_); });
  std::sort(visited.begin(), visited.end());
  EXPECT_EQ(visited, (std::vector<sl_element_id_t>{0, 30, 32}));
}

TEST(NodeCacheTest, EraseOfACleanFileRemovesItFromTheEvictionList) {
  NodeCache cache;
  cache.SetLimit(1);
  cache.Insert(MakeFile(40));
  cache.Insert(MakeFile(41));
  cache.Erase(40);
  EXPECT_EQ(cache.CleanFileCount(), 1u);
  EXPECT_EQ(cache.EvictToLimit(), 0u);
  EXPECT_TRUE(cache.Contains(41));

  // An element that leaves the cache is detached: its flag changes reach no list.
  auto detached = MakeFile(42);
  cache.Insert(detached);
  cache.Erase(42);
  detached->MarkModified();
  EXPECT_EQ(cache.DirtyCount(), 0u);
  EXPECT_EQ(cache.CleanFileCount(), 1u);
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
