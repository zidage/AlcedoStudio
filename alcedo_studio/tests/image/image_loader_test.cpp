//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#ifdef _WIN32
#include "../leak_detector/memory_leak_detector.hpp"
#endif
#include "decoders/decoder_scheduler.hpp"
#include "image/image.hpp"
#include "io/image/image_loader.hpp"
#include "type/type.hpp"


#include <cstddef>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

TEST(ImageLoaderTest, BatchLoad) {
  using namespace alcedo;
  // MemoryLeakDetector leakDetector;
  ImageLoader image_loader(128, 8, 0);
  image_path_t path = std::string(TEST_IMG_PATH) + std::string("/raw/batch");
  std::vector<image_path_t> imgs;
  for (const auto &img : std::filesystem::directory_iterator(path)) {
    imgs.push_back(img.path());
  }

  image_loader.StartLoading(imgs, DecodeType::SLEEVE_LOADING);
  size_t total_size = imgs.size();
  while (total_size > 0) {
    std::shared_ptr<Image> img = image_loader.LoadImage();
    // std::wcout << *img << std::endl;
    total_size--;
  }
}