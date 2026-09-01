//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/mask/mask_store.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace alcedo {
namespace {

auto TestRoot(std::string_view name) -> std::filesystem::path {
  return std::filesystem::path{"build/tmp/mask_store"} / name;
}

auto MakePixels(std::uint32_t width, std::uint32_t height, std::uint8_t value)
    -> std::vector<std::uint8_t> {
  return std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height, value);
}

auto DefaultDescriptor(std::uint32_t width = 4, std::uint32_t height = 3) -> MaskAssetDescriptor {
  MaskAssetDescriptor descriptor;
  descriptor.extent           = {width, height};
  descriptor.reference_bounds = {0.1f, 0.2f, 0.7f, 0.6f};
  return descriptor;
}

class MaskStoreFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ = TestRoot(::testing::UnitTest::GetInstance()->current_test_info()->name());
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  void TearDown() override { SetMaskStorePublishHookForTesting(nullptr); }

  std::filesystem::path root_;
};

TEST_F(MaskStoreFixture, MaskStoreRejectsRasterMaskLargerThan4096OnEitherAxis) {
  MaskStore store(root_);
  auto      too_wide = MakePixels(4097, 1, 1);
  auto      too_tall = MakePixels(1, 4097, 1);
  EXPECT_THROW((void)store.Put({{4097, 1}, {}}, too_wide), std::invalid_argument);
  EXPECT_THROW((void)store.Put({{1, 4097}, {}}, too_tall), std::invalid_argument);
}

TEST_F(MaskStoreFixture, EqualRasterContentReturnsOneAssetKey) {
  const auto descriptor = DefaultDescriptor();
  const auto pixels     = MakePixels(4, 3, 17);
  MaskStore  store(root_);
  const auto first  = store.Put(descriptor, pixels);
  const auto second = store.Put(descriptor, pixels);
  EXPECT_EQ(first, second);
  EXPECT_EQ(first, MakeMaskAssetKey(descriptor, pixels));
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root_)) {
    if (entry.is_regular_file()) ++files;
  }
  EXPECT_EQ(files, 1U);
  MaskStore  reopened(root_);
  const auto loaded = reopened.Load(first);
  EXPECT_EQ(loaded->key, first);
  EXPECT_EQ(loaded->descriptor, descriptor);
  EXPECT_EQ(loaded->pixels, pixels);
}

TEST_F(MaskStoreFixture, DescriptorOrPixelChangeReturnsDifferentAssetKey) {
  const auto base     = DefaultDescriptor();
  const auto pixels   = MakePixels(4, 3, 17);
  const auto original = MakeMaskAssetKey(base, pixels);

  auto wider           = base;
  wider.extent.width   = 5;
  EXPECT_NE(original, MakeMaskAssetKey(wider, MakePixels(5, 3, 17)));

  auto taller          = base;
  taller.extent.height = 4;
  EXPECT_NE(original, MakeMaskAssetKey(taller, MakePixels(4, 4, 17)));

  auto bounds                   = base;
  bounds.reference_bounds.x     = 0.11f;
  EXPECT_NE(original, MakeMaskAssetKey(bounds, pixels));

  auto changed     = pixels;
  changed.front()  = 18;
  EXPECT_NE(original, MakeMaskAssetKey(base, changed));
}

TEST_F(MaskStoreFixture, ExistingAssetBytesCannotBeReplaced) {
  const auto descriptor = DefaultDescriptor();
  const auto pixels     = MakePixels(4, 3, 17);
  const auto key        = MakeMaskAssetKey(descriptor, pixels);
  std::filesystem::create_directories(root_);
  MaskStore store(root_);
  const auto destination = store.PathFor(key);
  {
    std::ofstream garbage(destination, std::ios::binary | std::ios::trunc);
    garbage << "not-a-mask-asset";
  }
  EXPECT_THROW((void)store.Put(descriptor, pixels), std::runtime_error);
  std::ifstream remaining(destination, std::ios::binary);
  std::string   contents((std::istreambuf_iterator<char>(remaining)),
                         std::istreambuf_iterator<char>());
  EXPECT_EQ(contents, "not-a-mask-asset");
}

TEST_F(MaskStoreFixture, InterruptedRasterWritePublishesNoAsset) {
  const auto descriptor = DefaultDescriptor();
  const auto pixels     = MakePixels(4, 3, 19);
  const auto key        = MakeMaskAssetKey(descriptor, pixels);
  MaskStore  store(root_);
  SetMaskStorePublishHookForTesting([](const std::filesystem::path&, const std::filesystem::path&) {
    throw std::runtime_error("injected publish failure");
  });
  EXPECT_THROW((void)store.Put(descriptor, pixels), std::runtime_error);
  EXPECT_FALSE(std::filesystem::exists(store.PathFor(key)));
  if (std::filesystem::exists(root_)) {
    for (const auto& entry : std::filesystem::directory_iterator(root_)) {
      EXPECT_EQ(entry.path().filename().string().find(".tmp."), std::string::npos);
    }
  }
  MaskStore reopened(root_);
  EXPECT_THROW((void)reopened.Load(key), std::runtime_error);
}

TEST_F(MaskStoreFixture, ConcurrentEqualRasterWritesProduceOneVerifiedAsset) {
  const auto descriptor = DefaultDescriptor();
  const auto pixels     = MakePixels(4, 3, 21);
  MaskAssetKey first_key;
  MaskAssetKey second_key;
  std::thread  writer_a([&] {
    MaskStore store(root_);
    first_key = store.Put(descriptor, pixels);
  });
  std::thread writer_b([&] {
    MaskStore store(root_);
    second_key = store.Put(descriptor, pixels);
  });
  writer_a.join();
  writer_b.join();
  EXPECT_EQ(first_key, second_key);
  EXPECT_EQ(first_key, MakeMaskAssetKey(descriptor, pixels));
  MaskStore  reopened(root_);
  const auto loaded = reopened.Load(first_key);
  EXPECT_EQ(loaded->pixels, pixels);
  EXPECT_EQ(loaded->descriptor, descriptor);
  std::size_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator(root_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".r8mask") ++files;
  }
  EXPECT_EQ(files, 1U);
}

TEST_F(MaskStoreFixture, MaskStoreUsesConfiguredRoot) {
  MaskStore  store(root_ / "configured");
  const auto descriptor = DefaultDescriptor();
  const auto pixels     = MakePixels(4, 3, 3);
  const auto key        = store.Put(descriptor, pixels);
  EXPECT_EQ(store.Root(), root_ / "configured");
  EXPECT_TRUE(std::filesystem::exists(store.PathFor(key)));
}

TEST_F(MaskStoreFixture, MaskHostCacheEvictsByBytesWithoutDeletingMaskFile) {
  MaskStore  store(root_, 8);
  const auto first_desc  = DefaultDescriptor(3, 2);
  const auto second_desc = DefaultDescriptor(3, 2);
  const auto first       = store.Put(first_desc, MakePixels(3, 2, 1));
  const auto second      = store.Put(second_desc, MakePixels(3, 2, 2));
  EXPECT_NE(first, second);
  EXPECT_LE(store.HostCacheBytes(), 8U);
  EXPECT_EQ(store.HostCacheEntryCount(), 1U);
  EXPECT_TRUE(std::filesystem::exists(store.PathFor(first)));
  MaskStore reopened(root_);
  EXPECT_EQ(reopened.Load(first)->pixels, MakePixels(3, 2, 1));
}

}  // namespace
}  // namespace alcedo
