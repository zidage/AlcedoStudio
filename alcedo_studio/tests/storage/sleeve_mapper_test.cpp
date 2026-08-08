//  Copyright 2025 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.


#include <gtest/gtest.h>

#include <exception>
#include <exiv2/error.hpp>
#include <filesystem>
#include <stdexcept>

#include "sleeve/sleeve_manager.hpp"
#include "storage/store/database.hpp"
#include "storage/store/image/image_store.hpp"
#include "storage/mapper/duckorm/duckdb_types.hpp"
#include "type/type.hpp"

using namespace alcedo;

std::filesystem::path db_path(
TEST_DB_PATH);

std::filesystem::path raw_path(
    std::string(TEST_IMG_PATH) + std::string("/raw/batch"));
TEST(SleeveMapperTest, DISABLED_InitTest1) {
  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
  {
    try {
      Database db_ctr{db_path};
      db_ctr.InitializeDB();
    } catch (std::exception& e) {
      std::cout << e.what() << std::endl;
    }
  }
  // std::filesystem::remove(db_path);
}

TEST(SleeveMapperTest, SimpleCaptureTest1) {
  if (std::filesystem::exists(db_path)) {
    std::filesystem::remove(db_path);
  }
  {
    try {
      Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);

      // Database db_ctr{db_path};
      // db_ctr.InitializeDB();
      // ImageStore img_ctr{db_ctr.GetConnectionGuard()};

      SleeveManager   manager{db_path};
      std::vector<image_path_t> imgs;
      for (const auto& img : std::filesystem::directory_iterator(raw_path)) {
        imgs.push_back(img.path());
      }
      manager.LoadToPath(imgs, L"");

      // img_ctr.CaptureImagePool(manager.GetPool());
    } catch (std::exception& e) {
      std::cout << e.what() << std::endl;
      FAIL();
    }
  }
}

TEST(SleeveMapperTest, DISABLED_SimpleCaptureTest2) {}