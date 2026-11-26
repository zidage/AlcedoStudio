//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/node_cache.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace alcedo {

NodeCache::~NodeCache() {
  dirty_.Clear();
  clean_files_.Clear();
  for (auto& page : pages_) {
    if (!page.elements) {
      continue;
    }
    for (auto& slot : *page.elements) {
      if (slot) {
        slot->cache_ = nullptr;
      }
    }
  }
}

auto NodeCache::Find(sl_element_id_t id) -> std::shared_ptr<SleeveElement> {
  const auto page = id / kPageSlots;
  if (page >= pages_.size() || !pages_[page].elements) {
    return nullptr;
  }
  const auto& slot = (*pages_[page].elements)[id % kPageSlots];
  if (slot && ListFor(slot->type_, slot->SyncState()) == &clean_files_) {
    clean_files_.MoveToBack(*slot);
  }
  return slot;
}

auto NodeCache::Contains(sl_element_id_t id) const -> bool {
  const auto page = id / kPageSlots;
  return page < pages_.size() && pages_[page].elements &&
         (*pages_[page].elements)[id % kPageSlots] != nullptr;
}

void NodeCache::Insert(std::shared_ptr<SleeveElement> element) {
  if (!element) {
    throw std::invalid_argument("NodeCache: the element is null");
  }
  if (element->cache_ != nullptr && element->cache_ != this) {
    throw std::logic_error("NodeCache: another cache holds the element " +
                           std::to_string(element->element_id_));
  }
  const auto id         = element->element_id_;
  const auto page_index = id / kPageSlots;
  if (page_index >= pages_.size()) {
    pages_.resize(page_index + 1);
  }
  auto& page = pages_[page_index];
  if (!page.elements) {
    page.elements = std::make_unique<Page>();
    ++page_count_;
  }
  auto& slot = (*page.elements)[id % kPageSlots];
  if (slot == element) {
    return;
  }
  if (slot) {
    throw std::logic_error("NodeCache: the slot of the element " + std::to_string(id) +
                           " holds a different element");
  }

  element->cache_ = this;
  if (auto* list = ListFor(element->type_, element->SyncState())) {
    list->PushBack(*element);
  }
  if (element->type_ == ElementType::FOLDER) {
    folder_ids_.insert(id);
  }
  slot = std::move(element);
  ++page.used;
  ++size_;
}

void NodeCache::Erase(sl_element_id_t id) {
  if (!Contains(id)) {
    return;
  }
  const auto& slot = Slot(id);
  if (IsDirty(slot->SyncState()) && slot->SyncState() != SyncFlag::DELETED) {
    throw std::logic_error("NodeCache: the element " + std::to_string(id) +
                           " has a change that no sync wrote");
  }
  Release(id);
}

auto NodeCache::EvictToLimit() -> std::size_t {
  std::size_t evicted = 0;
  // Each element is visited at most once: a held element moves to the back and the walk ends
  // after the elements that were in the list at its start.
  for (auto remaining = clean_files_.Size(); clean_files_.Size() > limit_ && remaining > 0;
       --remaining) {
    auto* oldest = clean_files_.Front();
    if (Slot(oldest->element_id_).use_count() > 1) {
      clean_files_.MoveToBack(*oldest);
      continue;
    }
    Release(oldest->element_id_);
    ++evicted;
  }
  return evicted;
}

auto NodeCache::DirtyElements() const -> std::vector<std::shared_ptr<SleeveElement>> {
  std::vector<std::shared_ptr<SleeveElement>> elements;
  elements.reserve(dirty_.Size());
  for (const auto& element : dirty_) {
    elements.push_back(Slot(element.element_id_));
  }
  return elements;
}

void NodeCache::OnSyncFlagChanged(SleeveElement& element, SyncFlag old_flag) {
  auto* old_list = ListFor(element.type_, old_flag);
  auto* new_list = ListFor(element.type_, element.SyncState());
  if (old_list == new_list) {
    return;
  }
  if (old_list != nullptr) {
    old_list->Erase(element);
  }
  if (new_list != nullptr) {
    new_list->PushBack(element);
  }
}

auto NodeCache::ListFor(ElementType type, SyncFlag flag) -> ElementList* {
  if (IsDirty(flag)) {
    return &dirty_;
  }
  return type == ElementType::FILE ? &clean_files_ : nullptr;
}

auto NodeCache::Slot(sl_element_id_t id) const -> const std::shared_ptr<SleeveElement>& {
  return (*pages_[id / kPageSlots].elements)[id % kPageSlots];
}

void NodeCache::Release(sl_element_id_t id) {
  auto& page = pages_[id / kPageSlots];
  auto& slot = (*page.elements)[id % kPageSlots];
  if (auto* list = ListFor(slot->type_, slot->SyncState())) {
    list->Erase(*slot);
  }
  if (slot->type_ == ElementType::FOLDER) {
    folder_ids_.erase(id);
  }
  slot->cache_ = nullptr;
  slot.reset();
  --size_;
  if (--page.used == 0) {
    page.elements.reset();
    --page_count_;
  }
}

}  // namespace alcedo
