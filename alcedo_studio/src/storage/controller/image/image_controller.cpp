//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/controller/image/image_controller.hpp"

#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <vector>

#include "concurrency/thread_pool.hpp"
#include "image/image.hpp"
#include "storage/service/image/image_service.hpp"
#include "utils/queue/queue.hpp"
#include "utils/string/convert.hpp"

namespace alcedo {
/**
 * @brief Construct a new Image Controller:: Image Controller object
 *
 * @param guard
 */
ImageController::ImageController(ConnectionGuard&& guard)
    : guard_(std::move(guard)), service_(guard_.conn_) {}

/**
 * @brief Capture the image pool and insert the parameters into the database.
 *
 * @param image_pool
 */
void ImageController::CaptureImagePool(std::shared_ptr<ImagePoolManager> image_pool) {
  ThreadPool                                 thread_pool{8};
  auto&                                      pool = image_pool->GetPool();
  ConcurrentBlockingQueue<ImageMapperParams> converted_params{348};
  for (auto& pool_val : pool) {
    auto img = pool_val.second;
    thread_pool.Submit(
        [img, &converted_params]() { converted_params.push_r(ImageService::ToParams(img)); });
  }

  auto db_lock = guard_.Lock();
  for (size_t i = 0; i < pool.size(); ++i) {
    auto result = converted_params.pop_r();
    service_.InsertParams(result);
  }
}

/**
 * @brief Add an image to the database.
 *
 * @param image
 */
void ImageController::AddImage(std::shared_ptr<Image> image) {
  auto db_lock = guard_.Lock();
  service_.Insert(image);
}

void ImageController::AddImages(std::span<const std::shared_ptr<Image>> images) {
  auto db_lock = guard_.Lock();
  service_.InsertBatch(images);
}

/**
 * @brief Remove an image by its ID.
 *
 * @param remove_id
 */
void ImageController::RemoveImageById(uint32_t remove_id) {
  auto db_lock = guard_.Lock();
  service_.RemoveById(remove_id);
}

void ImageController::RemoveImagesByIds(std::span<const image_id_t> remove_ids) {
  auto db_lock = guard_.Lock();
  service_.RemoveByIds(remove_ids);
}

/**
 * @brief Remove an image by its type.
 *
 * @param type
 */
void ImageController::RemoveImageByType(ImageType type) {
  auto db_lock = guard_.Lock();
  service_.RemoveByClause(std::format("type={}", static_cast<uint32_t>(type)));
}

/**
 * @brief Remove an image by its path.
 *
 * @param path
 */
void ImageController::RemoveImageByPath(const std::wstring& path) {
  auto db_lock = guard_.Lock();
  service_.RemoveByClause(std::format("image_path={}", conv::ToBytes(path)));
}

void ImageController::UpdateImage(const std::shared_ptr<Image> image) {
  auto db_lock = guard_.Lock();
  service_.Update(image, image->image_id_);
}

void ImageController::UpdateImages(
    std::span<const std::pair<image_id_t, std::shared_ptr<Image>>> updates) {
  auto db_lock = guard_.Lock();
  service_.UpdateBatch(updates);
}

/**
 * @brief Get an image by its ID.
 *
 * @param id
 * @return std::shared_ptr<Image>
 */
auto ImageController::GetImageById(image_id_t id) -> std::shared_ptr<Image> {
  auto db_lock = guard_.Lock();
  auto result  = service_.GetImageById(id);
  // Assume the id is unique
  if (result.empty()) {
    return nullptr;
  }
  auto img = result[0];
  img->MarkSyncState(ImageSyncState::SYNCED);
  return img;
}

/**
 * @brief Get images by their type.
 *
 * @param type
 * @return std::vector<std::shared_ptr<Image>>
 */
auto ImageController::GetImageByType(ImageType type) -> std::vector<std::shared_ptr<Image>> {
  auto db_lock = guard_.Lock();
  return service_.GetImageByType(type);
}

/**
 * @brief Get images by their name.
 *
 * @param name
 * @return std::vector<std::shared_ptr<Image>>
 */
auto ImageController::GetImageByName(const std::wstring& name)
    -> std::vector<std::shared_ptr<Image>> {
  auto db_lock = guard_.Lock();
  return service_.GetImageByName(name);
}

/**
 * @brief Get images by their path.
 *
 * @param path
 * @return std::vector<std::shared_ptr<Image>>
 */
auto ImageController::GetImageByPath(const std::filesystem::path path)
    -> std::vector<std::shared_ptr<Image>> {
  auto db_lock = guard_.Lock();
  return service_.GetImageByPath(path);
}
};  // namespace alcedo
