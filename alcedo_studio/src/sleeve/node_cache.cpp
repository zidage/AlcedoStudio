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

auto NodeCache::Find(sl_element_id_t id) const -> std::shared_ptr<SleeveElement> {
  const auto page = id / kPageSlots;
  if (page >= pages_.size() || !pages_[page].elements) {
    return nullptr;
  }
  return (*pages_[page].elements)[id % kPageSlots];
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
  if (IsDirty(*element)) {
    dirty_.PushBack(*element);
  }
  if (element->type_ == ElementType::FOLDER) {
    folder_ids_.insert(id);
  }
  slot = std::move(element);
  ++page.used;
  ++size_;
}

void NodeCache::Erase(sl_element_id_t id) {
  const auto page_index = id / kPageSlots;
  if (page_index >= pages_.size() || !pages_[page_index].elements) {
    return;
  }
  auto& page = pages_[page_index];
  auto& slot = (*page.elements)[id % kPageSlots];
  if (!slot) {
    return;
  }
  if (IsDirty(*slot) && slot->SyncState() != SyncFlag::DELETED) {
    throw std::logic_error("NodeCache: the element " + std::to_string(id) +
                           " has a change that no sync wrote");
  }

  if (slot->dirty_hook_.IsLinked()) {
    dirty_.Erase(*slot);
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

auto NodeCache::DirtyElements() const -> std::vector<std::shared_ptr<SleeveElement>> {
  std::vector<std::shared_ptr<SleeveElement>> elements;
  elements.reserve(dirty_.Size());
  for (const auto& element : dirty_) {
    elements.push_back(Slot(element.element_id_));
  }
  return elements;
}

void NodeCache::OnSyncFlagChanged(SleeveElement& element) {
  const bool dirty  = IsDirty(element);
  const bool listed = element.dirty_hook_.IsLinked();
  if (dirty && !listed) {
    dirty_.PushBack(element);
  } else if (!dirty && listed) {
    dirty_.Erase(element);
  }
}

auto NodeCache::Slot(sl_element_id_t id) const -> const std::shared_ptr<SleeveElement>& {
  return (*pages_[id / kPageSlots].elements)[id % kPageSlots];
}

auto NodeCache::IsDirty(const SleeveElement& element) -> bool {
  return element.SyncState() != SyncFlag::SYNCED;
}

}  // namespace alcedo
