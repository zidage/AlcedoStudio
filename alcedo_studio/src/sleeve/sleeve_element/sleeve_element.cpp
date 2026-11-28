//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_element/sleeve_element.hpp"

#include <chrono>

#include "sleeve/node_cache.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {

SleeveElement::SleeveElement(sl_element_id_t id, file_name_t element_name)
    : element_id_(id), element_name_(element_name), ref_count_(0), pinned_(false) {
  this->SetAddTime();
}

SleeveElement::~SleeveElement() {}

auto SleeveElement::Copy(sl_element_id_t new_id) const -> std::shared_ptr<SleeveElement> {
  // TODO: Remove placeholder
  return nullptr;
}

auto SleeveElement::Clear() -> bool {
  // Placeholder
  return true;
}

void SleeveElement::SetAddTime() {
  added_time_         = std::chrono::system_clock::to_time_t(TimeProvider::Now());
  last_modified_time_ = added_time_;
}

void SleeveElement::SetLastModifiedTime() {
  last_modified_time_ = std::chrono::system_clock::to_time_t(TimeProvider::Now());
}

void SleeveElement::IncrementRefCount() { ++ref_count_; }

void SleeveElement::DecrementRefCount() {
  --ref_count_;
  if (ref_count_ <= 0) {
    SetSyncFlag(SyncFlag::DELETED);
  }
}

auto SleeveElement::IsShared() -> bool { return ref_count_ > 1; }

void SleeveElement::SetSyncFlag(SyncFlag flag) {
  const auto old_flag = sync_flag_;
  sync_flag_          = flag;
  if (cache_ != nullptr && old_flag != flag) {
    cache_->OnSyncFlagChanged(*this, old_flag);
  }
}

void SleeveElement::MarkModified() {
  if (sync_flag_ == SyncFlag::SYNCED) {
    SetSyncFlag(SyncFlag::MODIFIED);
  }
}
};  // namespace alcedo