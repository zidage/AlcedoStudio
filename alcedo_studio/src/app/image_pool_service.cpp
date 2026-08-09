//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/image_pool_service.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "image/image.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "type/type.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
ImagePoolService::ImagePoolService(std::shared_ptr<Storage> storage_service,
                                           image_id_t                      start_id)
    : storage_(storage_service) {
  pool_manager_ = std::make_unique<ImagePoolManager>(start_id);
}

auto ImagePoolService::CreateAndReturnPinnedEmpty() -> ImagePoolManager::PinnedImageHandle {
  std::unique_lock       lock(pool_lock_);
  return pool_manager_->CreateAndReturnPinnedEmpty();
}

void ImagePoolService::Remove(image_id_t image_id) {
  std::unique_lock lock(pool_lock_);
  if (!pool_manager_) {
    throw std::runtime_error("[ERROR] ImagePoolService: Pool manager is not initialized.");
  }

  // Check if the image exists in the pool
  auto img = pool_manager_->GetImage(image_id);
  if (img) {
    img->MarkSyncState(ImageSyncState::DELETED);
  } else {
    // Check in the storage
    try {
      storage_->GetImageStore().RemoveImageById(image_id);
    } catch (std::exception& e) {
      throw std::runtime_error(std::format(
          "[ERROR] ImagePoolService: Failed to remove image with ID {} from storage: {}", image_id,
          e.what()));
    }
  }

}

void ImagePoolService::RemoveBatch(std::span<const image_id_t> image_ids) {
  std::unique_lock lock(pool_lock_);
  if (!pool_manager_) {
    throw std::runtime_error("[ERROR] ImagePoolService: Pool manager is not initialized.");
  }

  std::vector<image_id_t> remove_from_storage;
  remove_from_storage.reserve(image_ids.size());

  for (const auto image_id : image_ids) {
    if (image_id == 0) {
      continue;
    }
    auto img = pool_manager_->GetImage(image_id);
    if (img) {
      img->MarkSyncState(ImageSyncState::DELETED);
    } else {
      remove_from_storage.push_back(image_id);
    }
  }

  if (!remove_from_storage.empty()) {
    try {
      storage_->GetImageStore().RemoveImagesByIds(remove_from_storage);
    } catch (std::exception& e) {
      throw std::runtime_error(std::format(
          "[ERROR] ImagePoolService: Failed to remove images from storage: {}", e.what()));
    }
  }
}

auto ImagePoolService::SyncWithStorage() -> ImagePoolSyncStatus {
  std::unique_lock lock(pool_lock_);
  auto&            img_ctrl = storage_->GetImageStore();

  // Classify the pool first so each sync state can be flushed as one batched
  // transaction instead of one autocommit transaction per image.
  std::vector<std::shared_ptr<Image>>                        to_insert;
  std::vector<std::pair<image_id_t, std::shared_ptr<Image>>> to_update;
  std::vector<image_id_t>                                    to_remove;
  for (auto& [id, img] : pool_manager_->GetPool()) {
    switch (img->GetSyncState()) {
      case ImageSyncState::UNSYNCED:
        to_insert.push_back(img);
        break;
      case ImageSyncState::MODIFIED:
        to_update.emplace_back(id, img);
        break;
      case ImageSyncState::DELETED:
        to_remove.push_back(id);
        break;
      default:
        break;
    }
  }

  ImagePoolSyncStatus status;

  // Insert the whole batch in a single transaction. If the batch fails (for example
  // one row violates a constraint), fall back to per-row inserts so the individual
  // failure is isolated and the remaining valid images still persist.
  if (!to_insert.empty()) {
    try {
      img_ctrl.AddImages(to_insert);
      for (auto& img : to_insert) {
        img->MarkSyncState(ImageSyncState::SYNCED);
        status.synced_images_.push_back(img->image_id_);
      }
    } catch (const std::exception&) {
      for (auto& img : to_insert) {
        try {
          img_ctrl.AddImage(img);
          img->MarkSyncState(ImageSyncState::SYNCED);
          status.synced_images_.push_back(img->image_id_);
        } catch (const std::exception& e) {
          status.failed_images_.push_back({img->image_id_, e.what()});
        }
      }
    }
  }

  // Update the whole batch in a single transaction, with the same per-row fallback.
  if (!to_update.empty()) {
    try {
      img_ctrl.UpdateImages(to_update);
      for (auto& [id, img] : to_update) {
        img->MarkSyncState(ImageSyncState::SYNCED);
        status.synced_images_.push_back(id);
      }
    } catch (const std::exception&) {
      for (auto& [id, img] : to_update) {
        try {
          img_ctrl.UpdateImage(img);
          img->MarkSyncState(ImageSyncState::SYNCED);
          status.synced_images_.push_back(id);
        } catch (const std::exception& e) {
          status.failed_images_.push_back({id, e.what()});
        }
      }
    }
  }

  if (!to_remove.empty()) {
    try {
      img_ctrl.RemoveImagesByIds(to_remove);
      status.synced_images_.insert(status.synced_images_.end(), to_remove.begin(),
                                   to_remove.end());
    } catch (std::exception& e) {
      for (const auto id : to_remove) {
        status.failed_images_.push_back({id, e.what()});
      }
      to_remove.clear();
    }
  }

  for (auto id : to_remove) {
    pool_manager_->GetPool().erase(id);
  }
  return status;
}

auto ImagePoolService::GetCurrentID() -> image_id_t {
  std::unique_lock lock(pool_lock_);
  if (!pool_manager_) {
    throw std::runtime_error("[ERROR] ImagePoolService: Pool manager is not initialized.");
  }
  return pool_manager_->GetCurrentID();
}

};  // namespace alcedo
