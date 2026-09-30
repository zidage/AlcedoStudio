//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/ai_credential_store.hpp"
#include "app/ai_sidecar_runtime_service.hpp"
#include "app/thumbnail_service.hpp"
#include "app/thumbnail_types.hpp"
#include "sidecar_client/dto/image_analysis.hpp"
#include "type/type.hpp"

namespace alcedo {

struct ImageAnalysisItem {
  sl_element_id_t element_id = 0;
  image_id_t      image_id   = 0;
  std::string     camera_context;
};

enum class ImageAnalysisTask : uint8_t {
  kDescribe = 0,
  kScore,
  kAnalyze,
};

enum class ImageAnalysisItemStatus : uint8_t {
  kPending = 0,
  kThumbnailReady,
  kAnalyzed,
  kCanceled,
  kError,
};

// Long-lived provider credential for one analysis run. The `secret` is consumed once
// (registered with the sidecar vault to obtain an opaque handle) and then cleared from
// the options copy inside RunJob; it never enters ImageAnalysisRequest, result DTOs,
// AiSidecarRuntimeOptions, process args, or logs.
struct ImageAnalysisCredential {
  std::string provider_id;
  std::string secret;
};

inline constexpr int kImageAnalysisBatchSize = 3;
// Phase 5e prefill queue depth upper bound. Analyze runs consume 3 encoded items per
// provider call, so the default/cap leave room for one in-flight batch plus the next
// prefilled batch. Each queued entry is one encoded k1024 JPEG buffer.
inline constexpr int kMaxImageAnalysisPrefetch = kImageAnalysisBatchSize * 2;

struct ImageAnalysisOptions {
  ImageAnalysisTask         task                 = ImageAnalysisTask::kDescribe;
  ThumbnailResolution       thumbnail_resolution = ThumbnailResolution::k1024;
  int                       jpeg_quality         = 90;
  std::chrono::milliseconds timeout{60000};
  std::string               provider_id;  // "" = sidecar default
  std::string               model_id;     // "" = provider default
  std::string               prompt_profile_id;
  std::string               rubric_id;  // ScoreImage only; "" = provider default
  // Host-resolved output language for generated caption/reasons ("" or "en" =
  // English; "zh" = Simplified Chinese). Set from the active profile's
  // `output_language` ("follow" resolves to the current app language here).
  std::string               output_language;
  // Rating strictness persona for ScoreImage only: "" or "normal" = the default
  // balanced rubric; "lite" = generous; "high" = master-guided critique;
  // "xhigh" = 老法师; "max" = exacting 懂哥 connoisseur.
  // Selects the rating system prompt in the driver; ignored by DescribeImage.
  std::string               rating_severity;
  // Rating rationale is optional UI-selected content under the rating task.
  // Rating itself may still be requested without persisting or generating reasons.
  bool                      include_rating_reasons = true;
  ImageAnalysisCredential   credential;
  std::filesystem::path     temp_dir;  // empty => std::filesystem::temp_directory_path()
  int64_t                   credential_ttl_ms = 0;  // 0 => sidecar default
  // Phase 5e prefill queue depth: the maximum number of encoded JPEG renditions
  // buffered ahead of the single in-flight remote call. Analyze consumes up to
  // kImageAnalysisBatchSize items per RPC; the default keeps the next batch warm
  // while the current batch is in flight. Clamped in RunJob.
  int                       prefetch          = kMaxImageAnalysisPrefetch;
  // Phase 6d: optional cap on the encoded-rendition byte size, sourced from the
  // selected profile's `max_image_bytes`. 0 = no cap. When > 0, RunJob marks an
  // item as a prep failure (no provider call, no pin held) if the encoded JPEG
  // exceeds this limit — fail-closed so a profile cannot push an oversized payload
  // to a paid provider call.
  int64_t                   max_image_bytes   = 0;
};

struct ImageAnalysisConnectionValidationOptions {
  std::string               provider_id;
  std::string               credential_slot;
  bool                      requires_credential = true;
  std::chrono::milliseconds timeout{60000};
  int64_t                   credential_ttl_ms = 60000;
};

struct ImageAnalysisConnectionValidationResult {
  bool                           ok = false;
  std::string                    error;
  bool                           credential_revoked = false;
  ImageAnalysisListModelsResult  list_models;
  std::vector<AiDiscoveredModel> models;
};

struct ImageAnalysisProgress {
  size_t total    = 0;
  size_t analyzed = 0;
  size_t failed   = 0;
  size_t canceled = 0;
};

struct ImageAnalysisItemResult {
  ImageAnalysisItem                item{};
  std::string                      request_id;
  ImageAnalysisItemStatus          status = ImageAnalysisItemStatus::kPending;
  std::string                      error;
  ImageAnalysisUnderstandingResult understanding;  // filled when task == kDescribe
  ImageAnalysisRatingResult        rating;         // filled when task == kScore
  ImageAnalysisRendition           rendition;      // the rendition actually analyzed
};

using ImageAnalysisProgressCallback  = std::function<void(const ImageAnalysisProgress&)>;
using ImageAnalysisFinishedCallback  = std::function<void(std::vector<ImageAnalysisItemResult>)>;

// Serializes remote image-analysis calls to at most one in flight across ALL
// ImageAnalysisService instances that share the same concurrency limit. Phase 5d mandates a
// host-boundary in-flight limit of one (provider calls are non-idempotent / paid).
// Injectable so the album backend (Phase 6) can share one concurrency limit app-wide even if the
// service is constructed per-use; if none is passed the service creates a private one.
class ImageAnalysisConcurrencyLimit {
 public:
  ImageAnalysisConcurrencyLimit() = default;

  // Blocks until the slot is free or `is_canceled()` returns true. On success the slot
  // is acquired AND `request_id` is published under the same lock, so an observer can
  // never see the slot held with no published id. Returns true if the slot was acquired
  // (id published), false if the wait was canceled (the slot is NOT acquired, id NOT
  // published). Atomic acquire+publish closes the cancel race where ImageAnalysisJob::
  // Cancel could otherwise observe a held slot with an empty id (skip CancelTask) while
  // the worker was about to issue the paid provider RPC.
  auto AcquireAndPublish(const std::string& request_id, std::function<bool()> is_canceled) -> bool;
  void Release();
  // Clears the request_id of the job currently occupying the slot. Paired with
  // AcquireAndPublish; read by ImageAnalysisJob::Cancel to decide whether to best-effort
  // CancelTask this job's in-flight RPC. While the slot is held the id is always
  // non-empty (AcquireAndPublish publishes atomically), so Cancel's id check agrees with
  // its am_in_flight_ check.
  void ClearRequestId();
  auto CurrentRequestId() const -> std::string;
  // Wakes any waiter blocked in AcquireAndPublish (called by ImageAnalysisJob::Cancel).
  void NotifyAll();

 private:
  mutable std::mutex      mutex_;
  std::condition_variable cv_;
  bool                    in_flight_ = false;
  std::string             in_flight_request_id_;
};

// Sidecar-call seam for ImageAnalysisService (mirrors ISemanticImageEmbeddingClient).
// Adding Ready / RegisterCredential / CancelTask beyond the typed RPCs keeps the
// service's credential + server-cancel concerns behind one testable interface.
class IImageAnalysisClient {
 public:
  virtual ~IImageAnalysisClient()                                                  = default;

  virtual auto Ready() -> bool                                                     = 0;
  virtual auto RegisterCredential(const std::string& provider_id, const std::string& secret,
                                  int64_t ttl_ms, std::chrono::milliseconds timeout,
                                  std::string* handle, std::string* error) -> bool = 0;
  virtual auto RevokeCredential(const std::string& handle, std::chrono::milliseconds timeout,
                                bool* revoked, std::string* error) -> bool         = 0;
  virtual auto DescribeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisUnderstandingResult = 0;
  virtual auto ScoreImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisRatingResult = 0;
  virtual auto AnalyzeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisCombinedResult                                                          = 0;
  virtual auto BatchAnalyzeImage(const std::vector<ImageAnalysisRequest>& requests,
                                 std::chrono::milliseconds timeout)
      -> std::vector<ImageAnalysisCombinedResult>                                              = 0;
  // Phase 6c: dry-run model discovery (validate-connection flow). `provider_id`
  // selects the configured endpoint ("" = sidecar default); `credential_ref` is
  // the opaque vault handle. Returns unverified candidates; no persistence.
  virtual auto ListModels(const std::string& provider_id, const std::string& credential_ref,
                          std::chrono::milliseconds timeout) -> ImageAnalysisListModelsResult = 0;
  virtual auto CancelTask(const std::string& request_id, std::chrono::milliseconds timeout,
                          bool* cancelled, std::string* error) -> bool                        = 0;
};

class AiSidecarRuntimeImageAnalysisClient final : public IImageAnalysisClient {
 public:
  explicit AiSidecarRuntimeImageAnalysisClient(std::shared_ptr<AiSidecarRuntimeService> runtime);

  auto Ready() -> bool override;
  auto RegisterCredential(const std::string& provider_id, const std::string& secret, int64_t ttl_ms,
                          std::chrono::milliseconds timeout, std::string* handle,
                          std::string* error) -> bool override;
  auto RevokeCredential(const std::string& handle, std::chrono::milliseconds timeout, bool* revoked,
                        std::string* error) -> bool override;
  auto DescribeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisUnderstandingResult override;
  auto ScoreImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisRatingResult override;
  auto AnalyzeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds timeout)
      -> ImageAnalysisCombinedResult override;
  auto BatchAnalyzeImage(const std::vector<ImageAnalysisRequest>& requests,
                         std::chrono::milliseconds timeout)
      -> std::vector<ImageAnalysisCombinedResult> override;
  auto ListModels(const std::string& provider_id, const std::string& credential_ref,
                  std::chrono::milliseconds timeout) -> ImageAnalysisListModelsResult override;
  auto CancelTask(const std::string& request_id, std::chrono::milliseconds timeout, bool* cancelled,
                  std::string* error) -> bool override;

 private:
  std::shared_ptr<AiSidecarRuntimeService> runtime_;
};

class ImageAnalysisJob final {
 public:
  ImageAnalysisJob() = default;
  ~ImageAnalysisJob();

  ImageAnalysisJob(const ImageAnalysisJob&)            = delete;
  ImageAnalysisJob& operator=(const ImageAnalysisJob&) = delete;

  // Sets the cooperative cancel flag, wakes any queued wait on the in-flight concurrency limit, and
  // best-effort calls CancelTask on this job's in-flight RPC (if any). The correctness
  // guarantee is the post-RPC discard in RunJob, not CancelTask: a long provider call
  // may still complete and its result is dropped.
  void              Cancel();
  auto              IsCanceled() const -> bool;
  void              Wait();
  auto              SnapshotProgress() const -> ImageAnalysisProgress;
  auto              Results() const -> std::vector<ImageAnalysisItemResult>;

 private:
  friend class ImageAnalysisService;

  void               UpdateProgress(const std::function<void(ImageAnalysisProgress&)>& updater);
  void               AppendResult(ImageAnalysisItemResult result);
  void               SetWorkerThread(std::thread worker);
  void               Finish();
  void SetConcurrencyLimit(std::shared_ptr<ImageAnalysisConcurrencyLimit> concurrency_limit);
  void               SetClient(std::shared_ptr<IImageAnalysisClient> client);

  mutable std::mutex lock_;
  std::condition_variable                    finished_cv_;
  ImageAnalysisProgress                      progress_{};
  std::vector<ImageAnalysisItemResult>       results_;
  std::atomic<bool>                          canceled_{false};
  std::atomic<bool>                          am_in_flight_{false};
  std::thread                                worker_;
  std::thread                                producer_;  // Phase 5e prefill producer
  bool                                       finished_ = false;

  std::shared_ptr<ImageAnalysisConcurrencyLimit> concurrency_limit_;
  std::shared_ptr<IImageAnalysisClient>      client_;
};

class ImageAnalysisService final {
 public:
  ImageAnalysisService(std::shared_ptr<IAnalysisRenditionProvider>    thumbnail_provider,
                       std::shared_ptr<IImageAnalysisClient>          analysis_client,
                       std::shared_ptr<ImageAnalysisConcurrencyLimit> concurrency_limit = nullptr);

  auto StartAnalysis(std::vector<ImageAnalysisItem> items, ImageAnalysisOptions options = {},
                     ImageAnalysisProgressCallback on_progress = {},
                     ImageAnalysisFinishedCallback on_finished = {})
      -> std::shared_ptr<ImageAnalysisJob>;
  auto ValidateConnection(const ImageAnalysisConnectionValidationOptions& options,
                          IAiCredentialStore*                             credential_store)
      -> ImageAnalysisConnectionValidationResult;

 private:
  static void                                 RunJob(const std::shared_ptr<ImageAnalysisJob>& job,
                                                     const std::vector<ImageAnalysisItem>& items, ImageAnalysisOptions options,
                                                     ImageAnalysisProgressCallback                  on_progress,
                                                     ImageAnalysisFinishedCallback                  on_finished,
                                                     std::shared_ptr<IAnalysisRenditionProvider>    thumbnail_provider,
                                                     std::shared_ptr<IImageAnalysisClient>          analysis_client,
                                                     std::shared_ptr<ImageAnalysisConcurrencyLimit> concurrency_limit);

  std::shared_ptr<IAnalysisRenditionProvider> thumbnail_provider_;
  std::shared_ptr<IImageAnalysisClient>       analysis_client_;
  std::shared_ptr<ImageAnalysisConcurrencyLimit> concurrency_limit_;
};

auto ToString(ImageAnalysisItemStatus status) -> const char*;

}  // namespace alcedo
