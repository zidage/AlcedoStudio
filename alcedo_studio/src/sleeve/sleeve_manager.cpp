//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "sleeve/sleeve_manager.hpp"

#include <cassert>
#include <codecvt>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>

#include "io/image/image_loader.hpp"
#include "sleeve/sleeve_base.hpp"
#include "sleeve/sleeve_element/sleeve_element.hpp"
#include "sleeve/sleeve_element/sleeve_file.hpp"
#include "sleeve/sleeve_filesystem.hpp"
#include "sleeve/sleeve_view.hpp"
#include "storage/image_pool/image_pool_manager.hpp"
#include "type/type.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo {
/**
 * @brief Ad-hoc constructor for temporary sleeve, e.g. folder preview
 *
 */
SleeveManager::SleeveManager(std::filesystem::path db_path) : storage_(db_path) {
  // Update the application clock
  TimeProvider::Refresh();
  fs_ = std::make_shared<FileSystem>(db_path, storage_, 0);
  fs_->InitRoot();
  image_pool_ = std::make_shared<ImagePoolManager>(128, 4);
}

/**
 * @brief Return a shared pointer to the sleeve file system interface
 *
 * @return std::shared_ptr<FileSystem>
 */
auto SleeveManager::GetFilesystem() -> std::shared_ptr<FileSystem> { return fs_; }

/**
 * @brief Return a shared pointer to a sleeve view instance
 *
 * @return std::shared_ptr<SleeveView>
 */
auto SleeveManager::GetView() -> std::shared_ptr<SleeveView> { return view_; }

auto SleeveManager::GetPool() -> std::shared_ptr<ImagePoolManager> { return image_pool_; }

auto SleeveManager::GetImgCount() -> uint32_t { return image_pool_->Capacity(); }

/**
 * @brief Load a batch of images to the destination path
 * Update: This function is already moved to ImportService
 * FIXME: some tests are still relying on this function
 * 
 * @param img_os_paths
 * @param dest
 * @return uint32_t
 */
auto SleeveManager::LoadToPath(std::vector<image_path_t> img_os_paths, sl_path_t dest) -> uint32_t {
  ImageLoader loader{16, 20, 0};
  auto        expected_size = img_os_paths.size();
  auto        total_size    = 0;
  loader.StartLoading(std::move(img_os_paths), DecodeType::SLEEVE_LOADING);
  while (expected_size > 0) {
    std::shared_ptr<Image> loaded  = loader.LoadImage();
    auto                   element = fs_->Create(dest, loaded->image_name_, ElementType::FILE);
    std::static_pointer_cast<SleeveFile>(element)->SetImage(loaded);
    image_pool_->Insert(loaded);
    total_size++;
    --expected_size;
  }
  // FIXME: The following two function is used in the filter tests
  // So we can't remove them yet
  fs_->SyncToDB();
  storage_.GetImageStore().CaptureImagePool(image_pool_);
  return total_size;
}

};  // namespace alcedo
