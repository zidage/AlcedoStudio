//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "storage/store/image/image_store.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "concurrency/thread_pool.hpp"
#include "image/image.hpp"
#include "storage/mapper/duckorm/duckdb_expr.hpp"
#include "storage/mapper/duckorm/duckdb_orm.hpp"
#include "storage/mapper/image/image_mapper.hpp"
#include "utils/queue/queue.hpp"

namespace alcedo {
/**
 * @brief Construct a new Image Controller:: Image Controller object
 *
 * @param guard
 */
ImageStore::ImageStore(ConnectionGuard&& guard)
    : guard_(std::move(guard)), image_mapper_(guard_.conn_) {}

/**
 * @brief Capture the image pool and insert the parameters into the database.
 *
 * @param image_pool
 */
void ImageStore::CaptureImagePool(std::shared_ptr<ImagePoolManager> image_pool) {
  ThreadPool                                 thread_pool{8};
  auto&                                      pool = image_pool->GetPool();
  ConcurrentBlockingQueue<ImageMapperParams> converted_params{348};
  for (auto& pool_val : pool) {
    auto img = pool_val.second;
    thread_pool.Submit(
        [img, &converted_params]() { converted_params.push_r(ImageMapper::ToParams(img)); });
  }

  auto db_lock = guard_.Lock();
  for (size_t i = 0; i < pool.size(); ++i) {
    auto result = converted_params.pop_r();
    image_mapper_.InsertParams(result);
  }
}

/**
 * @brief Add an image to the database.
 *
 * @param image
 */
void ImageStore::AddImage(std::shared_ptr<Image> image) {
  auto db_lock = guard_.Lock();
  image_mapper_.Insert(image);
}

void ImageStore::AddImages(std::span<const std::shared_ptr<Image>> images) {
  if (images.empty()) {
    return;
  }
  auto                 db_lock = guard_.Lock();
  duckorm::Transaction transaction(guard_.conn_);
  image_mapper_.InsertBatch(images);
  transaction.commit();
}

/**
 * @brief Remove an image by its ID.
 *
 * @param remove_id
 */
void ImageStore::RemoveImageById(uint32_t remove_id) {
  auto db_lock = guard_.Lock();
  image_mapper_.RemoveById(remove_id);
}

void ImageStore::RemoveImagesByIds(std::span<const image_id_t> remove_ids) {
  auto db_lock = guard_.Lock();
  image_mapper_.RemoveByIds(remove_ids);
}

void ImageStore::RemoveImagesWithoutFileBinding() {
  auto db_lock = guard_.Lock();
  image_mapper_.RemoveWhere(
      duckorm::expr::raw("NOT EXISTS (SELECT 1 FROM FileImage fi WHERE fi.image_id = Image.id)"));
}

/**
 * @brief Remove an image by its type.
 *
 * @param type
 */
void ImageStore::RemoveImageByType(ImageType type) {
  auto db_lock = guard_.Lock();
  image_mapper_.RemoveWhere(
      duckorm::expr::column_eq("type", static_cast<std::int64_t>(static_cast<uint32_t>(type))));
}

void ImageStore::UpdateImage(const std::shared_ptr<Image> image) {
  auto db_lock = guard_.Lock();
  image_mapper_.Update(image, image->image_id_);
}

void ImageStore::UpdateImages(
    std::span<const std::pair<image_id_t, std::shared_ptr<Image>>> updates) {
  if (updates.empty()) {
    return;
  }
  auto                 db_lock = guard_.Lock();
  duckorm::Transaction transaction(guard_.conn_);
  image_mapper_.UpdateBatch(updates);
  transaction.commit();
}

/**
 * @brief Get an image by its ID.
 *
 * @param id
 * @return std::shared_ptr<Image>
 */
auto ImageStore::GetImageById(image_id_t id) -> std::shared_ptr<Image> {
  auto db_lock = guard_.Lock();
  auto result  = image_mapper_.GetImageById(id);
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
auto ImageStore::GetImageByType(ImageType type) -> std::vector<std::shared_ptr<Image>> {
  auto db_lock = guard_.Lock();
  return image_mapper_.GetImageByType(type);
}

/**
 * @brief Get images by their name.
 *
 * @param name
 * @return std::vector<std::shared_ptr<Image>>
 */
auto ImageStore::GetImageByName(const std::wstring& name)
    -> std::vector<std::shared_ptr<Image>> {
  auto db_lock = guard_.Lock();
  return image_mapper_.GetImageByName(name);
}

/**
 * @brief Get images by their path.
 *
 * @param path
 * @return std::vector<std::shared_ptr<Image>>
 */
auto ImageStore::GetImageByPath(const std::filesystem::path path)
    -> std::vector<std::shared_ptr<Image>> {
  auto db_lock = guard_.Lock();
  return image_mapper_.GetImageByPath(path);
}
};  // namespace alcedo
