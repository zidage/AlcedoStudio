//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/thumbnail_service.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "app/history_mgmt_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/render_service.hpp"
#include "app/thumbnail_disk_cache_service.hpp"
#include "storage/store/edit_history/commit_graph_store.hpp"
#include "concurrency/thread_pool.hpp"
#include "json.hpp"
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

auto IsRenderableThumbnailResult(const ImageBuffer& result_buffer) -> bool {
  return result_buffer.buffer_valid_ || result_buffer.cpu_data_valid_ ||
         result_buffer.gpu_data_valid_;
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

auto ReadColorTempOperatorParams(const std::shared_ptr<PipelineGuard>& pipeline)
    -> std::optional<nlohmann::json> {
  if (!pipeline || !pipeline->pipeline_) {
    return std::nullopt;
  }

  auto& to_ws_stage      = pipeline->pipeline_->GetStage(PipelineStageName::To_WorkingSpace);
  auto  color_temp_entry = to_ws_stage.GetOperator(OperatorType::COLOR_TEMP);
  if (!color_temp_entry.has_value() || !color_temp_entry.value() ||
      !color_temp_entry.value()->op_) {
    return std::nullopt;
  }

  return color_temp_entry.value()->op_->GetParams();
}

constexpr ThumbnailResolution kAllThumbnailResolutions[] = {
    ThumbnailResolution::k256,
    ThumbnailResolution::k512,
    ThumbnailResolution::k1024,
    ThumbnailResolution::k2048,
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
  std::shared_ptr<Storage>                 storage_    = nullptr;
  std::string                                    project_uuid_;
  std::unique_ptr<ThumbnailDiskCacheService>     disk_cache_service_;
  ThreadPool                                     disk_read_thread_pool_;

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

  // Phase 3: cancel tokens for analysis renditions. Separate from
  // generation_tokens_ to avoid collision when the same {element, resolution}
  // is rendered both via the live thumbnail path and the snapshot path.
  std::unordered_map<ThumbnailCacheKey, std::shared_ptr<std::atomic<uint64_t>>> analysis_tokens_{};

  // Pipeline scheduler (global/shared), must outlive tasks.
  std::shared_ptr<PipelineScheduler> pipeline_scheduler_ = nullptr;

  State(std::shared_ptr<SleeveServiceImpl>      sleeve_service,
        std::shared_ptr<ImagePoolService>       image_pool_service,
        std::shared_ptr<PipelineMgmtService>    pipeline_service,
        std::shared_ptr<Storage>          storage_service, std::string project_uuid,
        std::filesystem::path thumbnail_cache_root)
      : sleeve_service_(std::move(sleeve_service)),
        image_pool_service_(std::move(image_pool_service)),
        pipeline_service_(std::move(pipeline_service)),
        storage_(std::move(storage_service)),
        project_uuid_(std::move(project_uuid)),
        disk_read_thread_pool_(2),
        thumbnail_cache_(default_cache_size_) {
    pipeline_scheduler_ = RenderService::GetThumbnailOrExportScheduler();
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

  // Phase 3: analysis-rendition cancel token. Mirrors the generation-token
  // helpers but keys off analysis_tokens_ so the two render paths never collide.
  auto GetOrCreateAnalysisToken(const ThumbnailCacheKey& key)
      -> std::shared_ptr<std::atomic<uint64_t>> {
    auto it = analysis_tokens_.find(key);
    if (it != analysis_tokens_.end() && it->second) {
      return it->second;
    }
    auto token            = std::make_shared<std::atomic<uint64_t>>(0);
    analysis_tokens_[key] = token;
    return token;
  }

  // Acquires cache_lock_ internally; called from CancelAnalysisRendition with no
  // lock held.
  void IncrementAnalysisToken(const ThumbnailCacheKey& key) {
    std::unique_lock<std::mutex> lk(cache_lock_);
    GetOrCreateAnalysisToken(key)->fetch_add(1);
  }

  auto ReadCurrentVersionHash(sl_element_id_t id) -> std::string {
    if (storage_) {
      try {
        auto               db_guard = storage_->GetDatabase().GetConnectionGuard();
        auto               db_lock  = db_guard.Lock();
        CommitGraphStore graph_service(db_guard.conn_);
        const auto         graph = graph_service.LoadGraph(id);
        if (graph.has_value()) {
          const auto head = graph->GetActiveVersionRef().head_commit_hash;
          return head.has_value() ? head->ToString() : graph->GetRootId().ToString();
        }
      } catch (...) {
      }
    }
    return {};
  }

  auto BuildDiskCacheKey(sl_element_id_t id, ThumbnailResolution resolution,
                         ThumbnailDiskCachePurpose purpose)
      -> std::optional<ThumbnailDiskCacheKey> {
    if (!disk_cache_service_ || !storage_ || project_uuid_.empty()) {
      return std::nullopt;
    }

    ThumbnailDiskCacheKey key;
    key.project_uuid         = project_uuid_;
    key.element_id           = id;
    key.resolution           = resolution;
    key.purpose              = purpose;
    key.edit_version_hash    = ReadCurrentVersionHash(id);
    key.cache_schema_version = 1;
    if (key.edit_version_hash.empty()) {
      return std::nullopt;
    }
    return key;
  }

  // The pool is declared before cache_lock_/pending_/thumbnail_cache_data_
  // (members above), so the compiler-generated destructor destroys those first
  // and the pool last — but ThreadPool::~ThreadPool DRAINS queued tasks, which
  // access exactly those already-destroyed members (use-after-free). Stop the
  // pool here, while every member is still alive, so the queued disk-read tasks
  // are dropped and in-flight ones finish against live state. The pool's own
  // destructor then drains an empty queue (no-op).
  ~State() { disk_read_thread_pool_.Shutdown(); }
};

ThumbnailService::ThumbnailService(std::shared_ptr<SleeveServiceImpl>      sleeve_service,
                                   std::shared_ptr<ImagePoolService>       image_pool_service,
                                   std::shared_ptr<PipelineMgmtService>    pipeline_service,
                                   std::shared_ptr<Storage>          storage_service,
                                   const std::string&                      project_uuid,
                                   const std::filesystem::path&            thumbnail_cache_root)
    : state_(std::make_shared<State>(std::move(sleeve_service), std::move(image_pool_service),
                                     std::move(pipeline_service), std::move(storage_service),
                                     project_uuid, thumbnail_cache_root)) {}

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
  if (!st || !st->image_pool_service_ || !st->pipeline_service_ || !st->pipeline_scheduler_) {
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

  const auto disk_key =
      st->BuildDiskCacheKey(id, resolution, ThumbnailDiskCachePurpose::kThumbnail);

  std::shared_ptr<std::atomic<uint64_t>> gen_token;
  uint64_t                               expected_gen = 0;
  {
    std::unique_lock lock(st->cache_lock_);
    auto             it = st->pending_.find(cache_key);
    if (it != st->pending_.end()) {
      State::PendingCallback pending_cb{};
      pending_cb.callback_   = std::move(callback);
      pending_cb.dispatcher_ = std::move(dispatcher);
      pending_cb.key_        = cache_key;
      it->second.push_back(std::move(pending_cb));
      return;
    }
    State::PendingCallback pending_cb{};
    pending_cb.callback_   = std::move(callback);
    pending_cb.dispatcher_ = std::move(dispatcher);
    pending_cb.key_        = cache_key;
    std::vector<State::PendingCallback> pending_callbacks;
    pending_callbacks.push_back(std::move(pending_cb));
    st->pending_.emplace(cache_key, std::move(pending_callbacks));

    gen_token    = st->GetOrCreateGenerationToken(cache_key);
    expected_gen = gen_token->load();
  }

  auto schedule_pipeline_render = [st, id, image_id, cache_key, resolution, gen_token,
                                   expected_gen]() {
    struct ThumbnailTaskContext {
      std::shared_ptr<PipelineGuard> pipeline{};
      std::optional<nlohmann::json>  pre_render_color_temp_params{};
      std::atomic<bool>              pipeline_released{false};
    };

    auto task_context          = std::make_shared<ThumbnailTaskContext>();

    auto release_task_pipeline = [st, task_context]() {
      auto pipeline = task_context->pipeline;
      if (!pipeline || task_context->pipeline_released.exchange(true)) {
        return;
      }
      try {
        st->pipeline_service_->SavePipeline(pipeline);
      } catch (...) {
      }
    };

    auto fail_pending_request = [st, cache_key, task_context, release_task_pipeline](
                                    const std::string&                    message,
                                    const std::shared_ptr<PipelineGuard>& pipeline,
                                    bool                                  throw_after) -> bool {
      std::vector<State::PendingCallback> callbacks;
      {
        std::unique_lock lock(st->cache_lock_);
        auto             it = st->pending_.find(cache_key);
        if (it != st->pending_.end()) {
          callbacks = std::move(it->second);
          st->pending_.erase(it);
        }
        st->thumbnail_cache_.RemoveRecord(cache_key);
        st->thumbnail_cache_data_.erase(cache_key);
      }

      if (pipeline) {
        if (task_context->pipeline == pipeline) {
          release_task_pipeline();
        } else {
          try {
            st->pipeline_service_->SavePipeline(pipeline);
          } catch (...) {
          }
        }
      }

      for (const auto& pending_cb : callbacks) {
        DispatchThumbnailResultCallback(
            pending_cb.callback_, pending_cb.dispatcher_,
            ThumbnailRequestResult{.guard   = nullptr,
                                   .status  = ThumbnailRequestStatus::kError,
                                   .message = message,
                                   .key     = cache_key});
      }

      if (throw_after) {
        throw std::runtime_error(message);
      }
      return false;
    };

    const uint32_t  max_edge   = ResolutionToMaxEdge(resolution);
    const DecodeRes decode_res = ResolutionToDecodeRes(resolution);

    PipelineTask    thumb_task;
    thumb_task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
    thumb_task.options_.render_desc_.max_edge_    = max_edge;
    thumb_task.options_.render_desc_.decode_res_  = decode_res;
    thumb_task.options_.is_blocking_              = false;
    thumb_task.options_.is_callback_              = true;
    thumb_task.options_.is_seq_callback_          = false;
    thumb_task.cancel_requested_                  = [gen_token, expected_gen]() {
      return gen_token && gen_token->load() != expected_gen;
    };

    thumb_task.prepare_ = [st, id, image_id, task_context,
                           fail_pending_request](PipelineTask& task) mutable -> bool {
      std::shared_ptr<PipelineGuard> pipeline;
      try {
        pipeline = st->pipeline_service_->LoadPipeline(id);
      } catch (const std::exception& e) {
        return fail_pending_request(
            std::format("[ERROR] ThumbnailService: Failed to load pipeline for file ID {}: {}", id,
                        e.what()),
            nullptr, false);
      } catch (...) {
        return fail_pending_request(
            std::format(
                "[ERROR] ThumbnailService: Failed to load pipeline for file ID {}: unknown error.",
                id),
            nullptr, false);
      }

      task_context->pipeline = pipeline;
      if (!pipeline || !pipeline->pipeline_) {
        return fail_pending_request(
            std::format("[ERROR] ThumbnailService: Pipeline for file ID {} not available.", id),
            pipeline, false);
      }

      pipeline->pipeline_->SetForceCPUOutput(true);

      std::shared_ptr<Image> img_result;
      try {
        img_result = st->image_pool_service_->Read<std::shared_ptr<Image>>(
            image_id, [](const std::shared_ptr<Image>& img) { return img; });
      } catch (const std::exception& e) {
        return fail_pending_request(
            std::format("[ERROR] ThumbnailService: Failed to load image ID {} for element {}: {}",
                        image_id, id, e.what()),
            pipeline, false);
      } catch (...) {
        return fail_pending_request(std::format("[ERROR] ThumbnailService: Failed to load image ID "
                                                "{} for element {}: unknown error.",
                                                image_id, id),
                                    pipeline, false);
      }

      if (!img_result) {
        return fail_pending_request(
            std::format("[ERROR] ThumbnailService: Image with ID {} not found in pool.", image_id),
            pipeline, false);
      }

      task_context->pre_render_color_temp_params = ReadColorTempOperatorParams(pipeline);
      task.pipeline_executor_                    = pipeline->pipeline_;
      task.input_desc_                           = std::move(img_result);
      return true;
    };

    thumb_task.callback_ = [st, id, cache_key, task_context, release_task_pipeline, gen_token,
                            expected_gen](ImageBuffer& result_buffer) {
      // Strategy A: stale tasks must not touch pending_ because a newer request
      // for the same element/resolution may already have claimed that slot.
      if (gen_token && gen_token->load() != expected_gen) {
        release_task_pipeline();
        return;
      }

      std::shared_ptr<ThumbnailGuard>     guard;
      std::vector<State::PendingCallback> callbacks;
      std::unique_ptr<ImageBuffer>        display_buffer;
      try {
        if (IsRenderableThumbnailResult(result_buffer)) {
          display_buffer = MakeDisplayCacheThumbnailBuffer(result_buffer);
        }
      } catch (...) {
        display_buffer.reset();
      }

      {
        std::unique_lock lock(st->cache_lock_);

        if (gen_token && gen_token->load() != expected_gen) {
          release_task_pipeline();
          return;
        }

        auto       pending_it     = st->pending_.find(cache_key);
        const bool request_active = (pending_it != st->pending_.end());
        if (request_active) {
          callbacks = std::move(pending_it->second);
          st->pending_.erase(pending_it);
        }

        if (request_active && display_buffer) {
          guard                    = std::make_shared<ThumbnailGuard>();
          guard->thumbnail_buffer_ = std::move(display_buffer);
          guard->pin_count_        = static_cast<int>(std::max<size_t>(1, callbacks.size()));

          auto evicted = st->thumbnail_cache_.RecordAccess_WithEvict(cache_key, cache_key);
          HandleEvict(*st, evicted);
          st->thumbnail_cache_data_[cache_key] = guard;

          // Enqueue write to disk cache asynchronously.
          if (st->disk_cache_service_ && st->storage_) {
            try {
              const auto disk_key = st->BuildDiskCacheKey(id, cache_key.resolution,
                                                          ThumbnailDiskCachePurpose::kThumbnail);
              if (disk_key.has_value()) {
                std::shared_ptr<ImageBuffer> disk_cache_buffer(guard,
                                                               guard->thumbnail_buffer_.get());
                st->disk_cache_service_->EnqueueWrite(disk_key.value(),
                                                      std::move(disk_cache_buffer));
              }
            } catch (...) {
            }
          }
        } else {
          st->thumbnail_cache_.RemoveRecord(cache_key);
          st->thumbnail_cache_data_.erase(cache_key);
        }
      }

      // Strategy A: re-check token after pipeline work (before callbacks).
      // If cancelled mid-render, remove only the guard inserted by this task.
      if (gen_token && gen_token->load() != expected_gen) {
        if (guard) {
          std::unique_lock lock(st->cache_lock_);
          auto             guard_it = st->thumbnail_cache_data_.find(cache_key);
          if (guard_it != st->thumbnail_cache_data_.end() && guard_it->second == guard) {
            st->thumbnail_cache_.RemoveRecord(cache_key);
            st->thumbnail_cache_data_.erase(guard_it);
          }
        }
        release_task_pipeline();
        for (const auto& pending_cb : callbacks) {
          DispatchThumbnailResultCallback(
              pending_cb.callback_, pending_cb.dispatcher_,
              ThumbnailRequestResult{.guard   = nullptr,
                                     .status  = ThumbnailRequestStatus::kCanceled,
                                     .message = "Thumbnail request was canceled.",
                                     .key     = cache_key});
        }
        return;
      }

      const auto pipeline = task_context->pipeline;
      if (pipeline) {
        const auto post_render_color_temp_params = ReadColorTempOperatorParams(pipeline);
        if (post_render_color_temp_params != task_context->pre_render_color_temp_params) {
          pipeline->dirty_ = true;
        }
      }

      release_task_pipeline();

      const auto status = guard ? ThumbnailRequestStatus::kReady : ThumbnailRequestStatus::kError;
      const std::string message =
          guard
              ? std::string{}
              : std::format(
                    "[ERROR] ThumbnailService: Render for element {} produced no thumbnail buffer.",
                    id);
      for (const auto& pending_cb : callbacks) {
        DispatchThumbnailResultCallback(
            pending_cb.callback_, pending_cb.dispatcher_,
            ThumbnailRequestResult{
                .guard = guard, .status = status, .message = message, .key = cache_key});
      }
    };

    try {
      st->pipeline_scheduler_->ScheduleTask(std::move(thumb_task));
    } catch (const std::exception& e) {
      fail_pending_request(
          std::format("[ERROR] ThumbnailService: Failed to schedule thumbnail for element {}: {}",
                      id, e.what()),
          task_context->pipeline, true);
    } catch (...) {
      fail_pending_request(std::format("[ERROR] ThumbnailService: Failed to schedule thumbnail for "
                                       "element {}: unknown error.",
                                       id),
                           task_context->pipeline, true);
    }
  };

  if (disk_key.has_value() && st->disk_cache_service_) {
    const auto disk_key_value = disk_key.value();
    auto*      disk_cache     = st->disk_cache_service_.get();
    st->disk_read_thread_pool_.Submit([st, id, cache_key, disk_key_value, disk_cache, gen_token,
                                       expected_gen, schedule_pipeline_render]() {
      if (gen_token && gen_token->load() != expected_gen) {
        return;
      }

      auto disk_buffer = disk_cache->Read(disk_key_value);
      if (gen_token && gen_token->load() != expected_gen) {
        return;
      }

      if (!disk_buffer || !disk_buffer->cpu_data_valid_) {
        schedule_pipeline_render();
        return;
      }

      auto display_buffer = MakeDisplayCacheThumbnailBuffer(*disk_buffer);
      if (!display_buffer) {
        schedule_pipeline_render();
        return;
      }

      std::shared_ptr<ThumbnailGuard>     disk_guard;
      std::vector<State::PendingCallback> callbacks;
      {
        std::unique_lock lock(st->cache_lock_);
        if (gen_token && gen_token->load() != expected_gen) {
          return;
        }
        auto pending_it = st->pending_.find(cache_key);
        if (pending_it == st->pending_.end()) {
          return;
        }
        callbacks = std::move(pending_it->second);
        st->pending_.erase(pending_it);

        disk_guard                    = std::make_shared<ThumbnailGuard>();
        disk_guard->thumbnail_buffer_ = std::move(display_buffer);
        disk_guard->pin_count_        = static_cast<int>(std::max<size_t>(1, callbacks.size()));
        auto evicted = st->thumbnail_cache_.RecordAccess_WithEvict(cache_key, cache_key);
        HandleEvict(*st, evicted);
        st->thumbnail_cache_data_[cache_key] = disk_guard;
      }

      for (const auto& pending_cb : callbacks) {
        DispatchThumbnailResultCallback(
            pending_cb.callback_, pending_cb.dispatcher_,
            ThumbnailRequestResult{.guard   = disk_guard,
                                   .status  = ThumbnailRequestStatus::kReady,
                                   .message = {},
                                   .key     = cache_key});
      }
    });
    return;
  }

  schedule_pipeline_render();
}

void ThumbnailService::RequestAnalysisRendition(sl_element_id_t element_id, image_id_t image_id,
                                                ThumbnailResolution     resolution,
                                                ThumbnailResultCallback callback) {
  auto st = state_;
  if (!st || !st->image_pool_service_ || !st->pipeline_service_ || !st->pipeline_scheduler_) {
    throw std::runtime_error("[ERROR] ThumbnailService: Services not initialized.");
  }

  const ThumbnailCacheKey cache_key{element_id, resolution};
  const auto              disk_key =
      st->BuildDiskCacheKey(element_id, resolution, ThumbnailDiskCachePurpose::kAnalysis);

  std::shared_ptr<std::atomic<uint64_t>> gen_token;
  uint64_t                               expected_gen = 0;
  {
    std::unique_lock<std::mutex> lk(st->cache_lock_);
    gen_token    = st->GetOrCreateAnalysisToken(cache_key);
    expected_gen = gen_token->load();
  }

  auto delivered = std::make_shared<std::atomic<bool>>(false);
  auto deliver   = [callback, delivered](ThumbnailRequestResult result) {
    if (delivered->exchange(true)) {
      return;
    }
    DispatchThumbnailResultCallback(callback, nullptr, std::move(result));
  };

  auto schedule_snapshot_render = [st, element_id, image_id, resolution, cache_key, disk_key,
                                   gen_token, expected_gen, deliver]() mutable {
    if (gen_token && gen_token->load() != expected_gen) {
      deliver(ThumbnailRequestResult{.guard   = nullptr,
                                     .status  = ThumbnailRequestStatus::kCanceled,
                                     .message = "Analysis rendition was canceled.",
                                     .key     = cache_key});
      return;
    }

    std::string err;
    auto        snapshot = st->pipeline_service_->LoadPipelineSnapshot(element_id, image_id, &err);
    if (!snapshot) {
      deliver(ThumbnailRequestResult{.guard   = nullptr,
                                     .status  = ThumbnailRequestStatus::kError,
                                     .message = err,
                                     .key     = cache_key});
      return;
    }

    // The closure holds the only strong ref to the snapshot. No
    // analysis_snapshots_ map is needed: the snapshot is released from one
    // of the task callbacks and the shared_ptr then drops naturally.
    struct AnalysisTaskContext {
      std::shared_ptr<PipelineSnapshot> snapshot;
      std::atomic<bool>                 released{false};
    };
    auto ctx              = std::make_shared<AnalysisTaskContext>();
    ctx->snapshot         = std::move(snapshot);

    auto release_snapshot = [st, ctx]() {
      if (!ctx->snapshot || ctx->released.exchange(true)) {
        return;
      }
      try {
        st->pipeline_service_->ReleasePipelineSnapshot(ctx->snapshot);
      } catch (...) {
      }
    };

    auto fail = [release_snapshot, deliver, cache_key](const std::string& msg) -> bool {
      release_snapshot();
      deliver(ThumbnailRequestResult{.guard   = nullptr,
                                     .status  = ThumbnailRequestStatus::kError,
                                     .message = msg,
                                     .key     = cache_key});
      return false;
    };

    const uint32_t  max_edge   = ResolutionToMaxEdge(resolution);
    const DecodeRes decode_res = ResolutionToDecodeRes(resolution);

    PipelineTask    task;
    task.options_.render_desc_.render_type_ = RenderType::THUMBNAIL;
    task.options_.render_desc_.max_edge_    = max_edge;
    task.options_.render_desc_.decode_res_  = decode_res;
    task.options_.is_blocking_              = false;
    task.options_.is_callback_              = true;
    task.options_.is_seq_callback_          = false;
    task.cancel_requested_                  = [gen_token, expected_gen]() {
      return gen_token && gen_token->load() != expected_gen;
    };

    task.prepare_ = [st, element_id, image_id, ctx, cache_key, gen_token, expected_gen, deliver,
                     fail](PipelineTask& t) mutable -> bool {
      // Early cancel: avoid a wasted one-shot render. The scheduler will not
      // call callback_ when prepare_ returns false, so dispatch kCanceled here.
      if (gen_token && gen_token->load() != expected_gen) {
        if (ctx->snapshot && !ctx->released.exchange(true)) {
          try {
            st->pipeline_service_->ReleasePipelineSnapshot(ctx->snapshot);
          } catch (...) {
          }
        }
        deliver(ThumbnailRequestResult{.guard   = nullptr,
                                       .status  = ThumbnailRequestStatus::kCanceled,
                                       .message = "Analysis rendition was canceled.",
                                       .key     = cache_key});
        return false;
      }

      auto exec = ctx->snapshot->executor_;
      if (!exec) {
        return fail("analysis rendition: snapshot executor missing");
      }

      exec->SetForceCPUOutput(true);

      std::shared_ptr<Image> img_result;
      try {
        img_result = st->image_pool_service_->Read<std::shared_ptr<Image>>(
            image_id, [](const std::shared_ptr<Image>& img) { return img; });
      } catch (const std::exception& e) {
        return fail(std::format("analysis rendition: failed to load image ID {} for element {}: {}",
                                image_id, element_id, e.what()));
      } catch (...) {
        return fail(std::format(
            "analysis rendition: failed to load image ID {} for element {}: unknown error.",
            image_id, element_id));
      }
      if (!img_result) {
        return fail(
            std::format("analysis rendition: image with ID {} not found in pool.", image_id));
      }

      // No pre-render color-temp read / post-render dirty check: the snapshot
      // executor is throwaway, so that live-path dirty check is meaningless.
      t.pipeline_executor_ = exec;
      t.input_desc_        = std::move(img_result);
      return true;
    };

    task.callback_ = [st, element_id, cache_key, disk_key, gen_token, expected_gen,
                      release_snapshot, deliver](ImageBuffer& result_buffer) {
      if (gen_token && gen_token->load() != expected_gen) {
        release_snapshot();
        deliver(ThumbnailRequestResult{.guard   = nullptr,
                                       .status  = ThumbnailRequestStatus::kCanceled,
                                       .message = "Analysis rendition was canceled.",
                                       .key     = cache_key});
        return;
      }

      std::unique_ptr<ImageBuffer> display_buffer;
      try {
        if (IsRenderableThumbnailResult(result_buffer)) {
          display_buffer = MakeDisplayCacheThumbnailBuffer(result_buffer);
        }
      } catch (...) {
        display_buffer.reset();
      }

      auto guard = std::make_shared<ThumbnailGuard>();
      if (display_buffer) {
        guard->thumbnail_buffer_ = std::move(display_buffer);
        guard->pin_count_        = 1;
        if (disk_key.has_value() && st->disk_cache_service_) {
          std::shared_ptr<ImageBuffer> disk_cache_buffer(guard, guard->thumbnail_buffer_.get());
          st->disk_cache_service_->EnqueueWrite(disk_key.value(), std::move(disk_cache_buffer));
        }
      }

      release_snapshot();

      const bool ok     = static_cast<bool>(guard->thumbnail_buffer_);
      const auto status = ok ? ThumbnailRequestStatus::kReady : ThumbnailRequestStatus::kError;
      const std::string message =
          ok ? std::string{}
             : std::format(
                   "analysis rendition: render for element {} produced no thumbnail buffer.",
                   element_id);
      deliver(ThumbnailRequestResult{
          .guard = ok ? guard : nullptr, .status = status, .message = message, .key = cache_key});
    };

    try {
      st->pipeline_scheduler_->ScheduleTask(std::move(task));
    } catch (const std::exception& e) {
      release_snapshot();
      deliver(ThumbnailRequestResult{
          .guard   = nullptr,
          .status  = ThumbnailRequestStatus::kError,
          .message = std::format("analysis rendition: failed to schedule: {}", e.what()),
          .key     = cache_key});
    } catch (...) {
      release_snapshot();
      deliver(ThumbnailRequestResult{
          .guard   = nullptr,
          .status  = ThumbnailRequestStatus::kError,
          .message = "analysis rendition: failed to schedule: unknown error.",
          .key     = cache_key});
    }
  };

  if (disk_key.has_value() && st->disk_cache_service_) {
    const auto disk_key_value = disk_key.value();
    auto*      disk_cache     = st->disk_cache_service_.get();
    st->disk_read_thread_pool_.Submit([cache_key, disk_key_value, disk_cache, gen_token,
                                       expected_gen, deliver, schedule_snapshot_render]() mutable {
      if (gen_token && gen_token->load() != expected_gen) {
        deliver(ThumbnailRequestResult{.guard   = nullptr,
                                       .status  = ThumbnailRequestStatus::kCanceled,
                                       .message = "Analysis rendition was canceled.",
                                       .key     = cache_key});
        return;
      }

      auto disk_buffer = disk_cache->Read(disk_key_value);
      if (gen_token && gen_token->load() != expected_gen) {
        deliver(ThumbnailRequestResult{.guard   = nullptr,
                                       .status  = ThumbnailRequestStatus::kCanceled,
                                       .message = "Analysis rendition was canceled.",
                                       .key     = cache_key});
        return;
      }

      if (!disk_buffer || !disk_buffer->cpu_data_valid_) {
        schedule_snapshot_render();
        return;
      }

      auto display_buffer = MakeDisplayCacheThumbnailBuffer(*disk_buffer);
      if (!display_buffer) {
        schedule_snapshot_render();
        return;
      }

      auto guard               = std::make_shared<ThumbnailGuard>();
      guard->thumbnail_buffer_ = std::move(display_buffer);
      guard->pin_count_        = 1;
      deliver(ThumbnailRequestResult{.guard   = guard,
                                     .status  = ThumbnailRequestStatus::kReady,
                                     .message = {},
                                     .key     = cache_key});
    });
    return;
  }

  schedule_snapshot_render();
}

void ThumbnailService::CancelAnalysisRendition(const ThumbnailCacheKey& key) {
  auto st = state_;
  if (!st) {
    return;
  }
  st->IncrementAnalysisToken(key);
}

void ThumbnailService::ReleaseAnalysisRendition(const ThumbnailCacheKey& key) {
  auto st = state_;
  if (!st) {
    return;
  }
  std::unique_lock<std::mutex> lk(st->cache_lock_);
  st->analysis_tokens_.erase(key);
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
};  // namespace alcedo
