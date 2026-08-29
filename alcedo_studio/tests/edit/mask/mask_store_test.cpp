//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/mask_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

namespace alcedo {
namespace {

auto TestRoot(std::string_view name) -> std::filesystem::path {
  return std::filesystem::path{"build/tmp/g6_mask_store"} / name;
}

auto MakeAsset(std::string key, std::uint32_t width = 4, std::uint32_t height = 3,
               std::uint8_t value = 17) -> MaskAsset {
  MaskAsset asset;
  asset.key                         = MaskAssetKey{std::move(key)};
  asset.descriptor.extent           = {width, height};
  asset.descriptor.reference_bounds = {0.1f, 0.2f, 0.7f, 0.6f};
  asset.pixels.assign(static_cast<std::size_t>(width) * height, value);
  return asset;
}

class MaskStoreFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = TestRoot(::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  std::filesystem::path root_;
};

TEST_F(MaskStoreFixture, MaskStoreRejectsRasterMaskLargerThan4096OnEitherAxis) {
  MaskStore store(root_);
  auto      too_wide = MakeAsset("wide", 4097, 1);
  auto      too_tall = MakeAsset("tall", 1, 4097);
  EXPECT_THROW(store.Save(too_wide), std::invalid_argument);
  EXPECT_THROW(store.Save(too_tall), std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(store.PathFor(too_wide.key)));
}

TEST_F(MaskStoreFixture, MaskStoreRoundTripPreservesR8PixelsDescriptorAndKey) {
  const auto expected = MakeAsset("round_trip");
  MaskStore  store(root_);
  store.Save(expected);
  MaskStore  reopened(root_);
  const auto actual = reopened.Load(expected.key);
  EXPECT_EQ(actual->key, expected.key);
  EXPECT_EQ(actual->descriptor.extent, expected.descriptor.extent);
  EXPECT_FLOAT_EQ(actual->descriptor.reference_bounds.x, expected.descriptor.reference_bounds.x);
  EXPECT_FLOAT_EQ(actual->descriptor.reference_bounds.y, expected.descriptor.reference_bounds.y);
  EXPECT_FLOAT_EQ(actual->descriptor.reference_bounds.w, expected.descriptor.reference_bounds.w);
  EXPECT_FLOAT_EQ(actual->descriptor.reference_bounds.h, expected.descriptor.reference_bounds.h);
  EXPECT_EQ(actual->pixels, expected.pixels);
}

TEST_F(MaskStoreFixture, MaskStoreUsesConfiguredRoot) {
  MaskStore  store(root_ / "configured");
  const auto asset = MakeAsset("root");
  store.Save(asset);
  EXPECT_EQ(store.Root(), root_ / "configured");
  EXPECT_TRUE(std::filesystem::exists(store.PathFor(asset.key)));
}

TEST_F(MaskStoreFixture, MaskStoreReplacesFileOnlyAfterCompleteWrite) {
  MaskStore store(root_);
  auto      original = MakeAsset("replace", 2, 2, 31);
  store.Save(original);
  auto invalid = MakeAsset("replace", 2, 2, 99);
  invalid.pixels.pop_back();
  EXPECT_THROW(store.Save(invalid), std::invalid_argument);
  MaskStore reopened(root_);
  EXPECT_EQ(reopened.Load(original.key)->pixels, original.pixels);
}

TEST_F(MaskStoreFixture, MaskHostCacheEvictsByBytesWithoutDeletingMaskFile) {
  MaskStore  store(root_, 8);
  const auto first  = MakeAsset("first", 3, 2, 1);
  const auto second = MakeAsset("second", 3, 2, 2);
  store.Save(first);
  store.Save(second);
  EXPECT_LE(store.HostCacheBytes(), 8U);
  EXPECT_EQ(store.HostCacheEntryCount(), 1U);
  EXPECT_TRUE(std::filesystem::exists(store.PathFor(first.key)));
  MaskStore reopened(root_);
  EXPECT_EQ(reopened.Load(first.key)->pixels, first.pixels);
}

}  // namespace
}  // namespace alcedo
