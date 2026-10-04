//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/thumbnail_types.hpp"
#include "image/image_buffer.hpp"
#include "type/hash_type.hpp"
#include "type/type.hpp"

namespace alcedo {

enum class ThumbnailCacheFormat : uint8_t {
  kJpeg = 0,
  kWebP = 1,
  kBmp  = 2,
};

enum class ThumbnailDiskCachePurpose : uint8_t {
  kThumbnail = 0,
  kAnalysis  = 1,
};

struct ThumbnailDiskCacheKey {
  std::string               project_uuid;
  sl_element_id_t           element_id = 0;
  ThumbnailResolution       resolution = ThumbnailResolution::k1024;
  ThumbnailDiskCachePurpose purpose    = ThumbnailDiskCachePurpose::kThumbnail;
  std::string               edit_version_hash;
  uint32_t                  cache_schema_version                                 = 1;

  bool                      operator==(const ThumbnailDiskCacheKey& other) const = default;
};

}  // namespace alcedo

template <>
struct std::hash<alcedo::ThumbnailDiskCacheKey> {
  size_t operator()(const alcedo::ThumbnailDiskCacheKey& key) const noexcept {
    size_t h = std::hash<std::string>{}(key.project_uuid);
    h ^= std::hash<uint32_t>{}(key.element_id) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(key.resolution)) + 0x9e3779b9 + (h << 6) +
         (h >> 2);
    h ^= std::hash<uint32_t>{}(static_cast<uint32_t>(key.purpose)) + 0x9e3779b9 + (h << 6) +
         (h >> 2);
    h ^= std::hash<std::string>{}(key.edit_version_hash) + 0x9e3779b9 + (h << 6) + (h >> 2);
    h ^= std::hash<uint32_t>{}(key.cache_schema_version) + 0x9e3779b9 + (h << 6) + (h >> 2);
    return h;
  }
};

namespace alcedo {

/**
 * @brief On-disk thumbnail cache of one project at a time.
 *
 * Each project has its own directory under the cache root, with its own index file
 * (cache_metadata.json) and its own LRU budget of max entries. Opening a project reads only that
 * project's index, and one project's writes never evict another project's thumbnails.
 */
class ThumbnailDiskCacheService {
 public:
  struct Stats {
    size_t      total_entries    = 0;
    size_t      total_size_bytes = 0;
    size_t      hit_count        = 0;
    size_t      miss_count       = 0;
    size_t      max_entries      = 0;
    bool        enabled          = true;
    std::string cache_root_path;
  };

  ThumbnailDiskCacheService();
  explicit ThumbnailDiskCacheService(const std::filesystem::path& cache_root);
  ~ThumbnailDiskCacheService();

  ThumbnailDiskCacheService(const ThumbnailDiskCacheService&)            = delete;
  ThumbnailDiskCacheService& operator=(const ThumbnailDiskCacheService&) = delete;
  ThumbnailDiskCacheService(ThumbnailDiskCacheService&&)                 = delete;
  ThumbnailDiskCacheService&   operator=(ThumbnailDiskCacheService&&)    = delete;

  void                         Initialize(const std::string& project_uuid);
  void                         Shutdown();

  bool                         Lookup(const ThumbnailDiskCacheKey& key);
  std::unique_ptr<ImageBuffer> Read(const ThumbnailDiskCacheKey& key);
  void                         EnqueueWrite(const ThumbnailDiskCacheKey& key, ImageBuffer buffer,
                                            ThumbnailCacheFormat format = ThumbnailCacheFormat::kJpeg);
  void  EnqueueWrite(const ThumbnailDiskCacheKey& key, std::shared_ptr<ImageBuffer> buffer,
                     ThumbnailCacheFormat format = ThumbnailCacheFormat::kJpeg);
  void  Invalidate(const std::string& project_uuid, sl_element_id_t element_id);

  Stats GetStats() const;

  // ── Phase 4: Configuration ────────────────────────────────────────────
  void  SetEnabled(bool enabled);
  bool  IsEnabled() const;
  void  SetCacheRoot(const std::filesystem::path& cache_root);
  const std::filesystem::path& GetCacheRoot() const;
  /// Entry budget of the open project.
  void                         SetMaxEntries(size_t max_entries);
  size_t                       GetMaxEntries() const;
  void                         SetJpegQuality(int quality);
  int                          GetJpegQuality() const;
  void                         SetWebPQuality(int quality);
  int                          GetWebPQuality() const;

  // ── Phase 4: Operations ───────────────────────────────────────────────
  /// Remove everything under the cache root: every project's directory and any file left there.
  void                         ClearAll();
  void                         ClearProject(const std::string& project_uuid);
  void                         FlushMetadata();

 private:
  friend class ThumbnailService;

  struct EntryMeta {
    ThumbnailDiskCacheKey key;
    size_t                file_size_bytes = 0;
    std::filesystem::path file_path;
    int64_t               last_access_time = 0;
  };

  struct WriteTask {
    ThumbnailDiskCacheKey        key;
    std::shared_ptr<ImageBuffer> buffer;
    std::string                  key_hash;
    ThumbnailCacheFormat         format                  = ThumbnailCacheFormat::kJpeg;
    uint64_t                     invalidation_generation = 0;
    uint64_t                     clear_generation        = 0;
  };

  struct State;
  std::unique_ptr<State> state_;

  static std::string     MakeKeyHashString(const ThumbnailDiskCacheKey& key);
  std::filesystem::path  DeriveFilePath(const std::string&   key_hash,
                                        ThumbnailCacheFormat format) const;
  void                   WriterThreadLoop();
  /// Encode and store one queued write. True when the index changed.
  bool                   WriteEntry(WriteTask& task);
  /// FlushMetadata() is the non-throwing wrapper around this.
  void                   FlushMetadataOrThrow();
  void                   LoadMetadata();
  void                   RecordLruAccessLocked(const std::string& key_hash);
  void                   RemoveEntryFromIndexLocked(const std::string& key_hash);
  void                   EvictLruLocked(size_t target_count);
  void                   ReopenWithCacheRoot(const std::filesystem::path& cache_root);
  void                   BumpClearGenerationLocked();
  int64_t                CurrentTimeSeconds() const;
};

}  // namespace alcedo
