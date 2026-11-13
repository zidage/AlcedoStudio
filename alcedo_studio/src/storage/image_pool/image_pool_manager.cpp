//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/image_pool/image_pool_manager.hpp"

#include <cstdint>
#include <memory>
#include <optional>

#include "type/type.hpp"

namespace alcedo {

ImagePoolManager::ImagePoolManager() = default;

ImagePoolManager::ImagePoolManager(uint32_t start_id) : id_generator_(start_id) {}

ImagePoolManager::ImagePoolManager(uint32_t capacity_thumb, uint32_t start_id)
    : id_generator_(start_id), capacity_(capacity_thumb) {}

void ImagePoolManager::PinnedImageHandle::Release() {
  if (manager_) {
    manager_->Unpin(image_id_);
    manager_ = nullptr;
  }
}



/**
 * @brief
 *
 * @return std::unordered_map<image_id_t, std::shared_ptr<Image>>
 */
auto ImagePoolManager::GetPool() -> std::unordered_map<image_id_t, std::shared_ptr<Image>>& {
  return image_pool_;
}

/**
 * @brief Insert an image into the image pool
 *
 * @param img
 */
void ImagePoolManager::Insert(const std::shared_ptr<Image> img) {
  EnsureCapacityForInsert();
  image_pool_[img->image_id_] = img;
  pin_counts_.try_emplace(img->image_id_, 0);
  lru_pool_.RecordAccess(img->image_id_, img->image_id_);
}

auto ImagePoolManager::CreateAndReturnPinnedEmpty() -> PinnedImageHandle {
  EnsureCapacityForInsert();
  auto new_id         = id_generator_.GenerateID();
  auto img            = std::make_shared<Image>(new_id);
  img->sync_state_    = ImageSyncState::UNSYNCED;
  image_pool_[new_id] = img;
  pin_counts_.try_emplace(new_id, 0);
  lru_pool_.RecordAccess(new_id, new_id);

  Pin(new_id);
  return PinnedImageHandle(this, new_id, std::move(img));
}

/**
 * @brief Check whether an image with the given id exists in the image pool
 *
 * @param id
 * @return true image exists
 * @return false image not exists
 */
auto ImagePoolManager::PoolContains(const image_id_t& id) -> bool {
  return image_pool_.contains(id);
}

auto ImagePoolManager::GetImage(const image_id_t& id) -> std::shared_ptr<Image> {
  auto it = image_pool_.find(id);
  if (it == image_pool_.end()) {
    return nullptr;
  }
  lru_pool_.RecordAccess(id, id);
  return it->second;
}

auto ImagePoolManager::GetImagePinned(const image_id_t& id) -> std::optional<PinnedImageHandle> {
  auto img = GetImage(id);
  if (!img) {
    return std::nullopt;
  }
  Pin(id);
  return PinnedImageHandle(this, id, std::move(img));
}

auto ImagePoolManager::Capacity() -> uint32_t {
  return capacity_;
}

void ImagePoolManager::ResizeCache(const uint32_t new_capacity) {
  ResizePool(new_capacity);
}

void ImagePoolManager::ResizePool(const uint32_t new_capacity) {
  capacity_ = new_capacity;
  EnsureCapacityForInsert();
}

auto ImagePoolManager::IsEvictable(image_id_t id) -> bool {
  const auto pin_it = pin_counts_.find(id);
  if (pin_it != pin_counts_.end() && pin_it->second > 0) {
    return false;
  }
  // The pool is the only owner of a pending write (a new, modified, or deleted Image that
  // SyncWithStorage has not written yet). Evicting it would drop that write.
  const auto image_it = image_pool_.find(id);
  return image_it == image_pool_.end() ||
         image_it->second->GetSyncState() == ImageSyncState::SYNCED;
}

void ImagePoolManager::EnsureCapacityForInsert() {
  if (capacity_ == 0) {
    return;
  }
  while (image_pool_.size() >= capacity_) {
    bool evicted = false;
    auto keys = lru_pool_.GetLRUKeys();
    for (auto key : keys) {
      if (IsEvictable(key)) {
        EvictByKey(key);
        evicted = true;
        break;
      }
    }
    if (!evicted) {
      break;
    }
  }
}

void ImagePoolManager::EvictByKey(image_id_t id) {
  lru_pool_.RemoveRecord(id);
  image_pool_.erase(id);
  pin_counts_.erase(id);
}

void ImagePoolManager::Pin(image_id_t id) {
  ++pin_counts_[id];
}

void ImagePoolManager::Unpin(image_id_t id) {
  auto it = pin_counts_.find(id);
  if (it == pin_counts_.end()) {
    return;
  }
  if (it->second > 0) {
    --it->second;
  }
}


/**
 * @brief Clear the whole image pool as well as the cache
 *
 */
void ImagePoolManager::Clear() {
  image_pool_.clear();
  pin_counts_.clear();
  lru_pool_.Flush();
}

void ImagePoolManager::Flush() {
  Clear();
}

};  // namespace alcedo