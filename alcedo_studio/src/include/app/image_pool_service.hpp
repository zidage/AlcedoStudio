//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

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
      status = SyncWithStorage();
      return status;
    } else {
      TResult result = operation(img);
      status         = SyncWithStorage();
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

  void Remove(image_id_t image_id);
  void RemoveBatch(std::span<const image_id_t> image_ids);
  auto SyncWithStorage() -> ImagePoolSyncStatus;
  auto GetCurrentID() -> image_id_t;
};
};  // namespace alcedo
