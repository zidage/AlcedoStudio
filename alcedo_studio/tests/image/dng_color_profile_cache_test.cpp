// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
// Additional permission under GPLv3 section 7 applies; see the LICENSE file.
#include "image/dng_color_profile_cache.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "image/dng_color_profile.hpp"
#include "image/image.hpp"
#include "image/metadata_extractor.hpp"

namespace alcedo {
namespace {

/// Source files whose text content is the profile name, so equal content gives equal fingerprints.
class DngColorProfileCacheTest : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("alcedo-dng-profile-cache-" +
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }

  auto WriteSource(const std::string& name, const std::string& content) -> std::filesystem::path {
    const auto    path = dir_ / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    return path;
  }

  /// Loader: profile named after the file content; "fail" makes the read throw.
  static auto ReadNamedProfile(const std::filesystem::path& path) -> DngColorProfilePtr {
    std::ifstream     in(path, std::ios::binary);
    const std::string content((std::istreambuf_iterator<char>(in)), {});
    if (content == "fail") {
      throw std::runtime_error("profile tag is invalid");
    }
    DngColorProfile profile;
    profile.name = content;
    return MakeDngColorProfile(std::move(profile));
  }

  std::filesystem::path dir_;
};

TEST_F(DngColorProfileCacheTest, EvictsLeastRecentlyUsedFileWhenTheHundredAndFirstFileLoads) {
  DngColorProfileCache cache(&ReadNamedProfile);
  ASSERT_EQ(DngColorProfileCache::kDefaultCapacity, 100u);
  std::vector<std::filesystem::path> sources;
  for (int i = 0; i <= 100; ++i) {
    sources.push_back(
        WriteSource("file" + std::to_string(i) + ".dng", "profile" + std::to_string(i)));
  }
  for (int i = 0; i < 100; ++i) {
    ASSERT_NE(cache.Load(sources[static_cast<std::size_t>(i)]), nullptr);
  }
  EXPECT_EQ(cache.Size(), 100u);
  EXPECT_EQ(cache.LoaderCallCount(), 100u);

  // A hit makes file 0 the most recent entry, so file 1 is the least recently used.
  EXPECT_EQ(cache.Load(sources[0])->name, "profile0");
  EXPECT_EQ(cache.LoaderCallCount(), 100u);

  EXPECT_EQ(cache.Load(sources[100])->name, "profile100");
  EXPECT_EQ(cache.Size(), 100u);
  EXPECT_EQ(cache.LoaderCallCount(), 101u);

  EXPECT_EQ(cache.Load(sources[0])->name, "profile0");
  EXPECT_EQ(cache.LoaderCallCount(), 101u);
  EXPECT_EQ(cache.Load(sources[1])->name, "profile1");
  EXPECT_EQ(cache.LoaderCallCount(), 102u);
  EXPECT_EQ(cache.Size(), 100u);
}

TEST_F(DngColorProfileCacheTest, FilesWithEqualProfileContentShareOneProfile) {
  DngColorProfileCache cache(&ReadNamedProfile);
  const auto           first  = cache.Load(WriteSource("a.dng", "Adobe Standard"));
  const auto           second = cache.Load(WriteSource("b.dng", "Adobe Standard"));
  const auto           other  = cache.Load(WriteSource("c.dng", "Camera Neutral"));
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);
  EXPECT_NE(first, other);
  EXPECT_NE(first->fingerprint, other->fingerprint);
  EXPECT_EQ(cache.Size(), 3u);
  EXPECT_EQ(cache.LoaderCallCount(), 3u);
}

TEST_F(DngColorProfileCacheTest, ChangedSourceFileIsReadAgainAndGivesTheNewProfile) {
  DngColorProfileCache cache(&ReadNamedProfile);
  const auto           source = WriteSource("edited.dng", "Adobe Standard");
  const auto           before = cache.Load(source);
  ASSERT_EQ(cache.Load(source), before);
  EXPECT_EQ(cache.LoaderCallCount(), 1u);

  const auto written = std::filesystem::last_write_time(source);
  WriteSource("edited.dng", "Adobe Standard v2");
  std::filesystem::last_write_time(source, written + std::chrono::seconds(10));
  const auto after = cache.Load(source);
  EXPECT_EQ(cache.LoaderCallCount(), 2u);
  EXPECT_EQ(after->name, "Adobe Standard v2");
  EXPECT_NE(after->fingerprint, before->fingerprint);
  EXPECT_EQ(cache.Size(), 1u);

  // Only the write time changes: the entry is stale and the file is read again.
  std::filesystem::last_write_time(source, written + std::chrono::seconds(20));
  EXPECT_EQ(cache.Load(source), after);
  EXPECT_EQ(cache.LoaderCallCount(), 3u);
}

TEST_F(DngColorProfileCacheTest, MissingFileOrFailedReadThrowsAndLeavesNoEntry) {
  DngColorProfileCache cache(&ReadNamedProfile);
  EXPECT_THROW((void)cache.Load(dir_ / "missing.dng"), std::runtime_error);
  EXPECT_EQ(cache.LoaderCallCount(), 0u);

  const auto broken = WriteSource("broken.dng", "fail");
  EXPECT_THROW((void)cache.Load(broken), std::runtime_error);
  EXPECT_THROW((void)cache.Load(broken), std::runtime_error);
  EXPECT_EQ(cache.LoaderCallCount(), 2u);
  EXPECT_EQ(cache.Size(), 0u);
}

TEST_F(DngColorProfileCacheTest, FileWithoutDngProfileIsCachedAsNoProfile) {
  DngColorProfileCache cache([](const std::filesystem::path&) { return DngColorProfilePtr{}; });
  const auto           source = WriteSource("plain.nef", "raw");
  EXPECT_EQ(cache.Load(source), nullptr);
  EXPECT_EQ(cache.Load(source), nullptr);
  EXPECT_EQ(cache.LoaderCallCount(), 1u);
}

TEST(DngColorProfileSharedCache, RealDngLoadsTheProfileFingerprintBoundAtImport) {
  const auto path = std::filesystem::path(TEST_IMG_PATH) /
                    "ci_rawfiles/tag @ryanbreitkreutz - free raws from @signatureeditsco - "
                    "DSC06683.dng";
  if (!std::filesystem::exists(path)) GTEST_SKIP() << "CI DNG fixture missing";
  Image image(1, path, ImageType::DNG);
  MetadataExtractor::ExtractEXIF_ToImage(path, image);
  const auto& imported = image.GetRawColorContext().dng_profile_;
  ASSERT_TRUE(imported.IsBound());

  auto&      cache  = DngColorProfileCache::Shared();
  const auto calls  = cache.LoaderCallCount();
  const auto loaded = cache.Load(path);
  ASSERT_NE(loaded, nullptr);
  EXPECT_EQ(loaded->fingerprint, imported->fingerprint);
  EXPECT_EQ(cache.Load(path), loaded);
  EXPECT_EQ(cache.LoaderCallCount(), calls + 1);
}

}  // namespace
}  // namespace alcedo
