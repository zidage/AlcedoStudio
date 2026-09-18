//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>

#include "app/image_pool_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/sleeve_service.hpp"
#include "app/thumbnail_types.hpp"
#include "image/image_buffer.hpp"
#include "type/type.hpp"

namespace alcedo {
class ThumbnailDiskCacheService;
}  // namespace alcedo

namespace alcedo {

struct ThumbnailGuard {
  std::unique_ptr<ImageBuffer> thumbnail_buffer_   = nullptr;
  int                          pin_count_          = 0;

  ThumbnailGuard()                                 = default;
  ~ThumbnailGuard()                                = default;

  // Non-copyable
  ThumbnailGuard(const ThumbnailGuard&)            = delete;
  ThumbnailGuard& operator=(const ThumbnailGuard&) = delete;

  // Movable
  ThumbnailGuard(ThumbnailGuard&&)                 = default;
  ThumbnailGuard& operator=(ThumbnailGuard&&)      = default;
};

enum class ThumbnailRequestStatus {
  kReady,
  kCanceled,
  kError,
};

struct ThumbnailRequestResult {
  std::shared_ptr<ThumbnailGuard> guard{};
  ThumbnailRequestStatus          status = ThumbnailRequestStatus::kError;
  std::string                     message{};
  ThumbnailCacheKey               key{};
};

using ThumbnailCallback       = std::function<void(std::shared_ptr<ThumbnailGuard>)>;
using ThumbnailResultCallback = std::function<void(ThumbnailRequestResult)>;
using CallbackDispatcher      = std::function<void(std::function<void()>)>;

class ThumbnailService {
 private:
  struct State;
  std::shared_ptr<State> state_;

  static void            HandleEvict(State& st, std::optional<ThumbnailCacheKey> evicted_key);

 public:
  ThumbnailService() = delete;
  ThumbnailService(std::shared_ptr<SleeveServiceImpl>      sleeve_service,
                   std::shared_ptr<ImagePoolService>       image_pool_service,
                   std::shared_ptr<PipelineMgmtService>    pipeline_service,
                   std::shared_ptr<Storage>          storage_service      = nullptr,
                   const std::string&                      project_uuid         = {},
                   const std::filesystem::path&            thumbnail_cache_root = {});
  ~ThumbnailService() = default;

  // Request a thumbnail for the given element/image pair.
  // resolution selects the desired fixed tier (256, 512, 1024, 2048).
  void GetThumbnail(sl_element_id_t id, image_id_t image_id, ThumbnailCallback callback,
                    bool pin_if_found = true, CallbackDispatcher dispatcher = nullptr,
                    ThumbnailResolution resolution = ThumbnailResolution::k1024);

  // Request a thumbnail and receive a detailed result. Rendering uses the live
  // pipeline handle (same document/executor as the editor) under the scheduler
  // render lock. Thumbnail work bypasses session GPU caches. This distinguishes
  // cancellation from render/load failures, while GetThumbnail preserves the
  // legacy guard/null callback behavior.
  void GetThumbnailDetailed(sl_element_id_t id, image_id_t image_id,
                            ThumbnailResultCallback callback, bool pin_if_found = true,
                            CallbackDispatcher  dispatcher = nullptr,
                            ThumbnailResolution resolution = ThumbnailResolution::k1024);

  // Render an analysis rendition from the live pipeline handle. Pins via
  // LoadPipeline and releases with ReleasePipelineUse (no SavePipeline). Results
  // are not stored in thumbnail_cache_. Disk cache hits/writes use a separate
  // analysis namespace and skip writes when the queued commit label is stale,
  // the live document is dirty, or an editor preview is unsettled.
  void RequestAnalysisRendition(sl_element_id_t element_id, image_id_t image_id,
                                ThumbnailResolution resolution, ThumbnailResultCallback callback);
  void CancelAnalysisRendition(const ThumbnailCacheKey& key);
  void ReleaseAnalysisRendition(const ThumbnailCacheKey& key);

  // Cancel a pending thumbnail request for one element/resolution key.
  // Also increments the key generation token so queued tasks skip execution.
  void CancelPending(const ThumbnailCacheKey& key);

  // Cancel all pending thumbnail requests for the given element at all resolutions.
  // Use this only for content-level invalidation, deletion, or full element teardown.
  void CancelPending(sl_element_id_t sleeve_element_id);

  // Force the cached thumbnail for this sleeve element to be discarded.
  // Next GetThumbnail() will re-render via pipeline.
  void InvalidateThumbnail(sl_element_id_t sleeve_element_id);

  // Release a cached/pending thumbnail for one element/resolution key.
  void ReleaseThumbnail(const ThumbnailCacheKey& key);

  // Release all resolution tiers for an element.
  // Use this only for full element teardown or legacy callers.
  void ReleaseThumbnail(sl_element_id_t sleeve_element_id);

  // Proactively resize the cache to the desired capacity.
  // Useful when zoom level changes: growing avoids eviction churn,
  // shrinking reduces wasted memory.
  void ResizeCache(uint32_t desired_capacity);

  // ── Phase 4: Disk cache configuration & operations ──────────────────
  void SetDiskCacheEnabled(bool enabled);
  bool IsDiskCacheEnabled() const;
  void SetDiskCacheRoot(const std::filesystem::path& cache_root);
  std::filesystem::path GetDiskCacheRoot() const;
  void                  SetDiskCacheMaxEntries(size_t max_entries);
  size_t                GetDiskCacheMaxEntries() const;
  void                  SetDiskCacheJpegQuality(int quality);
  int                   GetDiskCacheJpegQuality() const;
  void                  SetDiskCacheWebPQuality(int quality);
  int                   GetDiskCacheWebPQuality() const;

  void                  ClearAllDiskCache();
  void                  ClearProjectDiskCache();
  void                  FlushDiskCacheMetadata();

  struct DiskCacheStats {
    size_t      total_entries    = 0;
    size_t      total_size_bytes = 0;
    size_t      hit_count        = 0;
    size_t      miss_count       = 0;
    size_t      max_entries      = 0;
    bool        enabled          = true;
    std::string cache_root_path;
  };
  DiskCacheStats GetDiskCacheStats() const;
};
};  // namespace alcedo
