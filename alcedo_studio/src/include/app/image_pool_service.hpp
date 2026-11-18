//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <vector>

#include "image/image.hpp"
#include "sleeve/storage.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "type/type.hpp"

namespace alcedo {
struct ImagePoolSyncErrorResult {
  image_id_t  image_id_;
  std::string message_{""};
};

struct ImagePoolSyncStatus {
  std::vector<image_id_t>               synced_images_{};
  std::vector<ImagePoolSyncErrorResult> failed_images_{};
};


class ImagePoolService {
 private:
  std::unique_ptr<ImagePoolManager> pool_manager_;
  // This should be injected from sleeve service in future
  std::shared_ptr<Storage>   storage_;

  std::mutex                        pool_lock_;
  /// Flush with pool_lock_ already held, including focused mutations made by Write.
  auto                              SyncWithStorageLocked() -> ImagePoolSyncStatus;

 public:
  ImagePoolService() = delete;
  ImagePoolService(std::shared_ptr<Storage> storage_service, image_id_t start_id);

  ~ImagePoolService() = default;

  template <typename TResult>
  auto Read(image_id_t image_id, std::function<TResult(std::shared_ptr<Image>)> operation)
      -> TResult {
    std::unique_lock lock(pool_lock_);
    if (!pool_manager_) {
      throw std::runtime_error("[ERROR] ImagePoolService: Pool manager is not initialized.");
    }

    // Check if the image exists in the pool
    std::shared_ptr<Image> img = pool_manager_->GetImage(image_id);
    if (!img) {
      // Check in the storage
      auto& img_ctrl = storage_->GetImageStore();
      img            = img_ctrl.GetImageById(image_id);
      if (img) {
        pool_manager_->Insert(img);
      }
    }
    if (!img) {
      throw std::runtime_error(std::format(
          "[ERROR] ImagePoolService: Image with ID {} not found in storage.", image_id));
    }

    if constexpr (std::is_void_v<TResult>) {
      operation(img);
      return;
    } else {
      return operation(img);
    }
  }

  template <typename TResult>
  auto Write(image_id_t image_id, std::function<TResult(std::shared_ptr<Image>)> operation)
      -> std::conditional_t<std::is_void_v<TResult>, ImagePoolSyncStatus,
                            std::pair<TResult, ImagePoolSyncStatus>> {
    std::unique_lock       lock(pool_lock_);
    ImagePoolSyncStatus    status;
    std::shared_ptr<Image> img;

    // Check if the image exists in the pool
    img = pool_manager_->GetImage(image_id);
    if (!img) {
      // Check in the storage
      auto result = storage_->GetImageStore().GetImageById(image_id);
      if (!result) {
        throw std::runtime_error(std::format(
            "[ERROR] ImagePoolService: Image with ID {} not found in storage.", image_id));
      }
      img = result;
      pool_manager_->Insert(img);
    }

    // Perform the operation
    img->MarkSyncState(ImageSyncState::MODIFIED);
    if constexpr (std::is_void_v<TResult>) {
      operation(img);
      status = SyncWithStorageLocked();
      return status;
    } else {
      TResult result = operation(img);
      status         = SyncWithStorageLocked();
      return {result, status};
    }
  }

  template <typename TResult>
  auto Write_NoSync(image_id_t image_id, std::function<TResult(std::shared_ptr<Image>)> operation)
      -> TResult {
    std::unique_lock       lock(pool_lock_);
    std::shared_ptr<Image> img;

    // Check if the image exists in the pool
    img = pool_manager_->GetImage(image_id);
    if (!img) {
      // Check in the storage
      auto result = storage_->GetImageStore().GetImageById(image_id);
      if (!result) {
        throw std::runtime_error(std::format(
            "[ERROR] ImagePoolService: Image with ID {} not found in storage.", image_id));
      }
      img = result;
      pool_manager_->Insert(img);
    }

    // Perform the operation
    img->MarkSyncState(ImageSyncState::MODIFIED);

    if constexpr (std::is_void_v<TResult>) {
      operation(img);
      return;
    } else {
      return operation(img);
    }
  }

  auto CreateAndReturnPinnedEmpty() -> ImagePoolManager::PinnedImageHandle;
  /// Creates an UNSYNCED image, runs @p fill on it, and returns it pinned. The pool lock is held
  /// through @p fill, so SyncWithStorage on another thread never inserts a half-filled image.
  /// @p fill must not call this service.
  auto CreateAndReturnPinned(const std::function<void(Image&)>& fill)
      -> ImagePoolManager::PinnedImageHandle;

  void Remove(image_id_t image_id);
  void RemoveBatch(std::span<const image_id_t> image_ids);
  auto SyncWithStorage() -> ImagePoolSyncStatus;
  /// Persist only new admitted imports. Holds the pool lock and propagates the batch write
  /// error without retry. Marks objects synced only after the entire insert succeeds.
  void PersistImportedImages(std::span<const image_id_t> ids);
  /// Remove provisional imports without syncing other pool entries. Throws before eviction
  /// when storage removal fails, and never retries through a substitute write path.
  void DiscardImportedImages(std::span<const image_id_t> ids);
  auto GetCurrentID() -> image_id_t;
};
};  // namespace alcedo
