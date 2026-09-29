//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

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

/// Identity of one analysis rendition request; unique within one ThumbnailService.
using AnalysisRenditionId     = std::uint64_t;

/**
 * @brief Renders thumbnails and analysis renditions of committed image states.
 *
 * Every render reads the committed pipeline graph snapshot of the image
 * (@ref PipelineMgmtService::AcquireCommittedSnapshot): uncommitted editor values never reach a
 * thumbnail, an analysis input, or the disk cache, and the disk cache key is the snapshot's
 * history label (head and chain).
 *
 * Owns its own renderers: a fixed pool of batch executors (@ref kDefaultBatchExecutorCount) and a
 * scheduler with one worker per executor. It never takes the editor's render lock and never
 * renders on the editor's executor.
 *
 * Request flow: memory cache (thumbnails only) → lookup worker (snapshot, disk cache read) →
 * batch render → RGBA8 display buffer → disk cache write → callback.
 */
class ThumbnailService {
 private:
  struct State;
  std::shared_ptr<State> state_;

  static void            HandleEvict(State& st, std::optional<ThumbnailCacheKey> evicted_key);

 public:
  /// Batch executors (and render workers) of one service unless the caller asks for another count.
  static constexpr std::size_t kDefaultBatchExecutorCount = 2;

  ThumbnailService() = delete;
  /**
   * @param batch_executor_count Number of batch executors and render workers; at least 1. Each
   *        executor creates its GPU device on first use with the pipeline service's accelerator
   *        preference at construction time.
   */
  ThumbnailService(std::shared_ptr<SleeveServiceImpl>   sleeve_service,
                   std::shared_ptr<ImagePoolService>    image_pool_service,
                   std::shared_ptr<PipelineMgmtService> pipeline_service,
                   std::shared_ptr<Storage>             storage_service      = nullptr,
                   const std::string&                   project_uuid         = {},
                   const std::filesystem::path&         thumbnail_cache_root = {},
                   std::size_t batch_executor_count = kDefaultBatchExecutorCount);
  /// Drops queued work, waits for running renders, and releases the executors.
  ~ThumbnailService();

  ThumbnailService(const ThumbnailService&)                    = delete;
  auto operator=(const ThumbnailService&) -> ThumbnailService& = delete;

  // Request a thumbnail for the given element/image pair.
  // resolution selects the desired fixed tier (256, 512, 1024, 2048).
  void GetThumbnail(sl_element_id_t id, image_id_t image_id, ThumbnailCallback callback,
                    bool pin_if_found = true, CallbackDispatcher dispatcher = nullptr,
                    ThumbnailResolution resolution = ThumbnailResolution::k1024);

  // Request a thumbnail and receive a detailed result that distinguishes cancellation from
  // render/load failures; GetThumbnail keeps the guard/null callback behavior.
  void GetThumbnailDetailed(sl_element_id_t id, image_id_t image_id,
                            ThumbnailResultCallback callback, bool pin_if_found = true,
                            CallbackDispatcher  dispatcher = nullptr,
                            ThumbnailResolution resolution = ThumbnailResolution::k1024);

  /**
   * @brief Render one analysis rendition of the committed state of @p element_id.
   *
   * Same render path as a thumbnail, but the result is delivered once to @p callback and is not
   * stored in the memory cache; the disk cache uses the analysis namespace.
   * @return Identity for @ref CancelAnalysisRendition and @ref ReleaseAnalysisRendition. It
   *         cancels only this request, never another client's request for the same image.
   */
  auto RequestAnalysisRendition(sl_element_id_t element_id, image_id_t image_id,
                                ThumbnailResolution resolution, ThumbnailResultCallback callback)
      -> AnalysisRenditionId;
  /// Cancel @p id. A request that has not delivered yet delivers kCanceled.
  void                  CancelAnalysisRendition(AnalysisRenditionId id);
  /// Forget @p id; later cancels of it do nothing. Does not cancel it.
  void                  ReleaseAnalysisRendition(AnalysisRenditionId id);

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

/**
 * @brief Source of analysis renditions for one client (image analysis or semantic generation).
 *
 * Requests are keyed by (element, resolution) within the client; cancelling or releasing a key
 * affects only this client's requests.
 */
class IAnalysisRenditionProvider {
 public:
  virtual ~IAnalysisRenditionProvider()                           = default;

  virtual void RequestRendition(sl_element_id_t element_id, image_id_t image_id,
                                ThumbnailResolution     resolution,
                                ThumbnailResultCallback callback) = 0;
  virtual void CancelRendition(const ThumbnailCacheKey& key)      = 0;
  virtual void ReleaseRendition(const ThumbnailCacheKey& key)     = 0;
};

/**
 * @brief @ref IAnalysisRenditionProvider backed by @ref ThumbnailService analysis renditions.
 *
 * Maps this client's keys to the service's request identities. Thread: any thread.
 */
class ThumbnailServiceAnalysisRenditionProvider final : public IAnalysisRenditionProvider {
 public:
  explicit ThumbnailServiceAnalysisRenditionProvider(std::shared_ptr<ThumbnailService> service);

  void RequestRendition(sl_element_id_t element_id, image_id_t image_id,
                        ThumbnailResolution resolution, ThumbnailResultCallback callback) override;
  void CancelRendition(const ThumbnailCacheKey& key) override;
  void ReleaseRendition(const ThumbnailCacheKey& key) override;

 private:
  std::shared_ptr<ThumbnailService>                               service_;
  std::mutex                                                      mutex_;
  std::unordered_multimap<ThumbnailCacheKey, AnalysisRenditionId> requests_;
};
};  // namespace alcedo
