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
#include "edit/graph/pipeline_graph_snapshot.hpp"
#include "image/image_buffer.hpp"
#include "io/image/export_color_profile_config.hpp"
#include "type/type.hpp"
#include "ui/edit_viewer/frame_sink.hpp"

namespace alcedo {
class ThumbnailDiskCacheService;
}  // namespace alcedo

namespace alcedo {

struct ThumbnailGuard {
  std::unique_ptr<ImageBuffer> thumbnail_buffer_   = nullptr;
  int                          pin_count_          = 0;
  /// Output encoding of the pixels of `thumbnail_buffer_`: the DRT node of the committed snapshot
  /// that the thumbnail shows (@ref ThumbnailEncodingOf). A disk cache hit reports the same
  /// encoding, because the disk cache key is the history of that snapshot.
  ViewerDisplayConfig          encoding{};

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

/**
 * @brief The output encoding of a thumbnail of @p snapshot.
 *
 * A thumbnail render sets no output color override, so its pixels have the encoding of the DRT
 * node of the document, as the renderer reports for each render; the default encoding when the
 * document has no DRT node.
 */
[[nodiscard]] auto ThumbnailEncodingOf(const PipelineGraphSnapshot& snapshot)
    -> ViewerDisplayConfig;

using ThumbnailCallback       = std::function<void(std::shared_ptr<ThumbnailGuard>)>;
using ThumbnailResultCallback = std::function<void(ThumbnailRequestResult)>;
using CallbackDispatcher      = std::function<void(std::function<void()>)>;

/// The document of one preview render (ThumbnailService::RenderPreview) and its output encoding.
struct ThumbnailPreviewDocument {
  /// The frozen document to render. Required.
  std::shared_ptr<const PipelineGraphSnapshot> snapshot;
  /// Output encoding of the render; nullopt renders with the DRT of the document.
  std::optional<ExportColorProfileConfig>      output_color;
};

/// Outcome of one preview render, delivered once.
struct ThumbnailPreviewResult {
  /// kReady with pixels, or kError with the real reason in @ref message.
  ThumbnailRequestStatus       status = ThumbnailRequestStatus::kError;
  std::string                  message;
  /// RGBA8 host pixels; set only when kReady. Owned by the receiver; the service keeps no copy.
  std::shared_ptr<ImageBuffer> pixels;
  /// Output encoding of @ref pixels: the override of the document, or its DRT.
  ViewerDisplayConfig          encoding{};
};

/**
 * @brief One preview render of a library photo (ThumbnailService::RenderPreview).
 *
 * The edit presets plan (PS4, step 3): the Apply Preset dialog shows each preset on a library
 * photo that the editor does not hold.
 */
struct ThumbnailPreviewRequest {
  sl_element_id_t     element_id = 0;
  image_id_t          image_id   = 0;
  /// Size tier of the render: its long edge and its RAW decode resolution.
  ThumbnailResolution resolution = ThumbnailResolution::k256;
  /**
   * Runs once on a lookup worker of the service with the committed snapshot of the photo (its
   * active Version) and returns the document to render. Throws with the real reason; the
   * request then completes with kError.
   */
  std::function<ThumbnailPreviewDocument(
      const std::shared_ptr<const PipelineGraphSnapshot>& committed)>
                                              build_document;
  /// Receives the result once, on a worker of the service. Required.
  std::function<void(ThumbnailPreviewResult)> on_complete;
};

/**
 * @brief Renders thumbnails of committed image states.
 *
 * Every render reads the committed pipeline graph snapshot of the image
 * (@ref PipelineMgmtService::AcquireCommittedSnapshot): uncommitted editor values never reach a
 * thumbnail or the disk cache, and the disk cache key is the snapshot's
 * history label (head and chain).
 *
 * Owns its own renderers: a fixed pool of batch executors (@ref kDefaultBatchExecutorCount) and a
 * scheduler with one worker per executor. It never takes the editor's render lock and never
 * renders on the editor's executor.
 *
 * Request flow: memory cache → lookup worker (snapshot, disk cache read) →
 * batch render → RGBA8 display buffer → disk cache write → callback. Each delivered guard
 * reports the output encoding of its pixels (@ref ThumbnailGuard::encoding).
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
   * @brief Render a caller document of one photo and hand the pixels to the caller.
   *
   * The render takes the lookup, decode, and render stages and the batch executors of a
   * thumbnail: the committed snapshot of @p request.element_id goes to
   * ThumbnailPreviewRequest::build_document, and the returned document renders at the tier of
   * @p request.resolution. The pixels go to @p request.on_complete only: the memory cache and the
   * disk cache of the thumbnails do not change, and the request is not one of the pending
   * thumbnail requests, so CancelPending and InvalidateThumbnail do not end it.
   *
   * Thread: any thread. @p request.on_complete runs on a worker of the service.
   * @throws std::invalid_argument when build_document or on_complete is missing;
   *         std::runtime_error when the services are not initialized.
   */
  void RenderPreview(ThumbnailPreviewRequest request);

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
