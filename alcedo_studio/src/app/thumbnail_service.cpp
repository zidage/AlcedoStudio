//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/thumbnail_service.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <opencv2/imgproc.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "app/batch_executor_pool.hpp"
#include "app/pipeline_service.hpp"
#include "app/thumbnail_disk_cache_service.hpp"
#include "concurrency/thread_pool.hpp"
#include "renderer/pipeline_scheduler.hpp"
#include "renderer/pipeline_task.hpp"

namespace alcedo {
namespace {

// Map ThumbnailResolution to the max_edge value for SetRenderRes.
constexpr uint32_t ResolutionToMaxEdge(ThumbnailResolution res) {
  return static_cast<uint32_t>(res);
}

// Map ThumbnailResolution to the appropriate DecodeRes for RAW decoding.
constexpr DecodeRes ResolutionToDecodeRes(ThumbnailResolution res) {
  switch (res) {
    case ThumbnailResolution::k256:
      return DecodeRes::EIGHTH;
    case ThumbnailResolution::k512:
      return DecodeRes::QUARTER;
    case ThumbnailResolution::k1024:
      return DecodeRes::QUARTER;
    case ThumbnailResolution::k2048:
      return DecodeRes::HALF;
  }
  return DecodeRes::QUARTER;
}

/// Disk cache schema. 3: entries are keyed by the committed snapshot's head and chain.
constexpr uint32_t kDiskCacheSchemaVersion = 3;

void DispatchThumbnailResultCallback(const ThumbnailResultCallback& callback,
                                     const CallbackDispatcher&      dispatcher,
                                     ThumbnailRequestResult         result) {
  if (!callback) {
    return;
  }

  try {
    if (dispatcher) {
      dispatcher([callback, result = std::move(result)]() mutable { callback(std::move(result)); });
    } else {
      callback(std::move(result));
    }
  } catch (...) {
  }
}

auto ConvertThumbnailMatToRgba8(const cv::Mat& src) -> cv::Mat {
  if (src.empty()) {
    return {};
  }

  const int channels = src.channels();
  if (channels != 1 && channels != 3 && channels != 4) {
    return {};
  }

  cv::Mat src8;
  if (src.depth() == CV_8U) {
    src8 = src;
  } else if (src.depth() == CV_32F) {
    src.convertTo(src8, CV_MAKETYPE(CV_8U, channels), 255.0);
  } else {
    src.convertTo(src8, CV_MAKETYPE(CV_8U, channels));
  }

  cv::Mat rgba8;
  switch (channels) {
    case 1:
      cv::cvtColor(src8, rgba8, cv::COLOR_GRAY2RGBA);
      break;
    case 3:
      cv::cvtColor(src8, rgba8, src.depth() == CV_32F ? cv::COLOR_RGB2RGBA : cv::COLOR_BGR2RGBA);
      break;
    case 4:
      rgba8 = src8;
      break;
    default:
      return {};
  }

  return rgba8.isContinuous() ? rgba8 : rgba8.clone();
}

auto MakeDisplayCacheThumbnailBuffer(ImageBuffer& source) -> std::unique_ptr<ImageBuffer> {
  if (!source.cpu_data_valid_) {
    if (!source.gpu_data_valid_) {
      return nullptr;
    }
    source.SyncToCPU();
  }

  cv::Mat rgba8 = ConvertThumbnailMatToRgba8(source.GetCPUData());
  if (rgba8.empty() || rgba8.type() != CV_8UC4) {
    return nullptr;
  }
  return std::make_unique<ImageBuffer>(std::move(rgba8));
}

/// Disk cache label of a committed snapshot: its head (or "root" before the first commit) and
/// its transaction chain, which folds the root identity and every first-parent commit.
auto CommittedHistoryLabel(const PipelineGraphSnapshot& snapshot) -> std::string {
  const auto& head = snapshot.Head();
  return (head.has_value() ? head->ToString() : std::string{"root"}) + ":" +
         snapshot.Chain().ToString();
}

constexpr ThumbnailResolution kAllThumbnailResolutions[] = {
    ThumbnailResolution::k256,
    ThumbnailResolution::k512,
    ThumbnailResolution::k1024,
    ThumbnailResolution::k2048,
};

/**
 * @brief One thumbnail or analysis request between lookup and delivery.
 *
 * Exactly one of @ref DeliverPixels and @ref Fail takes effect; later calls do nothing.
 */
struct RenditionRequest {
  sl_element_id_t           element_id = 0;
  image_id_t                image_id   = 0;
  ThumbnailResolution       resolution = ThumbnailResolution::k1024;
  ThumbnailDiskCachePurpose purpose    = ThumbnailDiskCachePurpose::kThumbnail;
  std::function<bool()>     canceled;
  /// Takes the RGBA8 display buffer. Returns the guard that now owns it, or null when the
  /// requester no longer wants it; only an owned buffer is written to the disk cache.
  std::function<std::shared_ptr<ThumbnailGuard>(std::unique_ptr<ImageBuffer>)> on_pixels;
  std::function<void(ThumbnailRequestStatus, std::string)>                     on_failure;
  std::atomic<bool>                                                            finished{false};

  [[nodiscard]] auto IsCanceled() const -> bool {
    try {
      return canceled && canceled();
    } catch (...) {
      return true;
    }
  }

  auto DeliverPixels(std::unique_ptr<ImageBuffer> pixels) -> std::shared_ptr<ThumbnailGuard> {
    if (finished.exchange(true)) {
      return nullptr;
    }
    return on_pixels ? on_pixels(std::move(pixels)) : nullptr;
  }

  void Fail(ThumbnailRequestStatus status, std::string message) {
    if (finished.exchange(true)) {
      return;
    }
    if (on_failure) {
      on_failure(status, std::move(message));
    }
  }

  /// Fail as canceled when the requester canceled, otherwise as an error with @p message.
  void FailCanceledOr(std::string message) {
    if (IsCanceled()) {
      Fail(ThumbnailRequestStatus::kCanceled, "Request was canceled.");
    } else {
      Fail(ThumbnailRequestStatus::kError, std::move(message));
    }
  }
};

}  // namespace

struct ThumbnailService::State {
  static constexpr size_t default_cache_size_ = 64;

  struct PendingCallback {
    ThumbnailResultCallback callback_{};
    CallbackDispatcher      dispatcher_{};
    ThumbnailCacheKey       key_{};
  };

  std::shared_ptr<SleeveServiceImpl>             sleeve_service_     = nullptr;
  std::shared_ptr<ImagePoolService>              image_pool_service_ = nullptr;
  std::shared_ptr<PipelineMgmtService>           pipeline_service_   = nullptr;
  std::shared_ptr<Storage>                       storage_            = nullptr;
  std::string                                    project_uuid_;
  std::unique_ptr<ThumbnailDiskCacheService>     disk_cache_service_;
  BatchExecutorPool                              executors_;
  // Render workers, one per batch executor.
  PipelineScheduler                              render_scheduler_;
  // Snapshot acquisition and disk cache reads, off the caller's thread.
  ThreadPool                                     lookup_thread_pool_;

  std::mutex                                     cache_lock_;

  // LRU keyed by composite {element_id, resolution_tier}.
  LRUCache<ThumbnailCacheKey, ThumbnailCacheKey> thumbnail_cache_;
  std::unordered_map<ThumbnailCacheKey, std::shared_ptr<ThumbnailGuard>> thumbnail_cache_data_{};
  std::unordered_map<ThumbnailCacheKey, std::vector<PendingCallback>>    pending_{};

  // Generation tokens for Strategy A (pre-flight cancellation).
  // Tokens are keyed by {element, resolution} so cancelling an old zoom tier
  // does not invalidate the currently visible tier for the same element.
  std::unordered_map<ThumbnailCacheKey, std::shared_ptr<std::atomic<uint64_t>>>
      generation_tokens_{};

  // Cancel flags of analysis renditions that have not delivered, by request identity.
  std::unordered_map<AnalysisRenditionId, std::shared_ptr<std::atomic<bool>>> analysis_requests_{};
  AnalysisRenditionId                                                         next_analysis_id_ = 1;

  State(std::shared_ptr<SleeveServiceImpl>   sleeve_service,
        std::shared_ptr<ImagePoolService>    image_pool_service,
        std::shared_ptr<PipelineMgmtService> pipeline_service,
        std::shared_ptr<Storage> storage_service, std::string project_uuid,
        std::filesystem::path thumbnail_cache_root, std::size_t batch_executor_count)
      : sleeve_service_(std::move(sleeve_service)),
        image_pool_service_(std::move(image_pool_service)),
        pipeline_service_(std::move(pipeline_service)),
        storage_(std::move(storage_service)),
        project_uuid_(std::move(project_uuid)),
        executors_(batch_executor_count, pipeline_service_
                                             ? pipeline_service_->GetAcceleratorBackendPreference()
                                             : AcceleratorBackendPreference::Auto),
        render_scheduler_(executors_.Size()),
        lookup_thread_pool_(2),
        thumbnail_cache_(default_cache_size_) {
    if (storage_ && !project_uuid_.empty()) {
      if (thumbnail_cache_root.empty()) {
        disk_cache_service_ = std::make_unique<ThumbnailDiskCacheService>();
      } else {
        disk_cache_service_ = std::make_unique<ThumbnailDiskCacheService>(thumbnail_cache_root);
      }
      disk_cache_service_->Initialize(project_uuid_);
    }
  }

  // Get or create a generation token for the given request key.
  auto GetOrCreateGenerationToken(const ThumbnailCacheKey& key)
      -> std::shared_ptr<std::atomic<uint64_t>> {
    auto it = generation_tokens_.find(key);
    if (it != generation_tokens_.end() && it->second) {
      return it->second;
    }
    auto token              = std::make_shared<std::atomic<uint64_t>>(0);
    generation_tokens_[key] = token;
    return token;
  }

  void IncrementGenerationTokenLocked(const ThumbnailCacheKey& key) {
    auto token = GetOrCreateGenerationToken(key);
    token->fetch_add(1);
  }

  auto BuildDiskCacheKey(const PipelineGraphSnapshot& snapshot, ThumbnailResolution resolution,
                         ThumbnailDiskCachePurpose purpose) const
      -> std::optional<ThumbnailDiskCacheKey> {
    if (!disk_cache_service_ || project_uuid_.empty()) {
      return std::nullopt;
    }
    ThumbnailDiskCacheKey key;
    key.project_uuid         = project_uuid_;
    key.element_id           = snapshot.ElementId();
    key.resolution           = resolution;
    key.purpose              = purpose;
    key.edit_version_hash    = CommittedHistoryLabel(snapshot);
    key.cache_schema_version = kDiskCacheSchemaVersion;
    return key;
  }

  /// Queue @p request on a lookup worker.
  static void StartRendition(const std::shared_ptr<State>&     st,
                             std::shared_ptr<RenditionRequest> request);
  /// Lookup stage: committed snapshot, then the disk cache, then a render.
  static void LookUpRendition(const std::shared_ptr<State>&            st,
                              const std::shared_ptr<RenditionRequest>& request);
  /// Render stage on a render worker and one batch executor of the pool.
  static void ScheduleRenditionRender(const std::shared_ptr<State>&                st,
                                      const std::shared_ptr<RenditionRequest>&     request,
                                      std::shared_ptr<const PipelineGraphSnapshot> snapshot,
                                      std::optional<ThumbnailDiskCacheKey>         disk_key);

  /// Stop both worker pools while every member is still alive: queued work is dropped and
  /// running work finishes. Each queued task holds the State, so the owner calls this before it
  /// drops its reference; otherwise the last task would destroy the pools from their own thread.
  void        StopWorkers() {
    lookup_thread_pool_.Shutdown();
    render_scheduler_.Shutdown();
  }

  ~State() { StopWorkers(); }
};

void ThumbnailService::State::StartRendition(const std::shared_ptr<State>&     st,
                                             std::shared_ptr<RenditionRequest> request) {
  st->lookup_thread_pool_.Submit([st, request = std::move(request)] {
    try {
      LookUpRendition(st, request);
    } catch (const std::exception& e) {
      request->Fail(ThumbnailRequestStatus::kError, e.what());
    } catch (...) {
      request->Fail(ThumbnailRequestStatus::kError,
                    "[ERROR] ThumbnailService: lookup failed with an unknown error.");
    }
  });
}

void ThumbnailService::State::LookUpRendition(const std::shared_ptr<State>&            st,
                                              const std::shared_ptr<RenditionRequest>& request) {
  if (request->IsCanceled()) {
    request->Fail(ThumbnailRequestStatus::kCanceled, "Request was canceled.");
    return;
  }

  std::shared_ptr<const PipelineGraphSnapshot> snapshot;
  try {
    snapshot = st->pipeline_service_->AcquireCommittedSnapshot(request->element_id);
  } catch (const std::exception& e) {
    request->Fail(ThumbnailRequestStatus::kError,
                  std::format("[ERROR] ThumbnailService: committed state of element {} is not "
                              "available: {}",
                              request->element_id, e.what()));
    return;
  }

  const auto disk_key = st->BuildDiskCacheKey(*snapshot, request->resolution, request->purpose);
  if (disk_key.has_value()) {
    auto disk_buffer = st->disk_cache_service_->Read(*disk_key);
    if (request->IsCanceled()) {
      request->Fail(ThumbnailRequestStatus::kCanceled, "Request was canceled.");
      return;
    }
    if (disk_buffer && disk_buffer->cpu_data_valid_) {
      if (auto display = MakeDisplayCacheThumbnailBuffer(*disk_buffer)) {
        (void)request->DeliverPixels(std::move(display));
        return;
      }
    }
  }

  ScheduleRenditionRender(st, request, std::move(snapshot), disk_key);
}

void ThumbnailService::State::ScheduleRenditionRender(
    const std::shared_ptr<State>& st, const std::shared_ptr<RenditionRequest>& request,
    std::shared_ptr<const PipelineGraphSnapshot> snapshot,
    std::optional<ThumbnailDiskCacheKey>         disk_key) {
  // The executor a render takes from the pool, returned when the task completes.
  struct ExecutorLease {
    std::optional<std::size_t> index;
  };
  auto         lease = std::make_shared<ExecutorLease>();

  PipelineTask task;
  task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
  task.options_.render_desc_.max_edge_    = ResolutionToMaxEdge(request->resolution);
  task.options_.render_desc_.decode_res_  = ResolutionToDecodeRes(request->resolution);
  task.options_.is_blocking_              = false;
  task.options_.is_callback_              = true;
  task.options_.is_seq_callback_          = false;
  task.cancel_requested_                  = [request] { return request->IsCanceled(); };
  task.snapshot_under_render_lock_        = [snapshot = std::move(snapshot)] { return snapshot; };

  task.prepare_                           = [st, request, lease](PipelineTask& prepared) -> bool {
    auto image = st->image_pool_service_->Read<std::shared_ptr<Image>>(
        request->image_id, [](const std::shared_ptr<Image>& img) { return img; });
    if (!image) {
      throw std::runtime_error(
          std::format("[ERROR] ThumbnailService: image {} of element {} is not in the pool.",
                                                request->image_id, request->element_id));
    }
    auto [index, executor]      = st->executors_.Take();
    lease->index                = index;
    prepared.pipeline_executor_ = std::move(executor);
    prepared.input_desc_        = std::move(image);
    return true;
  };

  task.callback_ = [st, request, disk_key](ImageBuffer& result) {
    if (request->IsCanceled()) {
      return;
    }
    auto display = MakeDisplayCacheThumbnailBuffer(result);
    if (!display) {
      return;
    }
    const auto guard = request->DeliverPixels(std::move(display));
    if (guard && guard->thumbnail_buffer_ && disk_key.has_value()) {
      try {
        std::shared_ptr<ImageBuffer> pixels(guard, guard->thumbnail_buffer_.get());
        st->disk_cache_service_->EnqueueWrite(*disk_key, std::move(pixels));
      } catch (...) {
        // The disk cache only speeds up later requests; the pixels were already delivered.
      }
    }
  };

  // Runs once on every terminal path, after the render released the executor's lock. A request
  // that delivered pixels ignores the failure below.
  task.on_complete_ = [st, request, lease](bool success, std::string message) {
    if (lease->index.has_value()) {
      st->executors_.Return(*lease->index);
    }
    request->FailCanceledOr(
        success || message.empty()
            ? std::format("[ERROR] ThumbnailService: render of element {} produced no thumbnail "
                          "buffer.",
                          request->element_id)
            : std::move(message));
  };

  try {
    st->render_scheduler_.ScheduleTask(std::move(task));
  } catch (const std::exception& e) {
    request->Fail(ThumbnailRequestStatus::kError,
                  std::format("[ERROR] ThumbnailService: failed to schedule element {}: {}",
                              request->element_id, e.what()));
  }
}

ThumbnailService::ThumbnailService(std::shared_ptr<SleeveServiceImpl>   sleeve_service,
                                   std::shared_ptr<ImagePoolService>    image_pool_service,
                                   std::shared_ptr<PipelineMgmtService> pipeline_service,
                                   std::shared_ptr<Storage>             storage_service,
                                   const std::string&                   project_uuid,
                                   const std::filesystem::path&         thumbnail_cache_root,
                                   std::size_t                          batch_executor_count)
    : state_(std::make_shared<State>(std::move(sleeve_service), std::move(image_pool_service),
                                     std::move(pipeline_service), std::move(storage_service),
                                     project_uuid, thumbnail_cache_root, batch_executor_count)) {}

ThumbnailService::~ThumbnailService() {
  if (state_) {
    state_->StopWorkers();
  }
}

void ThumbnailService::GetThumbnail(sl_element_id_t id, image_id_t image_id,
                                    ThumbnailCallback callback, bool pin_if_found,
                                    CallbackDispatcher dispatcher, ThumbnailResolution resolution) {
  GetThumbnailDetailed(
      id, image_id,
      [callback = std::move(callback)](ThumbnailRequestResult result) {
        if (callback) {
          callback(std::move(result.guard));
        }
      },
      pin_if_found, std::move(dispatcher), resolution);
}

void ThumbnailService::GetThumbnailDetailed(sl_element_id_t id, image_id_t image_id,
                                            ThumbnailResultCallback callback, bool pin_if_found,
                                            CallbackDispatcher  dispatcher,
                                            ThumbnailResolution resolution) {
  auto st = state_;
  if (!st || !st->image_pool_service_ || !st->pipeline_service_) {
    throw std::runtime_error("[ERROR] ThumbnailService: Services not initialized.");
  }

  const ThumbnailCacheKey         cache_key{id, resolution};

  std::shared_ptr<ThumbnailGuard> guard;
  {
    std::unique_lock lock(st->cache_lock_);
    if (st->thumbnail_cache_.AccessElement(cache_key).has_value()) {
      auto guard_it = st->thumbnail_cache_data_.find(cache_key);
      if (guard_it != st->thumbnail_cache_data_.end() && guard_it->second) {
        guard = guard_it->second;
        if (pin_if_found) {
          guard->pin_count_++;
        }
      } else {
        st->thumbnail_cache_.RemoveRecord(cache_key);
      }
    }
  }

  if (guard) {
    DispatchThumbnailResultCallback(callback, dispatcher,
                                    ThumbnailRequestResult{.guard  = guard,
                                                           .status = ThumbnailRequestStatus::kReady,
                                                           .message = {},
                                                           .key     = cache_key});
    return;
  }

  std::shared_ptr<std::atomic<uint64_t>> gen_token;
  uint64_t                               expected_gen = 0;
  {
    std::unique_lock       lock(st->cache_lock_);
    State::PendingCallback pending_cb{};
    pending_cb.callback_   = std::move(callback);
    pending_cb.dispatcher_ = std::move(dispatcher);
    pending_cb.key_        = cache_key;
    auto it                = st->pending_.find(cache_key);
    if (it != st->pending_.end()) {
      it->second.push_back(std::move(pending_cb));
      return;
    }
    std::vector<State::PendingCallback> pending_callbacks;
    pending_callbacks.push_back(std::move(pending_cb));
    st->pending_.emplace(cache_key, std::move(pending_callbacks));

    gen_token    = st->GetOrCreateGenerationToken(cache_key);
    expected_gen = gen_token->load();
  }

  // A changed generation means CancelPending or InvalidateThumbnail already answered or dropped
  // this request's callbacks, and a later request for the same key may be waiting.
  const auto generation_changed = [gen_token, expected_gen] {
    return gen_token->load() != expected_gen;
  };

  auto request        = std::make_shared<RenditionRequest>();
  request->element_id = id;
  request->image_id   = image_id;
  request->resolution = resolution;
  request->purpose    = ThumbnailDiskCachePurpose::kThumbnail;
  request->canceled   = generation_changed;

  request->on_pixels  = [st, cache_key, generation_changed](
                           std::unique_ptr<ImageBuffer> pixels) -> std::shared_ptr<ThumbnailGuard> {
    std::shared_ptr<ThumbnailGuard>     published;
    std::vector<State::PendingCallback> callbacks;
    {
      std::unique_lock lock(st->cache_lock_);
      if (generation_changed()) {
        return nullptr;
      }
      auto pending_it = st->pending_.find(cache_key);
      if (pending_it == st->pending_.end()) {
        return nullptr;
      }
      callbacks = std::move(pending_it->second);
      st->pending_.erase(pending_it);

      published                    = std::make_shared<ThumbnailGuard>();
      published->thumbnail_buffer_ = std::move(pixels);
      published->pin_count_        = static_cast<int>(std::max<size_t>(1, callbacks.size()));
      auto evicted = st->thumbnail_cache_.RecordAccess_WithEvict(cache_key, cache_key);
      HandleEvict(*st, evicted);
      st->thumbnail_cache_data_[cache_key] = published;
    }
    for (const auto& pending_cb : callbacks) {
      DispatchThumbnailResultCallback(
          pending_cb.callback_, pending_cb.dispatcher_,
          ThumbnailRequestResult{.guard   = published,
                                 .status  = ThumbnailRequestStatus::kReady,
                                 .message = {},
                                 .key     = cache_key});
    }
    return published;
  };

  request->on_failure = [st, cache_key, generation_changed](ThumbnailRequestStatus status,
                                                            std::string            message) {
    std::vector<State::PendingCallback> callbacks;
    {
      std::unique_lock lock(st->cache_lock_);
      if (generation_changed()) {
        return;
      }
      auto it = st->pending_.find(cache_key);
      if (it != st->pending_.end()) {
        callbacks = std::move(it->second);
        st->pending_.erase(it);
      }
    }
    for (const auto& pending_cb : callbacks) {
      DispatchThumbnailResultCallback(
          pending_cb.callback_, pending_cb.dispatcher_,
          ThumbnailRequestResult{
              .guard = nullptr, .status = status, .message = message, .key = cache_key});
    }
  };

  State::StartRendition(st, std::move(request));
}

auto ThumbnailService::RequestAnalysisRendition(sl_element_id_t element_id, image_id_t image_id,
                                                ThumbnailResolution     resolution,
                                                ThumbnailResultCallback callback)
    -> AnalysisRenditionId {
  auto st = state_;
  if (!st || !st->image_pool_service_ || !st->pipeline_service_) {
    throw std::runtime_error("[ERROR] ThumbnailService: Services not initialized.");
  }

  const ThumbnailCacheKey cache_key{element_id, resolution};
  auto                    cancel_flag = std::make_shared<std::atomic<bool>>(false);
  AnalysisRenditionId     id          = 0;
  {
    std::unique_lock lock(st->cache_lock_);
    id                         = st->next_analysis_id_++;
    st->analysis_requests_[id] = cancel_flag;
  }

  // The request delivers once; afterwards a cancel of its identity has nothing to cancel.
  auto forget = [weak_state = std::weak_ptr<State>(st), id] {
    if (auto state = weak_state.lock()) {
      std::unique_lock lock(state->cache_lock_);
      state->analysis_requests_.erase(id);
    }
  };

  auto request        = std::make_shared<RenditionRequest>();
  request->element_id = element_id;
  request->image_id   = image_id;
  request->resolution = resolution;
  request->purpose    = ThumbnailDiskCachePurpose::kAnalysis;
  request->canceled   = [cancel_flag] { return cancel_flag->load(); };
  request->on_pixels  = [callback, cache_key, forget](
                           std::unique_ptr<ImageBuffer> pixels) -> std::shared_ptr<ThumbnailGuard> {
    forget();
    auto guard               = std::make_shared<ThumbnailGuard>();
    guard->thumbnail_buffer_ = std::move(pixels);
    guard->pin_count_        = 1;
    DispatchThumbnailResultCallback(callback, nullptr,
                                    ThumbnailRequestResult{.guard  = guard,
                                                           .status = ThumbnailRequestStatus::kReady,
                                                           .message = {},
                                                           .key     = cache_key});
    return guard;
  };
  request->on_failure = [callback, cache_key, forget](ThumbnailRequestStatus status,
                                                      std::string            message) {
    forget();
    DispatchThumbnailResultCallback(
        callback, nullptr,
        ThumbnailRequestResult{
            .guard = nullptr, .status = status, .message = std::move(message), .key = cache_key});
  };

  State::StartRendition(st, std::move(request));
  return id;
}

void ThumbnailService::CancelAnalysisRendition(AnalysisRenditionId id) {
  auto st = state_;
  if (!st) {
    return;
  }
  std::unique_lock lock(st->cache_lock_);
  if (const auto it = st->analysis_requests_.find(id); it != st->analysis_requests_.end()) {
    it->second->store(true);
  }
}

void ThumbnailService::ReleaseAnalysisRendition(AnalysisRenditionId id) {
  auto st = state_;
  if (!st) {
    return;
  }
  std::unique_lock lock(st->cache_lock_);
  st->analysis_requests_.erase(id);
}

void ThumbnailService::CancelPending(const ThumbnailCacheKey& key) {
  auto st = state_;
  if (!st) {
    return;
  }

  std::vector<State::PendingCallback> callbacks_to_dispatch;
  {
    std::unique_lock lock(st->cache_lock_);

    // Increment the generation token so queued/in-flight work for this key
    // sees the mismatch and skips publishing stale results.
    st->IncrementGenerationTokenLocked(key);

    auto it = st->pending_.find(key);
    if (it != st->pending_.end()) {
      callbacks_to_dispatch = std::move(it->second);
      st->pending_.erase(it);
    }
  }

  for (const auto& cb : callbacks_to_dispatch) {
    DispatchThumbnailResultCallback(
        cb.callback_, cb.dispatcher_,
        ThumbnailRequestResult{.guard   = nullptr,
                               .status  = ThumbnailRequestStatus::kCanceled,
                               .message = "Thumbnail request was canceled.",
                               .key     = key});
  }
}

void ThumbnailService::CancelPending(sl_element_id_t sleeve_element_id) {
  auto st = state_;
  if (!st) {
    return;
  }

  std::vector<State::PendingCallback> callbacks_to_dispatch;
  {
    std::unique_lock lock(st->cache_lock_);

    for (auto res : kAllThumbnailResolutions) {
      ThumbnailCacheKey key{sleeve_element_id, res};
      st->IncrementGenerationTokenLocked(key);
      auto it = st->pending_.find(key);
      if (it == st->pending_.end()) {
        continue;
      }
      auto callbacks = std::move(it->second);
      st->pending_.erase(it);
      callbacks_to_dispatch.insert(callbacks_to_dispatch.end(),
                                   std::make_move_iterator(callbacks.begin()),
                                   std::make_move_iterator(callbacks.end()));
    }
  }

  for (const auto& cb : callbacks_to_dispatch) {
    DispatchThumbnailResultCallback(
        cb.callback_, cb.dispatcher_,
        ThumbnailRequestResult{.guard   = nullptr,
                               .status  = ThumbnailRequestStatus::kCanceled,
                               .message = "Thumbnail request was canceled.",
                               .key     = cb.key_});
  }
}

void ThumbnailService::ReleaseThumbnail(const ThumbnailCacheKey& key) {
  auto st = state_;
  if (!st) {
    return;
  }

  CancelPending(key);

  std::unique_lock lock(st->cache_lock_);

  auto             it = st->thumbnail_cache_data_.find(key);
  if (it == st->thumbnail_cache_data_.end() || !it->second) {
    st->thumbnail_cache_.RemoveRecord(key);
    return;
  }

  auto guard = it->second;
  if (guard->pin_count_ > 0) {
    guard->pin_count_--;
  }

  if (guard->pin_count_ == 0) {
    st->thumbnail_cache_.RemoveRecord(key);
    st->thumbnail_cache_data_.erase(it);
  }
}

void ThumbnailService::ReleaseThumbnail(sl_element_id_t sleeve_element_id) {
  for (auto res : kAllThumbnailResolutions) {
    ReleaseThumbnail(ThumbnailCacheKey{sleeve_element_id, res});
  }
}

void ThumbnailService::InvalidateThumbnail(sl_element_id_t sleeve_element_id) {
  auto st = state_;
  if (!st) {
    return;
  }

  {
    std::unique_lock lock(st->cache_lock_);

    // Invalidate all resolution tiers for this element.
    for (auto res : kAllThumbnailResolutions) {
      ThumbnailCacheKey key{sleeve_element_id, res};
      st->IncrementGenerationTokenLocked(key);
      st->pending_.erase(key);
      st->thumbnail_cache_.RemoveRecord(key);
      st->thumbnail_cache_data_.erase(key);
    }
  }

  if (st->disk_cache_service_ && !st->project_uuid_.empty()) {
    st->disk_cache_service_->Invalidate(st->project_uuid_, sleeve_element_id);
  }
}

void ThumbnailService::ResizeCache(uint32_t desired_capacity) {
  auto st = state_;
  if (!st) {
    return;
  }

  std::unique_lock   lock(st->cache_lock_);

  // Clamp to reasonable bounds. The UI may request very small capacities for
  // 2048px tiers because cached thumbnails are float RGBA ImageBuffers.
  constexpr uint32_t kMinCacheSize = 4;
  constexpr uint32_t kMaxCacheSize = 1024;
  const uint32_t     capacity      = std::clamp(desired_capacity, kMinCacheSize, kMaxCacheSize);

  // Only shrink if all currently-cached entries are unpinned.
  // If pinned items would exceed capacity, keep current size.
  uint32_t           pinned_count  = 0;
  for (const auto& [key, guard] : st->thumbnail_cache_data_) {
    if (guard && guard->pin_count_ > 0) {
      pinned_count++;
    }
  }

  const uint32_t effective_capacity = std::max(capacity, pinned_count);
  const auto     evicted_keys       = st->thumbnail_cache_.Resize_WithEvict(effective_capacity);
  for (const auto& evicted_key : evicted_keys) {
    HandleEvict(*st, evicted_key);
  }
}

void ThumbnailService::HandleEvict(State& st, std::optional<ThumbnailCacheKey> evicted_key) {
  if (evicted_key.has_value()) {
    const auto& key = evicted_key.value();
    auto        it  = st.thumbnail_cache_data_.find(key);
    if (it != st.thumbnail_cache_data_.end() && it->second) {
      auto guard = it->second;
      if (guard->pin_count_ <= 0) {
        st.thumbnail_cache_data_.erase(it);
      } else {
        // Re-insert into cache since it's still pinned.
        // Boost the cache size to avoid immediate eviction.
        st.thumbnail_cache_.Resize(static_cast<uint32_t>(st.thumbnail_cache_data_.size() + 5));
        st.thumbnail_cache_.RecordAccess(key, key);
      }
    }
  }
}

// ── Phase 4: Disk cache configuration & operations ────────────────────────

void ThumbnailService::SetDiskCacheEnabled(bool enabled) {
  if (state_ && state_->disk_cache_service_) {
    state_->disk_cache_service_->SetEnabled(enabled);
  }
}

bool ThumbnailService::IsDiskCacheEnabled() const {
  if (state_ && state_->disk_cache_service_) {
    return state_->disk_cache_service_->IsEnabled();
  }
  return false;
}

void ThumbnailService::SetDiskCacheRoot(const std::filesystem::path& cache_root) {
  if (state_ && state_->disk_cache_service_) {
    state_->disk_cache_service_->SetCacheRoot(cache_root);
  }
}

std::filesystem::path ThumbnailService::GetDiskCacheRoot() const {
  if (state_ && state_->disk_cache_service_) {
    return state_->disk_cache_service_->GetCacheRoot();
  }
  return {};
}

void ThumbnailService::SetDiskCacheMaxEntries(size_t max_entries) {
  if (state_ && state_->disk_cache_service_) {
    state_->disk_cache_service_->SetMaxEntries(max_entries);
  }
}

size_t ThumbnailService::GetDiskCacheMaxEntries() const {
  if (state_ && state_->disk_cache_service_) {
    return state_->disk_cache_service_->GetMaxEntries();
  }
  return 0;
}

void ThumbnailService::SetDiskCacheJpegQuality(int quality) {
  if (state_ && state_->disk_cache_service_) {
    state_->disk_cache_service_->SetJpegQuality(quality);
  }
}

int ThumbnailService::GetDiskCacheJpegQuality() const {
  if (state_ && state_->disk_cache_service_) {
    return state_->disk_cache_service_->GetJpegQuality();
  }
  return 85;
}

void ThumbnailService::SetDiskCacheWebPQuality(int quality) {
  if (state_ && state_->disk_cache_service_) {
    state_->disk_cache_service_->SetWebPQuality(quality);
  }
}

int ThumbnailService::GetDiskCacheWebPQuality() const {
  if (state_ && state_->disk_cache_service_) {
    return state_->disk_cache_service_->GetWebPQuality();
  }
  return 80;
}

void ThumbnailService::ClearAllDiskCache() {
  if (state_ && state_->disk_cache_service_) {
    state_->disk_cache_service_->ClearAll();
  }
}

void ThumbnailService::ClearProjectDiskCache() {
  if (state_ && state_->disk_cache_service_ && !state_->project_uuid_.empty()) {
    state_->disk_cache_service_->ClearProject(state_->project_uuid_);
  }
}

void ThumbnailService::FlushDiskCacheMetadata() {
  if (state_ && state_->disk_cache_service_) {
    state_->disk_cache_service_->FlushMetadata();
  }
}

auto ThumbnailService::GetDiskCacheStats() const -> DiskCacheStats {
  DiskCacheStats s;
  if (state_ && state_->disk_cache_service_) {
    auto raw           = state_->disk_cache_service_->GetStats();
    s.total_entries    = raw.total_entries;
    s.total_size_bytes = raw.total_size_bytes;
    s.hit_count        = raw.hit_count;
    s.miss_count       = raw.miss_count;
    s.max_entries      = raw.max_entries;
    s.enabled          = raw.enabled;
    s.cache_root_path  = raw.cache_root_path;
  }
  return s;
}

ThumbnailServiceAnalysisRenditionProvider::ThumbnailServiceAnalysisRenditionProvider(
    std::shared_ptr<ThumbnailService> service)
    : service_(std::move(service)) {}

void ThumbnailServiceAnalysisRenditionProvider::RequestRendition(sl_element_id_t         element_id,
                                                                 image_id_t              image_id,
                                                                 ThumbnailResolution     resolution,
                                                                 ThumbnailResultCallback callback) {
  const ThumbnailCacheKey key{element_id, resolution};
  if (!service_) {
    callback(ThumbnailRequestResult{.guard   = nullptr,
                                    .status  = ThumbnailRequestStatus::kError,
                                    .message = "ThumbnailService is not available",
                                    .key     = key});
    return;
  }
  // Hold the lock across the request so a cancel of this key cannot run between the request and
  // the recording of its identity.
  std::scoped_lock lock(mutex_);
  const auto       id =
      service_->RequestAnalysisRendition(element_id, image_id, resolution, std::move(callback));
  requests_.emplace(key, id);
}

void ThumbnailServiceAnalysisRenditionProvider::CancelRendition(const ThumbnailCacheKey& key) {
  if (!service_) {
    return;
  }
  std::scoped_lock lock(mutex_);
  const auto [begin, end] = requests_.equal_range(key);
  for (auto it = begin; it != end; ++it) {
    service_->CancelAnalysisRendition(it->second);
  }
}

void ThumbnailServiceAnalysisRenditionProvider::ReleaseRendition(const ThumbnailCacheKey& key) {
  if (!service_) {
    return;
  }
  std::scoped_lock lock(mutex_);
  const auto [begin, end] = requests_.equal_range(key);
  for (auto it = begin; it != end; ++it) {
    service_->ReleaseAnalysisRendition(it->second);
  }
  requests_.erase(begin, end);
}
};  // namespace alcedo
