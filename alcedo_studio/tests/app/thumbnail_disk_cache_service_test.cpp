//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/thumbnail_disk_cache_service.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <vector>

namespace alcedo {
namespace {

class ThumbnailDiskCacheServiceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = std::filesystem::temp_directory_path() / "alcedo_test_cache";
    std::filesystem::create_directories(temp_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_, ec);
  }

  static cv::Mat CreateTestImage(int width, int height, uint8_t r, uint8_t g, uint8_t b) {
    cv::Mat mat(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        mat.at<cv::Vec3b>(y, x) = cv::Vec3b(b, g, r);
      }
    }
    return mat;
  }

  static cv::Mat CreateFloatTestImage(int width, int height) {
    cv::Mat mat(height, width, CV_32FC4);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        mat.at<cv::Vec4f>(y, x) = cv::Vec4f(0.5f, 0.3f, 0.1f, 1.0f);
      }
    }
    return mat;
  }

  static ThumbnailDiskCacheKey MakeTestKey(const std::string& project_uuid,
                                           sl_element_id_t     element_id,
                                           ThumbnailResolution res,
                                           const std::string&  version_hash) {
    ThumbnailDiskCacheKey key;
    key.project_uuid        = project_uuid;
    key.element_id          = element_id;
    key.resolution          = res;
    key.edit_version_hash   = version_hash;
    key.cache_schema_version = 1;
    return key;
  }

  static bool WaitForEntryCount(ThumbnailDiskCacheService& service,
                                size_t                     expected_count,
                                std::chrono::milliseconds  timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (service.GetStats().total_entries == expected_count) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return service.GetStats().total_entries == expected_count;
  }

  static bool WaitForMetadataFile(const std::filesystem::path& metadata_path,
                                  std::chrono::milliseconds    timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      if (std::filesystem::exists(metadata_path)) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return std::filesystem::exists(metadata_path);
  }

  std::filesystem::path temp_dir_;
};

TEST_F(ThumbnailDiskCacheServiceTest, ConstructAndInitialize) {
  ThumbnailDiskCacheService service(temp_dir_);
  EXPECT_NO_THROW(service.Initialize("test-project-uuid"));

  auto stats = service.GetStats();
  EXPECT_EQ(stats.total_entries, 0);
  EXPECT_EQ(stats.total_size_bytes, 0);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, WriteAndRead) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("test-project-uuid");

  cv::Mat original = CreateTestImage(64, 64, 255, 128, 64);
  cv::Mat orig_copy = original.clone();
  ImageBuffer buffer(std::move(original));

  auto key = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k256,
                         "abc123def456");

  EXPECT_FALSE(service.Lookup(key));

  service.EnqueueWrite(key, std::move(buffer));

  EXPECT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  service.Shutdown();

  // Reinitialize and verify persistence
  ThumbnailDiskCacheService service2(temp_dir_);
  service2.Initialize("test-project-uuid");

  EXPECT_TRUE(service2.Lookup(key));

  auto read_buffer = service2.Read(key);
  ASSERT_NE(read_buffer, nullptr);
  EXPECT_TRUE(read_buffer->cpu_data_valid_);

  const auto& decoded = read_buffer->GetCPUData();
  ASSERT_EQ(decoded.rows, 64);
  ASSERT_EQ(decoded.cols, 64);
  ASSERT_EQ(decoded.type(), CV_8UC3);

  double psnr = cv::PSNR(orig_copy, decoded);
  EXPECT_GT(psnr, 30.0);

  service2.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, Rgba8InputKeepsChannelOrder) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("rgba8-project");

  cv::Mat original(64, 64, CV_8UC4, cv::Scalar(255, 16, 8, 255));
  cv::Mat expected_bgr(64, 64, CV_8UC3, cv::Scalar(8, 16, 255));
  ImageBuffer buffer(std::move(original));

  auto key = MakeTestKey("rgba8-project", 1, ThumbnailResolution::k256, "rgba8_hash");
  service.EnqueueWrite(key, std::move(buffer));

  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  auto read_buffer = service.Read(key);
  ASSERT_NE(read_buffer, nullptr);
  ASSERT_TRUE(read_buffer->cpu_data_valid_);

  const auto& decoded = read_buffer->GetCPUData();
  ASSERT_EQ(decoded.rows, expected_bgr.rows);
  ASSERT_EQ(decoded.cols, expected_bgr.cols);
  ASSERT_EQ(decoded.type(), CV_8UC3);

  double psnr = cv::PSNR(expected_bgr, decoded);
  EXPECT_GT(psnr, 30.0);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, LookupMissReturnsFalse) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("test-project-uuid");

  auto key = MakeTestKey("test-project-uuid", 999, ThumbnailResolution::k1024,
                         "nonexistent-hash");
  EXPECT_FALSE(service.Lookup(key));

  auto stats = service.GetStats();
  EXPECT_EQ(stats.miss_count, 1);
  EXPECT_EQ(stats.hit_count, 0);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, ReadNonexistentReturnsNull) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("test-project-uuid");

  auto key = MakeTestKey("test-project-uuid", 999, ThumbnailResolution::k1024,
                         "nonexistent-hash");
  auto result = service.Read(key);
  EXPECT_EQ(result, nullptr);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, InvalidateRemovesEntries) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("test-project-uuid");

  auto key1 = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k256,
                          "hash_v1");
  auto key2 = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k512,
                          "hash_v1");
  auto key3 = MakeTestKey("test-project-uuid", 2, ThumbnailResolution::k256,
                          "hash_v1");

  {
    cv::Mat     mat = CreateTestImage(32, 32, 100, 200, 50);
    ImageBuffer buf1(mat.clone());
    service.EnqueueWrite(key1, std::move(buf1));
    ImageBuffer buf2(mat.clone());
    service.EnqueueWrite(key2, std::move(buf2));
    ImageBuffer buf3(mat.clone());
    service.EnqueueWrite(key3, std::move(buf3));
  }

  ASSERT_TRUE(WaitForEntryCount(service, 3, std::chrono::seconds(2)));

  // Invalidate element 1 — should remove key1 and key2, but not key3
  service.Invalidate("test-project-uuid", 1);

  EXPECT_FALSE(service.Lookup(key1));
  EXPECT_FALSE(service.Lookup(key2));
  EXPECT_TRUE(service.Lookup(key3));

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, DifferentResolutionsAreSeparateEntries) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("test-project-uuid");

  auto key256  = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k256,  "hash_v1");
  auto key512  = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k512,  "hash_v1");
  auto key1024 = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k1024, "hash_v1");

  {
    cv::Mat     mat = CreateTestImage(32, 32, 255, 0, 0);
    ImageBuffer buf1(mat.clone());
    service.EnqueueWrite(key256, std::move(buf1));
    ImageBuffer buf2(mat.clone());
    service.EnqueueWrite(key512, std::move(buf2));
    ImageBuffer buf3(mat.clone());
    service.EnqueueWrite(key1024, std::move(buf3));
  }

  ASSERT_TRUE(WaitForEntryCount(service, 3, std::chrono::seconds(2)));
  EXPECT_TRUE(service.Lookup(key256));
  EXPECT_TRUE(service.Lookup(key512));
  EXPECT_TRUE(service.Lookup(key1024));

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, DifferentVersionHashProducesCacheMiss) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("test-project-uuid");

  auto key_v1 = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k256,
                            "version_hash_aaa");
  auto key_v2 = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k256,
                            "version_hash_bbb");

  {
    cv::Mat     mat = CreateTestImage(32, 32, 0, 255, 0);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key_v1, std::move(buf));
  }

  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  // key_v1 should be found, key_v2 should miss (different hash)
  EXPECT_TRUE(service.Lookup(key_v1));
  EXPECT_FALSE(service.Lookup(key_v2));

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, DifferentProjectUUIDIsolation) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("project-a");

  auto key_a = MakeTestKey("project-a", 1, ThumbnailResolution::k256, "hash_v1");

  {
    cv::Mat     mat = CreateTestImage(32, 32, 255, 255, 0);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key_a, std::move(buf));
  }

  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  service.Shutdown();

  // Reinitialize with different project — should not see project-a's entries
  service.Initialize("project-b");
  EXPECT_FALSE(service.Lookup(key_a));

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, MetadataPersistenceAcrossRestarts) {
  auto key1 = MakeTestKey("persist-project", 1, ThumbnailResolution::k256,
                          "hash_abc");
  auto key2 = MakeTestKey("persist-project", 2, ThumbnailResolution::k1024,
                          "hash_def");

  {
    ThumbnailDiskCacheService service(temp_dir_);
    service.Initialize("persist-project");

    {
      cv::Mat     mat1 = CreateTestImage(64, 64, 255, 0, 0);
      ImageBuffer buf1(mat1);
      service.EnqueueWrite(key1, std::move(buf1));
    }

    {
      cv::Mat     mat2 = CreateTestImage(128, 128, 0, 255, 0);
      ImageBuffer buf2(mat2);
      service.EnqueueWrite(key2, std::move(buf2));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    service.Shutdown();
  }

  // Restart
  ThumbnailDiskCacheService service2(temp_dir_);
  service2.Initialize("persist-project");

  auto stats = service2.GetStats();
  EXPECT_EQ(stats.total_entries, 2);
  EXPECT_GT(stats.total_size_bytes, 0);

  EXPECT_TRUE(service2.Lookup(key1));
  EXPECT_TRUE(service2.Lookup(key2));

  auto read1 = service2.Read(key1);
  ASSERT_NE(read1, nullptr);
  EXPECT_TRUE(read1->cpu_data_valid_);

  auto read2 = service2.Read(key2);
  ASSERT_NE(read2, nullptr);
  EXPECT_TRUE(read2->cpu_data_valid_);

  // Different resolutions produce different sized images
  const auto& decoded1 = read1->GetCPUData();
  const auto& decoded2 = read2->GetCPUData();
  EXPECT_EQ(decoded1.rows, 64);
  EXPECT_EQ(decoded2.rows, 128);

  service2.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, StatsTrackingAccuracy) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("stats-project");

  EXPECT_EQ(service.GetStats().total_entries, 0);
  EXPECT_EQ(service.GetStats().hit_count, 0);
  EXPECT_EQ(service.GetStats().miss_count, 0);

  auto key = MakeTestKey("stats-project", 1, ThumbnailResolution::k256, "hash");

  {
    cv::Mat     mat = CreateTestImage(32, 32, 128, 128, 128);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }

  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  // Lookup hits
  EXPECT_TRUE(service.Lookup(key));
  EXPECT_TRUE(service.Lookup(key));
  EXPECT_TRUE(service.Lookup(key));

  // Lookup miss
  auto missing_key = MakeTestKey("stats-project", 999, ThumbnailResolution::k256, "hash");
  EXPECT_FALSE(service.Lookup(missing_key));

  auto stats = service.GetStats();
  EXPECT_EQ(stats.total_entries, 1);
  EXPECT_EQ(stats.hit_count, 3);
  EXPECT_EQ(stats.miss_count, 1);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, EmptyImageNotCached) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("test-project-uuid");

  ImageBuffer empty_buffer;
  EXPECT_FALSE(empty_buffer.cpu_data_valid_);

  auto key = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k256,
                         "hash");
  service.EnqueueWrite(key, std::move(empty_buffer));

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  EXPECT_FALSE(service.Lookup(key));
  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, MultipleWriteAndRead) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("multi-project");

  const int kNumEntries = 20;
  std::vector<ThumbnailDiskCacheKey> keys;
  keys.reserve(kNumEntries);

  for (int i = 0; i < kNumEntries; ++i) {
    auto key = MakeTestKey("multi-project", static_cast<sl_element_id_t>(i),
                           ThumbnailResolution::k256,
                           "hash_" + std::to_string(i));
    keys.push_back(key);

    cv::Mat     mat = CreateTestImage(16, 16, static_cast<uint8_t>(i * 10),
                                      static_cast<uint8_t>(255 - i * 10), 128);
    ImageBuffer buffer(mat);
    service.EnqueueWrite(key, std::move(buffer));
  }

  ASSERT_TRUE(WaitForEntryCount(service, kNumEntries, std::chrono::seconds(4)));

  auto stats = service.GetStats();
  EXPECT_EQ(stats.total_entries, kNumEntries);

  for (int i = 0; i < kNumEntries; ++i) {
    EXPECT_TRUE(service.Lookup(keys[i])) << "Entry " << i << " not found";
    auto read = service.Read(keys[i]);
    ASSERT_NE(read, nullptr) << "Read failed for entry " << i;
    EXPECT_TRUE(read->cpu_data_valid_);
  }

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, ShutdownWithoutInitializeIsSafe) {
  ThumbnailDiskCacheService service(temp_dir_);
  EXPECT_NO_THROW(service.Shutdown());
}

TEST_F(ThumbnailDiskCacheServiceTest, DoubleInitializeIsSafe) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("project");
  EXPECT_NO_THROW(service.Initialize("project"));
  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, LargeImageEncodeDecode) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("large-project");

  cv::Mat orig_mat = CreateTestImage(512, 512, 200, 100, 50);
  cv::Mat orig_copy = orig_mat.clone();
  ImageBuffer buffer(std::move(orig_mat));

  auto key = MakeTestKey("large-project", 1, ThumbnailResolution::k1024,
                         "large_hash");
  service.EnqueueWrite(key, std::move(buffer));
  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(3)));

  auto read = service.Read(key);
  ASSERT_NE(read, nullptr);
  EXPECT_TRUE(read->cpu_data_valid_);

  const auto& decoded = read->GetCPUData();
  EXPECT_EQ(decoded.rows, 512);
  EXPECT_EQ(decoded.cols, 512);

  double psnr = cv::PSNR(orig_copy, decoded);
  EXPECT_GT(psnr, 30.0);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, CacheDirectoryStructure) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("dir-structure-project");

  auto key = MakeTestKey("dir-structure-project", 1, ThumbnailResolution::k256,
                         "hash123");

  {
    cv::Mat     mat = CreateTestImage(16, 16, 255, 255, 255);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }

  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  service.Shutdown();

  // Verify directory structure exists
  auto project_dir = temp_dir_ / "dir-structure-project";
  EXPECT_TRUE(std::filesystem::exists(project_dir));

  // Metadata file should exist
  auto metadata_file = project_dir / "cache_metadata.json";
  EXPECT_TRUE(std::filesystem::exists(metadata_file));

  // There should be sharded directory with a cache file
  bool found_cache_file = false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(project_dir)) {
    const auto& ext = entry.path().extension();
    if (ext == ".jpg" || ext == ".bmp" || ext == ".webp") {
      found_cache_file = true;
      break;
    }
  }
  EXPECT_TRUE(found_cache_file);
}

TEST_F(ThumbnailDiskCacheServiceTest, ShutdownDrainsPendingWrites) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("drain-project");

  const int kNumEntries = 10;
  std::vector<ThumbnailDiskCacheKey> keys;
  keys.reserve(kNumEntries);

  for (int i = 0; i < kNumEntries; ++i) {
    auto key = MakeTestKey("drain-project", static_cast<sl_element_id_t>(i),
                           ThumbnailResolution::k256,
                           "drain_hash_" + std::to_string(i));
    keys.push_back(key);

    cv::Mat     mat = CreateTestImage(16, 16, static_cast<uint8_t>(i * 20),
                                      static_cast<uint8_t>(200 - i * 15), 64);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }

  // Shutdown immediately without sleep — writer must drain all pending tasks.
  service.Shutdown();

  // Reinitialize and verify all entries persisted.
  ThumbnailDiskCacheService service2(temp_dir_);
  service2.Initialize("drain-project");

  auto stats = service2.GetStats();
  EXPECT_EQ(stats.total_entries, kNumEntries);

  for (int i = 0; i < kNumEntries; ++i) {
    EXPECT_TRUE(service2.Lookup(keys[i])) << "Entry " << i << " not persisted after drain";
    auto read = service2.Read(keys[i]);
    ASSERT_NE(read, nullptr) << "Read failed for entry " << i;
    EXPECT_TRUE(read->cpu_data_valid_);
  }

  service2.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, InvalidateSuppressesPendingWrites) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("invalidate-pending-project");

  const int kNumEntries = 10;
  std::vector<ThumbnailDiskCacheKey> keys;
  keys.reserve(kNumEntries);

  for (int i = 0; i < kNumEntries; ++i) {
    auto key = MakeTestKey("invalidate-pending-project", 42, ThumbnailResolution::k256,
                           "pending_hash_" + std::to_string(i));
    keys.push_back(key);

    cv::Mat     mat = CreateTestImage(64, 64, static_cast<uint8_t>(20 + i),
                                      static_cast<uint8_t>(80 + i), 160);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }

  service.Invalidate("invalidate-pending-project", 42);
  service.Shutdown();

  ThumbnailDiskCacheService service2(temp_dir_);
  service2.Initialize("invalidate-pending-project");
  EXPECT_EQ(service2.GetStats().total_entries, 0);
  for (const auto& key : keys) {
    EXPECT_FALSE(service2.Lookup(key));
  }
  service2.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, WebPFormatWriteAndRead) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("webp-project");

  cv::Mat original = CreateTestImage(64, 64, 128, 64, 255);
  cv::Mat orig_copy = original.clone();
  ImageBuffer buffer(std::move(original));

  auto key = MakeTestKey("webp-project", 1, ThumbnailResolution::k512, "webp_hash");
  service.EnqueueWrite(key, std::move(buffer), ThumbnailCacheFormat::kWebP);

  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  EXPECT_TRUE(service.Lookup(key));
  auto read = service.Read(key);
  ASSERT_NE(read, nullptr);
  EXPECT_TRUE(read->cpu_data_valid_);

  const auto& decoded = read->GetCPUData();
  EXPECT_EQ(decoded.rows, 64);
  EXPECT_EQ(decoded.cols, 64);

  double psnr = cv::PSNR(orig_copy, decoded);
  EXPECT_GT(psnr, 30.0);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, FloatImageEncodeDecode) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("float-project");

  cv::Mat float_mat = CreateFloatTestImage(64, 64);
  ImageBuffer buffer(std::move(float_mat));

  auto key = MakeTestKey("float-project", 1, ThumbnailResolution::k256,
                         "float_hash");
  service.EnqueueWrite(key, std::move(buffer));

  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  EXPECT_TRUE(service.Lookup(key));
  auto read = service.Read(key);
  ASSERT_NE(read, nullptr);
  EXPECT_TRUE(read->cpu_data_valid_);

  const auto& decoded = read->GetCPUData();
  EXPECT_EQ(decoded.rows, 64);
  EXPECT_EQ(decoded.cols, 64);
  // Decoded result from JPEG is always CV_8UC3
  EXPECT_EQ(decoded.type(), CV_8UC3);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, DefaultFormatProducesCacheFile) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("default-format-project");

  auto key = MakeTestKey("default-format-project", 1, ThumbnailResolution::k256,
                         "default_format_hash");

  {
    cv::Mat     mat = CreateTestImage(32, 32, 255, 0, 0);
    ImageBuffer buf(mat);
    // Default format parameter (kJpeg) with fallback to kBmp if encoder unavailable.
    service.EnqueueWrite(key, std::move(buf));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  service.Shutdown();

  // Verify a cache file was created (format depends on available encoders).
  auto project_dir = temp_dir_ / "default-format-project";
  bool found_cache_file = false;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(project_dir)) {
    const auto& ext = entry.path().extension();
    if (ext == ".jpg" || ext == ".bmp" || ext == ".webp") {
      found_cache_file = true;
      break;
    }
  }
  EXPECT_TRUE(found_cache_file);

  // Verify the entry is readable.
  ThumbnailDiskCacheService service2(temp_dir_);
  service2.Initialize("default-format-project");
  EXPECT_TRUE(service2.Lookup(key));
  auto read = service2.Read(key);
  ASSERT_NE(read, nullptr);
  EXPECT_TRUE(read->cpu_data_valid_);
  service2.Shutdown();
}

// ── Phase 4: Configuration tests ────────────────────────────────────────

TEST_F(ThumbnailDiskCacheServiceTest, EnableDisableFlag) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("test-project-uuid");

  EXPECT_TRUE(service.IsEnabled());

  service.SetEnabled(false);
  EXPECT_FALSE(service.IsEnabled());

  auto key = MakeTestKey("test-project-uuid", 1, ThumbnailResolution::k256, "hash");
  {
    cv::Mat     mat = CreateTestImage(32, 32, 255, 0, 0);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(200));

  // When disabled, writes should be dropped and lookup should miss.
  EXPECT_FALSE(service.Lookup(key));
  EXPECT_EQ(service.GetStats().total_entries, 0);

  service.SetEnabled(true);
  EXPECT_TRUE(service.IsEnabled());

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, MaxEntriesEviction) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("evict-project");

  const size_t kMax = 5;
  service.SetMaxEntries(kMax);
  EXPECT_EQ(service.GetMaxEntries(), kMax);

  // Write more entries than the max.
  for (int i = 0; i < 10; ++i) {
    auto key = MakeTestKey("evict-project", static_cast<sl_element_id_t>(i),
                           ThumbnailResolution::k256, "hash_" + std::to_string(i));
    cv::Mat     mat = CreateTestImage(16, 16, static_cast<uint8_t>(i * 25), 100, 200);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(500));

  auto stats = service.GetStats();
  EXPECT_LE(stats.total_entries, kMax);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, JpegQualityConfig) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("quality-project");

  service.SetJpegQuality(50);
  EXPECT_EQ(service.GetJpegQuality(), 50);

  // Clamping
  service.SetJpegQuality(0);
  EXPECT_EQ(service.GetJpegQuality(), 1);

  service.SetJpegQuality(200);
  EXPECT_EQ(service.GetJpegQuality(), 100);

  service.SetWebPQuality(60);
  EXPECT_EQ(service.GetWebPQuality(), 60);

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, ClearAllRemovesEntries) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("clear-all-project");

  auto key = MakeTestKey("clear-all-project", 1, ThumbnailResolution::k256, "hash");
  {
    cv::Mat     mat = CreateTestImage(32, 32, 100, 200, 50);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }

  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  service.ClearAll();
  EXPECT_EQ(service.GetStats().total_entries, 0);
  EXPECT_FALSE(service.Lookup(key));

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, ClearProjectRemovesOnlyProjectEntries) {
  ThumbnailDiskCacheService service(temp_dir_);

  // Write entries for project-a
  service.Initialize("project-a");
  auto key_a = MakeTestKey("project-a", 1, ThumbnailResolution::k256, "hash_a");
  {
    cv::Mat     mat = CreateTestImage(32, 32, 255, 0, 0);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key_a, std::move(buf));
  }
  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));
  service.Shutdown();

  // Write entries for project-b while reusing the same cache root.
  service.Initialize("project-b");
  auto key_b = MakeTestKey("project-b", 1, ThumbnailResolution::k256, "hash_b");
  {
    cv::Mat     mat = CreateTestImage(32, 32, 0, 255, 0);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key_b, std::move(buf));
  }
  // The open project's index holds only its own entries.
  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  // Clear project-b only.
  service.ClearProject("project-b");
  EXPECT_FALSE(service.Lookup(key_b));

  service.Shutdown();

  // Reinit with project-a — its entries should still exist.
  service.Initialize("project-a");
  EXPECT_TRUE(service.Lookup(key_a));
  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, WritesNoSharedIndexFile) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("own-index-project");

  auto key = MakeTestKey("own-index-project", 1, ThumbnailResolution::k256, "hash");
  {
    cv::Mat     mat = CreateTestImage(32, 32, 128, 128, 128);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }
  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));
  service.Shutdown();

  EXPECT_TRUE(std::filesystem::exists(temp_dir_ / "own-index-project" / "cache_metadata.json"));
  EXPECT_FALSE(std::filesystem::exists(temp_dir_ / "cache_global.json"));

  service.Initialize("own-index-project");
  EXPECT_TRUE(service.Lookup(key));
  service.Shutdown();
}

// Earlier versions kept every project's entries in cache_global.json. Opening a project reads only
// that project's own index, so the old file has no effect.
TEST_F(ThumbnailDiskCacheServiceTest, OpenIgnoresLegacySharedIndex) {
  auto key = MakeTestKey("legacy-project", 1, ThumbnailResolution::k256, "hash");
  {
    ThumbnailDiskCacheService service(temp_dir_);
    service.Initialize("legacy-project");
    cv::Mat     mat = CreateTestImage(32, 32, 200, 100, 50);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
    ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));
    service.Shutdown();
  }

  // A legacy index that lists one more entry of this project, backed by a real file.
  const auto extra_file = temp_dir_ / "legacy-project" / "ff" / "ff" / "extra.jpg";
  std::filesystem::create_directories(extra_file.parent_path());
  {
    std::ofstream file(extra_file, std::ios::binary);
    file << "x";
  }
  {
    std::ofstream file(temp_dir_ / "cache_global.json", std::ios::trunc);
    file << R"({"cache_schema_version":1,"entries":[{"key_hash":"ffff","project_uuid":")"
         << "legacy-project" << R"(","element_id":2,"file_path":")"
         << extra_file.generic_string() << R"(","file_size_bytes":1}]})";
  }

  ThumbnailDiskCacheService reopened(temp_dir_);
  reopened.Initialize("legacy-project");
  EXPECT_EQ(reopened.GetStats().total_entries, 1u);
  EXPECT_TRUE(reopened.Lookup(key));
  reopened.Shutdown();
}

// Each project has its own entry budget: filling one project never evicts another's entries.
TEST_F(ThumbnailDiskCacheServiceTest, ProjectBudgetsAreIndependent) {
  constexpr size_t                   kMax = 3;
  std::vector<ThumbnailDiskCacheKey> keys_a;
  ThumbnailDiskCacheService          service(temp_dir_);

  service.Initialize("budget-a");
  service.SetMaxEntries(kMax);
  for (int i = 0; i < static_cast<int>(kMax); ++i) {
    keys_a.push_back(MakeTestKey("budget-a", static_cast<sl_element_id_t>(i),
                                 ThumbnailResolution::k256, "a_" + std::to_string(i)));
    cv::Mat     mat = CreateTestImage(16, 16, static_cast<uint8_t>(i * 40), 10, 10);
    ImageBuffer buf(mat);
    service.EnqueueWrite(keys_a.back(), std::move(buf));
  }
  ASSERT_TRUE(WaitForEntryCount(service, kMax, std::chrono::seconds(3)));
  service.Shutdown();

  service.Initialize("budget-b");
  service.SetMaxEntries(kMax);
  for (int i = 0; i < static_cast<int>(2 * kMax); ++i) {
    auto        key = MakeTestKey("budget-b", static_cast<sl_element_id_t>(i),
                                  ThumbnailResolution::k256, "b_" + std::to_string(i));
    cv::Mat     mat = CreateTestImage(16, 16, 10, static_cast<uint8_t>(i * 40), 10);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_LE(service.GetStats().total_entries, kMax);
  service.Shutdown();

  service.Initialize("budget-a");
  EXPECT_EQ(service.GetStats().total_entries, kMax);
  for (const auto& key : keys_a) {
    EXPECT_TRUE(service.Lookup(key));
  }
  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, ClearAllRemovesOtherProjectsAndLegacyFiles) {
  const auto other_file = temp_dir_ / "other-project" / "ab" / "cd" / "entry.jpg";
  std::filesystem::create_directories(other_file.parent_path());
  {
    std::ofstream file(other_file, std::ios::binary);
    file << "x";
  }
  {
    std::ofstream file(temp_dir_ / "cache_global.json", std::ios::trunc);
    file << R"({"entries":[]})";
  }

  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("clearing-project");
  service.ClearAll();

  EXPECT_FALSE(std::filesystem::exists(temp_dir_ / "other-project"));
  EXPECT_FALSE(std::filesystem::exists(temp_dir_ / "cache_global.json"));
  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, LookupUpdatesAccessTime) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("access-project");

  auto key = MakeTestKey("access-project", 1, ThumbnailResolution::k256, "access_hash");
  {
    cv::Mat     mat = CreateTestImage(32, 32, 255, 255, 255);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }
  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));

  EXPECT_TRUE(service.Lookup(key));

  // Lookup again to bump access time (LRU order should change).
  for (int i = 0; i < 5; ++i) {
    auto key_i = MakeTestKey("access-project", static_cast<sl_element_id_t>(100 + i),
                             ThumbnailResolution::k256, "extra_hash_" + std::to_string(i));
    cv::Mat     mat = CreateTestImage(16, 16, static_cast<uint8_t>(i * 40), 100, 100);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key_i, std::move(buf));
  }
  ASSERT_TRUE(WaitForEntryCount(service, 6, std::chrono::seconds(3)));

  // Access the first key again after the extra writes so it is truly the
  // most recently used entry before eviction.
  EXPECT_TRUE(service.Lookup(key));

  // Set max entries to 3 — the LRU eviction should keep the most recently accessed.
  service.SetMaxEntries(3);

  // The first key should still be present since we looked it up recently.
  EXPECT_TRUE(service.Lookup(key));

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, StatsReflectsConfig) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("stats-config-project");

  service.SetMaxEntries(500);
  service.SetEnabled(true);

  auto stats = service.GetStats();
  EXPECT_EQ(stats.max_entries, 500);
  EXPECT_TRUE(stats.enabled);
  EXPECT_FALSE(stats.cache_root_path.empty());

  service.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, SetCacheRootWhileInitializedReopensOnNewRoot) {
  auto root_a = temp_dir_ / "root-a";
  auto root_b = temp_dir_ / "root-b";

  ThumbnailDiskCacheService service(root_a);
  service.Initialize("move-root-project");

  auto key_a = MakeTestKey("move-root-project", 1, ThumbnailResolution::k256, "hash_a");
  {
    cv::Mat     mat = CreateTestImage(32, 32, 255, 0, 0);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key_a, std::move(buf));
  }
  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));
  EXPECT_TRUE(service.Lookup(key_a));

  service.SetCacheRoot(root_b);
  EXPECT_EQ(service.GetCacheRoot(), root_b);
  EXPECT_FALSE(service.Lookup(key_a));
  EXPECT_EQ(service.GetStats().total_entries, 0);

  auto key_b = MakeTestKey("move-root-project", 2, ThumbnailResolution::k256, "hash_b");
  {
    cv::Mat     mat = CreateTestImage(32, 32, 0, 255, 0);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key_b, std::move(buf));
  }
  ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));
  EXPECT_TRUE(service.Lookup(key_b));
  ASSERT_TRUE(WaitForMetadataFile(root_b / "move-root-project" / "cache_metadata.json",
                                  std::chrono::seconds(3)));

  service.Shutdown();

  ThumbnailDiskCacheService reopened(root_b);
  reopened.Initialize("move-root-project");
  EXPECT_TRUE(reopened.Lookup(key_b));
  EXPECT_FALSE(reopened.Lookup(key_a));
  reopened.Shutdown();
}

TEST_F(ThumbnailDiskCacheServiceTest, ClearAllSuppressesPendingWrites) {
  ThumbnailDiskCacheService service(temp_dir_);
  service.Initialize("clear-pending-project");

  constexpr int kNumEntries = 24;
  std::vector<ThumbnailDiskCacheKey> keys;
  keys.reserve(kNumEntries);
  for (int i = 0; i < kNumEntries; ++i) {
    auto key = MakeTestKey("clear-pending-project", static_cast<sl_element_id_t>(i),
                           ThumbnailResolution::k1024,
                           "pending_clear_hash_" + std::to_string(i));
    keys.push_back(key);

    cv::Mat     mat = CreateTestImage(512, 512, static_cast<uint8_t>(i * 7), 100, 200);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
  }

  service.ClearAll();
  service.Shutdown();

  ThumbnailDiskCacheService reopened(temp_dir_);
  reopened.Initialize("clear-pending-project");
  EXPECT_EQ(reopened.GetStats().total_entries, 0);
  for (const auto& key : keys) {
    EXPECT_FALSE(reopened.Lookup(key));
  }
  reopened.Shutdown();
}

// Cache files are named by key hash only. Without a readable index none of them can be found
// again, so the project's cache starts empty and its unreachable files are removed.
TEST_F(ThumbnailDiskCacheServiceTest, UnreadableProjectIndexStartsEmptyAndRemovesItsFiles) {
  auto key = MakeTestKey("corrupt-index-project", 1, ThumbnailResolution::k256, "hash");

  {
    ThumbnailDiskCacheService service(temp_dir_);
    service.Initialize("corrupt-index-project");
    cv::Mat     mat = CreateTestImage(32, 32, 128, 64, 32);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
    ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));
    service.Shutdown();
  }

  {
    std::ofstream file(temp_dir_ / "corrupt-index-project" / "cache_metadata.json",
                       std::ios::trunc);
    file << "corrupted{not valid json";
  }

  ThumbnailDiskCacheService reopened(temp_dir_);
  reopened.Initialize("corrupt-index-project");
  EXPECT_EQ(reopened.GetStats().total_entries, 0u);
  EXPECT_FALSE(reopened.Lookup(key));
  size_t files_left = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(temp_dir_ / "corrupt-index-project")) {
    files_left += entry.is_regular_file() ? 1 : 0;
  }
  EXPECT_EQ(files_left, 0u);

  cv::Mat     mat = CreateTestImage(32, 32, 10, 20, 30);
  ImageBuffer buf(mat);
  reopened.EnqueueWrite(key, std::move(buf));
  ASSERT_TRUE(WaitForEntryCount(reopened, 1, std::chrono::seconds(2)));
  reopened.Shutdown();
}

// A non-ASCII cache root, such as %LOCALAPPDATA% under a Chinese user name, used to crash the
// app on systems whose ANSI code page is GBK: the index stored path::string() bytes, which
// nlohmann::json refuses to dump, and the exception escaped the writer thread. The index must
// hold UTF-8 whatever the code page is. U+011F has no GBK mapping at all.
TEST_F(ThumbnailDiskCacheServiceTest, NonAsciiCacheRootKeepsUtf8IndexAcrossRestarts) {
  const std::u8string   root_name = u8"缩略图_ğ";
  const auto            root      = temp_dir_ / std::filesystem::path(root_name);
  const std::string     root_name_utf8(root_name.begin(), root_name.end());
  auto key = MakeTestKey("non-ascii-project", 1, ThumbnailResolution::k256, "hash");

  {
    ThumbnailDiskCacheService service(root);
    service.Initialize("non-ascii-project");
    cv::Mat     mat = CreateTestImage(32, 32, 1, 2, 3);
    ImageBuffer buf(mat);
    service.EnqueueWrite(key, std::move(buf));
    ASSERT_TRUE(WaitForEntryCount(service, 1, std::chrono::seconds(2)));
    const auto cache_root_path = service.GetStats().cache_root_path;
    EXPECT_NE(cache_root_path.find(root_name_utf8), std::string::npos);
    service.Shutdown();
  }

  const auto metadata_path = root / "non-ascii-project" / "cache_metadata.json";
  ASSERT_TRUE(std::filesystem::exists(metadata_path));
  std::string metadata;
  {
    std::ifstream file(metadata_path, std::ios::binary);
    metadata.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }
  EXPECT_NE(metadata.find(root_name_utf8), std::string::npos);

  ThumbnailDiskCacheService reopened(root);
  reopened.Initialize("non-ascii-project");
  EXPECT_EQ(reopened.GetStats().total_entries, 1u);
  EXPECT_TRUE(reopened.Lookup(key));
  auto read = reopened.Read(key);
  ASSERT_NE(read, nullptr);
  EXPECT_EQ(read->GetCPUData().rows, 32);
  reopened.Shutdown();
}

}  // namespace
}  // namespace alcedo
