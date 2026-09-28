//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/image_analysis_service.hpp"

#include "app/image_analysis_encoder.hpp"

#include <algorithm>
#include <deque>
#include <exception>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <utility>

#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {

// Generic RAII scope-exit: invokes `fn` on destruction. Used to guarantee the
// in-flight gate is released (and am_in_flight_ / the published request_id are
// cleared) even if a provider RPC throws between Acquire and Release.
template <typename F>
class ScopeExit {
 public:
  explicit ScopeExit(F fn) : fn_(std::move(fn)) {}
  ~ScopeExit() {
    if (engaged_) {
      fn_();
    }
  }
  ScopeExit(const ScopeExit&)            = delete;
  ScopeExit& operator=(const ScopeExit&) = delete;

 private:
  F    fn_;
  bool engaged_ = true;
};
template <typename F>
ScopeExit(F) -> ScopeExit<F>;

auto ProviderFailureMessage(int status, int error_code, const std::string& provider,
                            const std::string& model_id) -> std::string {
  std::string message = "provider call failed without an error message";
  if (status != 0) {
    message += " (status " + std::to_string(status);
    if (error_code != 0) {
      message += ", error code " + std::to_string(error_code);
    }
    message += ")";
  } else if (error_code != 0) {
    message += " (error code " + std::to_string(error_code) + ")";
  }
  if (!provider.empty()) {
    message += "; provider=" + provider;
  }
  if (!model_id.empty()) {
    message += "; model=" + model_id;
  }
  return message;
}
auto MakeRequestId(const ImageAnalysisItem& item, ImageAnalysisTask task) -> std::string {
  const char* task_name = "describe-";
  if (task == ImageAnalysisTask::kScore) {
    task_name = "score-";
  } else if (task == ImageAnalysisTask::kAnalyze) {
    task_name = "analyze-";
  }
  return std::string("image-analysis-") + task_name +
         std::to_string(item.element_id) + "-" + std::to_string(item.image_id);
}

struct ThumbnailWaitState {
  std::mutex             mutex;
  std::condition_variable cv;
  ThumbnailRequestResult result;
  bool                   done      = false;
  bool                   abandoned = false;
};

// Requests one thumbnail and blocks (cooperatively, 25ms poll) until it returns or the
// job is canceled. Mirrors WaitForThumbnailBatch in semantic_generation_service.cpp but
// for a single item, since image analysis is serialized one image at a time.
auto WaitForOneThumbnail(const std::shared_ptr<ImageAnalysisJob>&           job,
                         const std::shared_ptr<IAnalysisRenditionProvider>& provider,
                         const ImageAnalysisItem& item, ThumbnailResolution resolution)
    -> ThumbnailRequestResult {
  auto state = std::make_shared<ThumbnailWaitState>();

  try {
    provider->RequestRendition(item.element_id, item.image_id, resolution,
                               [state, provider](ThumbnailRequestResult result) {
                                 bool               release_late = false;
                                 ThumbnailCacheKey  late_key{};
                                 {
                                   std::unique_lock lk(state->mutex);
                                   release_late = state->abandoned && result.guard != nullptr;
                                   late_key     = result.key;
                                   if (!state->abandoned && !state->done) {
                                     state->result = std::move(result);
                                     state->done   = true;
                                   }
                                 }
                                 if (release_late) {
                                   provider->ReleaseRendition(late_key);
                                 }
                                 state->cv.notify_all();
                               });
  } catch (const std::exception& e) {
    ThumbnailRequestResult r;
    r.key     = ThumbnailCacheKey{item.element_id, resolution};
    r.status  = ThumbnailRequestStatus::kError;
    r.message = std::string("thumbnail request failed: ") + e.what();
    return r;
  } catch (...) {
    ThumbnailRequestResult r;
    r.key     = ThumbnailCacheKey{item.element_id, resolution};
    r.status  = ThumbnailRequestStatus::kError;
    r.message = "thumbnail request failed";
    return r;
  }

  {
    std::unique_lock lk(state->mutex);
    while (!state->done) {
      if (job->IsCanceled()) {
        state->abandoned = true;
        lk.unlock();
        provider->CancelRendition(ThumbnailCacheKey{item.element_id, resolution});
        ThumbnailRequestResult r;
        r.key     = ThumbnailCacheKey{item.element_id, resolution};
        r.status  = ThumbnailRequestStatus::kCanceled;
        r.message = "image analysis job was canceled";
        return r;
      }
      state->cv.wait_for(lk, std::chrono::milliseconds(25));
    }
  }
  return std::move(state->result);
}

// Phase 5e: one prepared, self-contained work unit handed from the producer to the
// consumer across the bounded ready queue. It carries encoded bytes + rendition
// metadata + item/request identity + the (non-secret) request fields the consumer
// needs to build the typed RPC — it never carries a ThumbnailGuard / ImageBuffer pin
// (the producer releases the pin before pushing). `credential_ref` is the opaque vault
// handle, never key material.
enum class EncodedItemKind : uint8_t { kEncoded = 0, kPrepFailed };

struct EncodedAnalysisItem {
  EncodedItemKind          kind = EncodedItemKind::kEncoded;
  ImageAnalysisItem        item{};
  std::string              request_id;
  std::vector<uint8_t>     bytes;           // kEncoded: encoded JPEG bytes
  std::string              image_format_hint;  // kEncoded: "image/jpeg;max_edge=<N>"
  ImageAnalysisRendition   rendition;       // kEncoded: records what was actually encoded
  std::string              provider_id;     // request identity (non-secret)
  std::string              model_id;
  std::string              prompt_profile_id;
  std::string              rubric_id;
  std::string              output_language;  // host-resolved; "" or "en" or "zh"
  std::string              rating_severity;  // host-resolved; "" or one known severity code
  std::string              camera_context;   // optional gear-sensitive EXIF/camera context
  std::string              credential_ref;  // opaque vault handle
  std::string              error;           // kPrepFailed: thumbnail/encode error message
};

// Bounded ready queue (Phase 5e prefill). The producer pushes prepared encoded items;
// the consumer pops them in FIFO order and runs the single in-flight remote call. The
// bound caps in-memory JPEG buffers at `capacity` (plus the one the consumer holds in
// flight), so a large album cannot accumulate unbounded encoded bytes. Waits use a 25ms
// timed poll so a cancel (which only flips the job flag; the queue is not a job member)
// is observed without an explicit notify — matching WaitForOneThumbnail's discipline.
// Notify-on-release / notify-on-done keep the normal (non-cancel) path prompt.
class PrefillQueue {
 public:
  explicit PrefillQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

  auto Push(EncodedAnalysisItem item, std::function<bool()> is_canceled) -> bool {
    std::unique_lock lk(mutex_);
    while (items_.size() >= capacity_ && !is_canceled()) {
      not_full_cv_.wait_for(lk, std::chrono::milliseconds(25));
    }
    if (is_canceled()) {
      return false;  // item NOT pushed; caller should stop producing
    }
    items_.push_back(std::move(item));
    not_empty_cv_.notify_one();
    return true;
  }

  auto Pop(std::function<bool()> is_canceled) -> std::optional<EncodedAnalysisItem> {
    std::unique_lock lk(mutex_);
    while (items_.empty() && !producer_done_ && !is_canceled()) {
      not_empty_cv_.wait_for(lk, std::chrono::milliseconds(25));
    }
    if (is_canceled()) {
      return std::nullopt;
    }
    if (items_.empty()) {
      return std::nullopt;  // producer_done_ && empty => end of stream
    }
    auto item = std::move(items_.front());
    items_.pop_front();
    not_full_cv_.notify_one();
    return item;
  }

  auto PopBatch(size_t max_items, std::function<bool()> is_canceled)
      -> std::vector<EncodedAnalysisItem> {
    std::vector<EncodedAnalysisItem> batch;
    if (max_items == 0) {
      return batch;
    }
    std::unique_lock lk(mutex_);
    while (items_.empty() && !producer_done_ && !is_canceled()) {
      not_empty_cv_.wait_for(lk, std::chrono::milliseconds(25));
    }
    while (items_.size() < max_items && !producer_done_ && !is_canceled()) {
      not_empty_cv_.wait_for(lk, std::chrono::milliseconds(25));
    }
    if (is_canceled() || items_.empty()) {
      return batch;
    }
    const auto count = std::min(max_items, items_.size());
    batch.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      batch.push_back(std::move(items_.front()));
      items_.pop_front();
    }
    not_full_cv_.notify_all();
    return batch;
  }

  void MarkProducerDone() {
    {
      std::unique_lock lk(mutex_);
      producer_done_ = true;
    }
    not_empty_cv_.notify_all();
  }

 private:
  mutable std::mutex             mutex_;
  std::condition_variable        not_full_cv_;
  std::condition_variable        not_empty_cv_;
  std::deque<EncodedAnalysisItem> items_;
  size_t                         capacity_;
  bool                           producer_done_ = false;
};

auto MakeRequestFromEncoded(EncodedAnalysisItem e, const ImageAnalysisOptions& options)
    -> ImageAnalysisRequest {
  ImageAnalysisRequest req;
  req.request_id            = std::move(e.request_id);
  req.image_bytes           = std::move(e.bytes);
  req.image_format_hint     = std::move(e.image_format_hint);
  req.rendition             = e.rendition;
  req.provider_id           = std::move(e.provider_id);
  req.model_id              = std::move(e.model_id);
  req.prompt_profile_id     = std::move(e.prompt_profile_id);
  req.credential_ref        = std::move(e.credential_ref);
  req.rubric_id             = std::move(e.rubric_id);
  req.output_language       = std::move(e.output_language);
  req.rating_severity       = std::move(e.rating_severity);
  req.camera_context        = std::move(e.camera_context);
  req.include_understanding = options.task != ImageAnalysisTask::kScore;
  req.include_rating        = options.task != ImageAnalysisTask::kDescribe;
  req.include_rating_reasons = req.include_rating && options.include_rating_reasons;
  return req;
}

}  // namespace

auto ToString(ImageAnalysisItemStatus status) -> const char* {
  switch (status) {
    case ImageAnalysisItemStatus::kPending:
      return "pending";
    case ImageAnalysisItemStatus::kThumbnailReady:
      return "thumbnail_ready";
    case ImageAnalysisItemStatus::kAnalyzed:
      return "analyzed";
    case ImageAnalysisItemStatus::kCanceled:
      return "canceled";
    case ImageAnalysisItemStatus::kError:
      return "error";
  }
  return "unknown";
}

// --- ImageAnalysisInFlightGate ---

auto ImageAnalysisInFlightGate::AcquireAndPublish(const std::string& request_id,
                                                  std::function<bool()> is_canceled) -> bool {
  std::unique_lock lk(mutex_);
  cv_.wait(lk, [&] { return !in_flight_ || is_canceled(); });
  if (is_canceled()) {
    return false;
  }
  in_flight_ = true;
  in_flight_request_id_ = request_id;  // published atomically with in_flight_
  return true;
}

void ImageAnalysisInFlightGate::Release() {
  {
    std::unique_lock lk(mutex_);
    in_flight_ = false;
  }
  cv_.notify_all();
}

void ImageAnalysisInFlightGate::ClearRequestId() {
  std::unique_lock lk(mutex_);
  in_flight_request_id_.clear();
}

auto ImageAnalysisInFlightGate::CurrentRequestId() const -> std::string {
  std::unique_lock lk(mutex_);
  return in_flight_request_id_;
}

void ImageAnalysisInFlightGate::NotifyAll() { cv_.notify_all(); }

// --- AiSidecarRuntimeImageAnalysisClient ---

AiSidecarRuntimeImageAnalysisClient::AiSidecarRuntimeImageAnalysisClient(
    std::shared_ptr<AiSidecarRuntimeService> runtime)
    : runtime_(std::move(runtime)) {}

auto AiSidecarRuntimeImageAnalysisClient::Ready() -> bool {
  return runtime_ && runtime_->Status().state == AiSidecarRuntimeState::kReady &&
         runtime_->ClientSession() != nullptr;
}

auto AiSidecarRuntimeImageAnalysisClient::RegisterCredential(
    const std::string& provider_id, const std::string& secret, int64_t ttl_ms,
    std::chrono::milliseconds timeout, std::string* handle, std::string* error) -> bool {
  if (!runtime_) {
    if (error) *error = "ai sidecar runtime is not available";
    return false;
  }
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    if (error) *error = "ai sidecar runtime is not ready";
    return false;
  }
  return session->credentials().RegisterCredential(provider_id, secret, ttl_ms, timeout, handle,
                                                   error);
}

auto AiSidecarRuntimeImageAnalysisClient::RevokeCredential(const std::string&        handle,
                                                           std::chrono::milliseconds timeout,
                                                           bool* revoked,
                                                           std::string* error) -> bool {
  if (!runtime_) {
    if (revoked) *revoked = false;
    return true;
  }
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    if (revoked) *revoked = false;
    return true;
  }
  return session->credentials().RevokeCredential(handle, timeout, revoked, error);
}

auto AiSidecarRuntimeImageAnalysisClient::DescribeImage(const ImageAnalysisRequest& request,
                                                        std::chrono::milliseconds timeout)
    -> ImageAnalysisUnderstandingResult {
  if (!runtime_) {
    ImageAnalysisUnderstandingResult r;
    r.request_id = request.request_id;
    r.ok         = false;
    r.error      = "ai sidecar runtime is not available";
    return r;
  }
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    ImageAnalysisUnderstandingResult r;
    r.request_id = request.request_id;
    r.ok         = false;
    r.error      = "ai sidecar runtime is not ready";
    return r;
  }
  return session->image_analysis().DescribeImage(request, timeout);
}

auto AiSidecarRuntimeImageAnalysisClient::ScoreImage(const ImageAnalysisRequest& request,
                                                     std::chrono::milliseconds timeout)
    -> ImageAnalysisRatingResult {
  if (!runtime_) {
    ImageAnalysisRatingResult r;
    r.request_id = request.request_id;
    r.ok         = false;
    r.error      = "ai sidecar runtime is not available";
    return r;
  }
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    ImageAnalysisRatingResult r;
    r.request_id = request.request_id;
    r.ok         = false;
    r.error      = "ai sidecar runtime is not ready";
    return r;
  }
  return session->image_analysis().ScoreImage(request, timeout);
}

auto AiSidecarRuntimeImageAnalysisClient::AnalyzeImage(const ImageAnalysisRequest& request,
                                                       std::chrono::milliseconds timeout)
    -> ImageAnalysisCombinedResult {
  if (!runtime_) {
    ImageAnalysisCombinedResult r;
    r.request_id = request.request_id;
    r.ok         = false;
    r.error      = "ai sidecar runtime is not available";
    return r;
  }
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    ImageAnalysisCombinedResult r;
    r.request_id = request.request_id;
    r.ok         = false;
    r.error      = "ai sidecar runtime is not ready";
    return r;
  }
  return session->image_analysis().AnalyzeImage(request, timeout);
}

auto AiSidecarRuntimeImageAnalysisClient::BatchAnalyzeImage(
    const std::vector<ImageAnalysisRequest>& requests, std::chrono::milliseconds timeout)
    -> std::vector<ImageAnalysisCombinedResult> {
  if (!runtime_) {
    std::vector<ImageAnalysisCombinedResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      ImageAnalysisCombinedResult r;
      r.request_id = request.request_id;
      r.ok         = false;
      r.error      = "ai sidecar runtime is not available";
      results.push_back(std::move(r));
    }
    return results;
  }
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    std::vector<ImageAnalysisCombinedResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      ImageAnalysisCombinedResult r;
      r.request_id = request.request_id;
      r.ok         = false;
      r.error      = "ai sidecar runtime is not ready";
      results.push_back(std::move(r));
    }
    return results;
  }
  return session->image_analysis().BatchAnalyzeImage(requests, timeout);
}

auto AiSidecarRuntimeImageAnalysisClient::ListModels(const std::string&        provider_id,
                                                     const std::string&        credential_ref,
                                                     std::chrono::milliseconds timeout)
    -> ImageAnalysisListModelsResult {
  if (!runtime_) {
    ImageAnalysisListModelsResult r;
    r.ok    = false;
    r.error = "ai sidecar runtime is not available";
    return r;
  }
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    ImageAnalysisListModelsResult r;
    r.ok    = false;
    r.error = "ai sidecar runtime is not ready";
    return r;
  }
  return session->image_analysis().ListModels(provider_id, credential_ref, timeout);
}

auto AiSidecarRuntimeImageAnalysisClient::CancelTask(const std::string& request_id,
                                                     std::chrono::milliseconds timeout,
                                                     bool* cancelled, std::string* error) -> bool {
  if (!runtime_) {
    if (error) *error = "ai sidecar runtime is not available";
    return false;
  }
  const auto session = runtime_->ClientSession();
  if (!session || runtime_->Status().state != AiSidecarRuntimeState::kReady) {
    if (error) *error = "ai sidecar runtime is not ready";
    return false;
  }
  return session->runtime().CancelTask(request_id, timeout, cancelled, error);
}

// --- ImageAnalysisJob ---

ImageAnalysisJob::~ImageAnalysisJob() {
  Cancel();
  // RunJob joins the producer before it returns, so producer_ is normally non-joinable
  // here. Join defensively (with the self-join guard) for the case where a job is
  // destroyed without Wait()-ing to completion. Order: producer first (shorter-lived),
  // then the worker. worker_ may be self (the worker thread releasing the last ref when
  // the caller drops the job without waiting); producer_ is never self here because the
  // producer thread's ref keeps the job alive until RunJob has already joined it.
  if (producer_.joinable()) {
    if (producer_.get_id() == std::this_thread::get_id()) {
      producer_.detach();
    } else {
      producer_.join();
    }
  }
  if (worker_.joinable()) {
    if (worker_.get_id() == std::this_thread::get_id()) {
      worker_.detach();
    } else {
      worker_.join();
    }
  }
}

void ImageAnalysisJob::Cancel() {
  canceled_.store(true);
  if (gate_) {
    gate_->NotifyAll();
  }
  // Best-effort server-side cancel of THIS job's in-flight RPC. am_in_flight_ is true
  // only while this job occupies the gate slot, and AcquireAndPublish publishes this
  // job's request_id atomically with the slot, so while am_in_flight_ is true the gate's
  // current id is this job's (non-empty) request_id. CancelTask returning cancelled=false
  // (already finished / unknown) is harmless; the pre-RPC re-check + post-RPC
  // IsCanceled() discard in RunJob are the guarantees that no paid RPC is honored after
  // cancel.
  if (am_in_flight_.load() && client_ && gate_) {
    const auto id = gate_->CurrentRequestId();
    if (!id.empty()) {
      bool cancelled = false;
      client_->CancelTask(id, std::chrono::milliseconds(2000), &cancelled, nullptr);
    }
  }
}

auto ImageAnalysisJob::IsCanceled() const -> bool { return canceled_.load(); }

void ImageAnalysisJob::Wait() {
  {
    std::unique_lock lk(lock_);
    finished_cv_.wait(lk, [this]() { return finished_; });
  }
  if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
    worker_.join();
  }
}

auto ImageAnalysisJob::SnapshotProgress() const -> ImageAnalysisProgress {
  std::unique_lock lk(lock_);
  return progress_;
}

auto ImageAnalysisJob::Results() const -> std::vector<ImageAnalysisItemResult> {
  std::unique_lock lk(lock_);
  return results_;
}

void ImageAnalysisJob::UpdateProgress(
    const std::function<void(ImageAnalysisProgress&)>& updater) {
  std::unique_lock lk(lock_);
  updater(progress_);
}

void ImageAnalysisJob::AppendResult(ImageAnalysisItemResult result) {
  std::unique_lock lk(lock_);
  results_.push_back(std::move(result));
}

void ImageAnalysisJob::SetWorkerThread(std::thread worker) { worker_ = std::move(worker); }

void ImageAnalysisJob::Finish() {
  {
    std::unique_lock lk(lock_);
    finished_ = true;
  }
  finished_cv_.notify_all();
}

void ImageAnalysisJob::SetGate(std::shared_ptr<ImageAnalysisInFlightGate> gate) {
  gate_ = std::move(gate);
}

void ImageAnalysisJob::SetClient(std::shared_ptr<IImageAnalysisClient> client) {
  client_ = std::move(client);
}

// --- ImageAnalysisService ---

ImageAnalysisService::ImageAnalysisService(
    std::shared_ptr<IAnalysisRenditionProvider> thumbnail_provider,
    std::shared_ptr<IImageAnalysisClient>       analysis_client,
    std::shared_ptr<ImageAnalysisInFlightGate>  in_flight_gate)
    : thumbnail_provider_(std::move(thumbnail_provider)),
      analysis_client_(std::move(analysis_client)),
      in_flight_gate_(in_flight_gate ? in_flight_gate
                                     : std::make_shared<ImageAnalysisInFlightGate>()) {
  if (!thumbnail_provider_) {
    throw std::invalid_argument("ImageAnalysisService requires a thumbnail provider");
  }
  if (!analysis_client_) {
    throw std::invalid_argument("ImageAnalysisService requires an analysis client");
  }
}

auto ImageAnalysisService::StartAnalysis(std::vector<ImageAnalysisItem> items,
                                         ImageAnalysisOptions           options,
                                         ImageAnalysisProgressCallback  on_progress,
                                         ImageAnalysisFinishedCallback  on_finished)
    -> std::shared_ptr<ImageAnalysisJob> {
  auto job = std::make_shared<ImageAnalysisJob>();
  job->UpdateProgress(
      [total = items.size()](ImageAnalysisProgress& p) { p.total = total; });
  job->SetGate(in_flight_gate_);
  job->SetClient(analysis_client_);

  auto thumbnail_provider = thumbnail_provider_;
  auto analysis_client    = analysis_client_;
  auto gate               = in_flight_gate_;
  auto worker             = std::thread(
      [job, items = std::move(items), options = std::move(options),
       on_progress = std::move(on_progress), on_finished = std::move(on_finished),
       thumbnail_provider = std::move(thumbnail_provider),
       analysis_client = std::move(analysis_client), gate = std::move(gate)]() mutable {
        RunJob(job, items, options, std::move(on_progress), std::move(on_finished),
               std::move(thumbnail_provider), std::move(analysis_client), std::move(gate));
      });
  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral("image_analysis.start total=%1 task=%2 provider=%3 model=%4")
             .arg(static_cast<qulonglong>(job->SnapshotProgress().total))
             .arg(options.task == ImageAnalysisTask::kScore ? QStringLiteral("score")
                                                            : QStringLiteral("describe"))
             .arg(QString::fromStdString(options.provider_id))
             .arg(QString::fromStdString(options.model_id));
  job->SetWorkerThread(std::move(worker));
  return job;
}

auto ImageAnalysisService::ValidateConnection(
    const ImageAnalysisConnectionValidationOptions& options, IAiCredentialStore* credential_store)
    -> ImageAnalysisConnectionValidationResult {
  ImageAnalysisConnectionValidationResult result;
  if (!analysis_client_->Ready()) {
    result.error = "ai sidecar runtime is not ready";
    return result;
  }
  if (options.provider_id.empty()) {
    result.error = "provider id is required";
    return result;
  }
  if (options.requires_credential && options.credential_slot.empty()) {
    result.error = "credential slot is required";
    return result;
  }

  std::string handle;
  std::string secret;
  if (options.requires_credential) {
    if (credential_store == nullptr) {
      result.error = "credential store is unavailable";
      return result;
    }
    std::string store_error;
    if (!credential_store->LoadCredential(options.credential_slot, &secret, &store_error)) {
      result.error = store_error.empty() ? std::string("credential is missing") : store_error;
      return result;
    }

    std::string register_error;
    const bool  registered = analysis_client_->RegisterCredential(
        options.provider_id, secret, options.credential_ttl_ms, options.timeout, &handle,
        &register_error);
    if (!secret.empty()) {
      std::fill(secret.begin(), secret.end(), '0');
    }
    secret.clear();
    secret.shrink_to_fit();

    if (!registered) {
      result.error =
          register_error.empty() ? std::string("credential registration failed") : register_error;
      return result;
    }
  }

  try {
    result.list_models = analysis_client_->ListModels(options.provider_id, handle, options.timeout);
    result.models = result.list_models.models;
    result.ok = result.list_models.ok;
    if (!result.ok) {
      result.error = result.list_models.error.empty() ? std::string("model discovery failed")
                                                      : result.list_models.error;
    }
  } catch (const std::exception& e) {
    result.ok = false;
    result.error = std::string("model discovery failed: ") + e.what();
  } catch (...) {
    result.ok = false;
    result.error = "model discovery failed";
  }

  if (!handle.empty()) {
    std::string revoke_error;
    bool        revoked = false;
    if (!analysis_client_->RevokeCredential(handle, options.timeout, &revoked, &revoke_error)) {
      if (result.ok) {
        result.ok = false;
        result.error =
            revoke_error.empty() ? std::string("credential revoke failed") : revoke_error;
      }
    }
    result.credential_revoked = revoked;
  }
  return result;
}

void ImageAnalysisService::RunJob(const std::shared_ptr<ImageAnalysisJob>&    job,
                                  const std::vector<ImageAnalysisItem>&       items,
                                  ImageAnalysisOptions                        options,
                                  ImageAnalysisProgressCallback               on_progress,
                                  ImageAnalysisFinishedCallback               on_finished,
                                  std::shared_ptr<IAnalysisRenditionProvider> thumbnail_provider,
                                  std::shared_ptr<IImageAnalysisClient>       analysis_client,
                                  std::shared_ptr<ImageAnalysisInFlightGate>  in_flight_gate) {
  auto dispatch_progress = [&](const ImageAnalysisProgress& p) {
    if (on_progress) {
      on_progress(p);
    }
  };
  auto update_and_dispatch = [&](std::function<void(ImageAnalysisProgress&)> updater) {
    job->UpdateProgress(updater);
    dispatch_progress(job->SnapshotProgress());
  };

  update_and_dispatch([&](ImageAnalysisProgress& p) { p.total = items.size(); });

  // (1) Register the credential once for this run, then drop the secret from the local
  // options copy. The handle (not the secret) threads into every request.
  std::string credential_ref;
  if (!options.credential.secret.empty()) {
    std::string reg_error;
    std::string handle;
    const bool  registered = analysis_client->RegisterCredential(
        options.credential.provider_id, options.credential.secret, options.credential_ttl_ms,
        options.timeout, &handle, &reg_error);
    // Zeroize + clear the secret from the local copy; it must not survive registration.
    auto& secret_field = options.credential.secret;
    if (!secret_field.empty()) {
      std::fill(secret_field.begin(), secret_field.end(), '0');
    }
    secret_field.clear();
    secret_field.shrink_to_fit();
    if (!registered) {
      for (const auto& item : items) {
        ImageAnalysisItemResult r;
        r.item   = item;
        r.status = ImageAnalysisItemStatus::kError;
        r.error  = std::string("credential registration failed: ") + reg_error;
        job->AppendResult(std::move(r));
      }
      update_and_dispatch([&](ImageAnalysisProgress& p) { p.failed += items.size(); });
      if (on_finished) {
        on_finished(job->Results());
      }
      job->Finish();
      return;
    }
    credential_ref = std::move(handle);
  }
  ScopeExit credential_revoke_guard([&] {
    if (credential_ref.empty()) {
      return;
    }
    bool        revoked = false;
    std::string revoke_error;
    if (!analysis_client->RevokeCredential(credential_ref, options.timeout, &revoked,
                                           &revoke_error) &&
        !revoke_error.empty()) {
      qCWarning(diag::semanticLog).noquote()
          << QStringLiteral("image_analysis.credential_revoke_failed error=%1")
                 .arg(QString::fromStdString(revoke_error));
    }
  });

  // Clamp the prefill depth: describe/score need one ready image; analyze needs one full
  // batch ready, and the default/cap allow the next batch to fill while the current
  // batch is in flight. The gate still caps remote at one provider call.
  const int min_prefetch = (options.task == ImageAnalysisTask::kAnalyze) ? kImageAnalysisBatchSize
                                                                         : 1;
  const int effective_prefetch =
      std::clamp(options.prefetch, min_prefetch, kMaxImageAnalysisPrefetch);
  if (effective_prefetch != options.prefetch) {
    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("image_analysis.prefetch clamped requested=%1 effective=%2 max=%3")
               .arg(options.prefetch)
               .arg(effective_prefetch)
               .arg(kMaxImageAnalysisPrefetch);
  }

  const auto temp_dir   = options.temp_dir.empty()
                              ? std::filesystem::temp_directory_path()
                              : options.temp_dir;
  const auto resolution = options.thumbnail_resolution;
  auto       queue      = std::make_shared<PrefillQueue>(
      static_cast<size_t>(effective_prefetch));

  // --- Producer thread (Phase 5e): overlaps local thumbnail/encode prep with the single
  // in-flight remote call. It prepares items in order, releases each ThumbnailGuard
  // immediately after encoding, and pushes a self-contained encoded item (bytes +
  // rendition + request identity; never a thumbnail pin) into the bounded ready queue.
  // The gate is NOT touched here — the consumer remains the sole remote-call boundary.
  // On cancel the producer stops requesting/encoding; the consumer finalizes un-produced
  // items. The ScopeExit guarantees MarkProducerDone on every exit so the consumer is
  // never stranded waiting for an item that will never come.
  auto producer_body = [&]() {
    ScopeExit done_guard([&] { queue->MarkProducerDone(); });
    for (const auto& item : items) {
      if (job->IsCanceled()) {
        break;
      }
      EncodedAnalysisItem e;
      e.item              = item;
      e.request_id        = MakeRequestId(item, options.task);
      e.provider_id       = options.provider_id;
      e.model_id          = options.model_id;
      e.prompt_profile_id = options.prompt_profile_id;
      e.rubric_id         = options.rubric_id;
      e.output_language   = options.output_language;
      e.rating_severity   = options.rating_severity;
      e.camera_context    = item.camera_context;
      e.credential_ref    = credential_ref;  // opaque handle; secret already cleared

      auto thumb = WaitForOneThumbnail(job, thumbnail_provider, item, resolution);
      if (job->IsCanceled() || thumb.status == ThumbnailRequestStatus::kCanceled) {
        break;  // consumer finalizer records canceled for this + remaining items
      }
      if (thumb.status != ThumbnailRequestStatus::kReady || !thumb.guard) {
        e.kind  = EncodedItemKind::kPrepFailed;
        e.error = thumb.message.empty() ? std::string("thumbnail unavailable") : thumb.message;
        if (!queue->Push(std::move(e), [job] { return job->IsCanceled(); })) {
          break;
        }
        continue;
      }

      std::string encode_error;
      auto        encoded = EncodeThumbnailForRemoteAnalysis(
          *thumb.guard, options.jpeg_quality, static_cast<uint32_t>(resolution), temp_dir,
          &encode_error);
      // Release the thumbnail pin immediately after encode — BEFORE the encoded item
      // waits in the queue / behind the remote gate. The queue holds bytes, not a pin.
      thumbnail_provider->ReleaseRendition(thumb.key);

      if (!encoded.ok) {
        e.kind  = EncodedItemKind::kPrepFailed;
        e.error = encode_error.empty() ? std::string("image encode failed") : encode_error;
      } else if (options.max_image_bytes > 0 &&
                 static_cast<int64_t>(encoded.bytes.size()) > options.max_image_bytes) {
        // Phase 6d: the selected profile caps the encoded-rendition byte size.
        // Fail closed — do NOT push the oversized payload to the bounded queue or
        // issue a paid provider call. The thumbnail pin was already released above,
        // so this path holds no pin while it reports the prep failure.
        e.kind  = EncodedItemKind::kPrepFailed;
        e.error = "encoded image (" + std::to_string(encoded.bytes.size()) +
                  " bytes) exceeds preset limit (" + std::to_string(options.max_image_bytes) +
                  " bytes)";
      } else {
        e.kind               = EncodedItemKind::kEncoded;
        e.bytes              = std::move(encoded.bytes);
        e.image_format_hint  = std::move(encoded.format_hint);
        e.rendition.kind     = std::move(encoded.rendition_kind);
        e.rendition.width    = encoded.width;
        e.rendition.height   = encoded.height;
        e.rendition.bytes    = e.bytes.size();
        e.rendition.max_edge = encoded.max_edge;
      }
      if (!queue->Push(std::move(e), [job] { return job->IsCanceled(); })) {
        break;  // canceled while blocked on queue capacity; finalizer cancels the rest
      }
    }
  };

  try {
    job->producer_ = std::thread(std::move(producer_body));
  } catch (const std::exception& e) {
    // Could not spawn the producer pipeline; fail every item and finish.
    for (const auto& item : items) {
      ImageAnalysisItemResult r;
      r.item   = item;
      r.status = ImageAnalysisItemStatus::kError;
      r.error  = std::string("image analysis pipeline failed: ") + e.what();
      job->AppendResult(std::move(r));
    }
    update_and_dispatch([&](ImageAnalysisProgress& p) { p.failed += items.size(); });
    if (on_finished) {
      on_finished(job->Results());
    }
    job->Finish();
    return;
  }

  // --- Consumer (runs on this worker thread): pops encoded items in FIFO order, holds
  // the single in-flight slot across the typed RPC, and appends structured DTO results.
  // Order is preserved (one queue entry per item, FIFO, single appender), so every input
  // item gets exactly one result in item order. On cancel the consumer stops popping to
  // RPC and the finalizer emits canceled results for every item it never processed
  // (including encoded-but-not-sent items left in the queue). No database writes.
  size_t consumed = 0;
  while (consumed < items.size()) {
    if (job->IsCanceled()) {
      break;
    }
    if (options.task == ImageAnalysisTask::kAnalyze) {
      auto batch = queue->PopBatch(static_cast<size_t>(kImageAnalysisBatchSize),
                                   [job] { return job->IsCanceled(); });
      if (batch.empty()) {
        break;
      }
      std::vector<EncodedAnalysisItem> encoded_entries;
      std::vector<ImageAnalysisRequest> requests;
      encoded_entries.reserve(batch.size());
      requests.reserve(batch.size());
      for (auto& e : batch) {
        if (e.kind == EncodedItemKind::kPrepFailed) {
          ImageAnalysisItemResult r;
          r.item      = e.item;
          r.request_id = e.request_id;
          r.rendition = e.rendition;
          r.status    = ImageAnalysisItemStatus::kError;
          r.error     = e.error;
          job->AppendResult(std::move(r));
          update_and_dispatch([&](ImageAnalysisProgress& p) { p.failed += 1; });
          ++consumed;
          continue;
        }
        encoded_entries.push_back(e);
        requests.push_back(MakeRequestFromEncoded(std::move(e), options));
      }
      if (requests.empty()) {
        continue;
      }
      const auto batch_request_id = requests.front().request_id + "-batch";
      if (!in_flight_gate->AcquireAndPublish(batch_request_id,
                                             [job]() { return job->IsCanceled(); })) {
        for (size_t i = 0; i < requests.size(); ++i) {
          ImageAnalysisItemResult r;
          r.item      = encoded_entries[i].item;
          r.request_id = requests[i].request_id;
          r.rendition = requests[i].rendition;
          r.status    = ImageAnalysisItemStatus::kCanceled;
          job->AppendResult(std::move(r));
        }
        update_and_dispatch([&](ImageAnalysisProgress& p) { p.canceled += requests.size(); });
        consumed += requests.size();
        continue;
      }
      job->am_in_flight_.store(true);
      if (job->IsCanceled()) {
        in_flight_gate->ClearRequestId();
        job->am_in_flight_.store(false);
        in_flight_gate->Release();
        for (size_t i = 0; i < requests.size(); ++i) {
          ImageAnalysisItemResult r;
          r.item      = encoded_entries[i].item;
          r.request_id = requests[i].request_id;
          r.rendition = requests[i].rendition;
          r.status    = ImageAnalysisItemStatus::kCanceled;
          job->AppendResult(std::move(r));
        }
        update_and_dispatch([&](ImageAnalysisProgress& p) { p.canceled += requests.size(); });
        consumed += requests.size();
        continue;
      }

      std::vector<ImageAnalysisCombinedResult> rpc_results;
      std::optional<std::string>               rpc_error;
      {
        ScopeExit slot_guard([&] {
          job->am_in_flight_.store(false);
          in_flight_gate->ClearRequestId();
          in_flight_gate->Release();
        });
        try {
          rpc_results = analysis_client->BatchAnalyzeImage(requests, options.timeout);
        } catch (const std::exception& ex) {
          rpc_error = std::string("image analysis batch rpc failed: ") + ex.what();
        }
      }

      for (size_t i = 0; i < requests.size(); ++i) {
        ImageAnalysisItemResult r;
        r.item      = encoded_entries[i].item;
        r.request_id = requests[i].request_id;
        r.rendition = requests[i].rendition;
        if (job->IsCanceled()) {
          r.status = ImageAnalysisItemStatus::kCanceled;
          job->AppendResult(std::move(r));
          update_and_dispatch([&](ImageAnalysisProgress& p) { p.canceled += 1; });
          ++consumed;
          continue;
        }
        if (rpc_error.has_value()) {
          r.status = ImageAnalysisItemStatus::kError;
          r.error  = *rpc_error;
          job->AppendResult(std::move(r));
          update_and_dispatch([&](ImageAnalysisProgress& p) { p.failed += 1; });
          ++consumed;
          continue;
        }
        ImageAnalysisCombinedResult res;
        if (i < rpc_results.size()) {
          res = std::move(rpc_results[i]);
        } else {
          res.request_id = requests[i].request_id;
          res.ok         = false;
          res.error      = "batch analysis response omitted item result";
        }
        r.request_id = res.request_id.empty() ? requests[i].request_id : res.request_id;
        if (res.ok && res.has_understanding) {
          r.understanding = std::move(res.understanding);
        }
        if (res.ok && res.has_rating) {
          r.rating = std::move(res.rating);
        }
        if (!res.ok) {
          r.understanding.request_id = r.request_id;
          r.understanding.ok         = false;
          r.understanding.status     = res.status;
          r.understanding.error_code = res.error_code;
          r.understanding.error      = res.error;
          r.understanding.provider   = res.provider;
          r.understanding.model_id   = res.model_id;
          r.rating.request_id        = r.request_id;
          r.rating.ok                = false;
          r.rating.status            = res.status;
          r.rating.error_code        = res.error_code;
          r.rating.error             = res.error;
          r.rating.provider          = res.provider;
          r.rating.model_id          = res.model_id;
        }

        const bool rating_valid = r.rating.rating >= 1 && r.rating.rating <= 5;
        const bool ok           = r.understanding.ok && r.rating.ok && rating_valid;
        if (ok) {
          r.status = ImageAnalysisItemStatus::kAnalyzed;
          if (r.understanding.rendition.width != 0 || r.understanding.rendition.height != 0 ||
              r.rating.rendition.width != 0 || r.rating.rendition.height != 0) {
            r.rendition = r.understanding.rendition;
          }
          update_and_dispatch([&](ImageAnalysisProgress& p) { p.analyzed += 1; });
        } else {
          r.status = ImageAnalysisItemStatus::kError;
          if (!r.understanding.ok && !r.understanding.error.empty()) {
            r.error = r.understanding.error;
          } else if (!r.rating.ok && !r.rating.error.empty()) {
            r.error = r.rating.error;
          } else if (!r.understanding.ok) {
            r.error = ProviderFailureMessage(r.understanding.status, r.understanding.error_code,
                                             r.understanding.provider, r.understanding.model_id);
          } else {
            r.error = ProviderFailureMessage(r.rating.status, r.rating.error_code,
                                             r.rating.provider, r.rating.model_id);
          }
          if (r.rating.ok && !rating_valid) {
            r.rating.ok = false;
            r.error     = "invalid image rating returned by provider: " +
                      std::to_string(r.rating.rating) + " (expected 1..5)";
          }
          update_and_dispatch([&](ImageAnalysisProgress& p) { p.failed += 1; });
        }
        job->AppendResult(std::move(r));
        ++consumed;
      }
      continue;
    }
    auto entry = queue->Pop([job] { return job->IsCanceled(); });
    if (!entry.has_value()) {
      break;  // producer done + empty (end of stream), or canceled
    }
    auto& e = *entry;

    if (e.kind == EncodedItemKind::kPrepFailed) {
      ImageAnalysisItemResult r;
      r.item      = e.item;
      r.request_id = e.request_id;
      r.rendition = e.rendition;
      r.status    = ImageAnalysisItemStatus::kError;
      r.error     = e.error;
      job->AppendResult(std::move(r));
      update_and_dispatch([&](ImageAnalysisProgress& p) { p.failed += 1; });
      ++consumed;
      continue;
    }

    // Encoded item. If the job was canceled while it sat in the queue, discard it as
    // canceled without touching the provider or the in-flight gate.
    if (job->IsCanceled()) {
      ImageAnalysisItemResult r;
      r.item      = e.item;
      r.request_id = e.request_id;
      r.rendition = e.rendition;
      r.status    = ImageAnalysisItemStatus::kCanceled;
      job->AppendResult(std::move(r));
      update_and_dispatch([&](ImageAnalysisProgress& p) { p.canceled += 1; });
      ++consumed;
      continue;
    }

    // Build the typed request from the self-contained queue entry. The consumer never
    // reads the secret-bearing options copy; the opaque credential_ref came via the queue.
    ImageAnalysisRequest req;
    req.request_id        = std::move(e.request_id);
    req.image_bytes       = std::move(e.bytes);
    req.image_format_hint = std::move(e.image_format_hint);
    req.rendition         = e.rendition;
    req.provider_id       = std::move(e.provider_id);
    req.model_id          = std::move(e.model_id);
    req.prompt_profile_id = std::move(e.prompt_profile_id);
    req.credential_ref    = std::move(e.credential_ref);
    req.rubric_id         = std::move(e.rubric_id);
    req.output_language   = std::move(e.output_language);
    req.rating_severity   = std::move(e.rating_severity);
    req.camera_context    = std::move(e.camera_context);

    // Acquire the service-wide in-flight slot (max one remote analysis at a time across
    // all services sharing this gate) AND publish this request_id atomically with the
    // slot. If canceled while queued, AcquireAndPublish returns false and we exit without
    // ever calling the provider. Atomic acquire+publish (rather than Acquire then a
    // separate PublishRequestId) closes the narrow cancel race where Cancel() could
    // observe a held slot with an empty id and skip CancelTask while the worker was still
    // about to issue the paid provider RPC.
    if (!in_flight_gate->AcquireAndPublish(req.request_id,
                                           [job]() { return job->IsCanceled(); })) {
      ImageAnalysisItemResult r;
      r.item      = e.item;
      r.request_id = req.request_id;
      r.rendition = req.rendition;
      r.status    = ImageAnalysisItemStatus::kCanceled;
      job->AppendResult(std::move(r));
      update_and_dispatch([&](ImageAnalysisProgress& p) { p.canceled += 1; });
      ++consumed;
      continue;
    }
    // Mark this job as the in-flight occupant AFTER the slot is won (not before: while
    // queued behind another job, am_in_flight_ must stay false so this job's Cancel()
    // never cancels the other job's RPC). Then re-check cancel: if Cancel() landed in the
    // window between AcquireAndPublish's internal IsCanceled check and this store, it saw
    // am_in_flight_ == false and sent no CancelTask - this re-check (after the store,
    // before the RPC) is what prevents the paid provider call from going out after a
    // cancel that sent no CancelTask. The seq_cst atomics + the gate mutex make the store
    // happen-after the atomic publish and the re-check observe a cancel that preceded it.
    job->am_in_flight_.store(true);
    if (job->IsCanceled()) {
      in_flight_gate->ClearRequestId();
      job->am_in_flight_.store(false);
      in_flight_gate->Release();
      ImageAnalysisItemResult r;
      r.item      = e.item;
      r.request_id = req.request_id;
      r.rendition = req.rendition;
      r.status    = ImageAnalysisItemStatus::kCanceled;
      job->AppendResult(std::move(r));
      update_and_dispatch([&](ImageAnalysisProgress& p) { p.canceled += 1; });
      ++consumed;
      continue;
    }

    req.include_understanding = options.task != ImageAnalysisTask::kScore;
    req.include_rating        = options.task != ImageAnalysisTask::kDescribe;
    req.include_rating_reasons = req.include_rating && options.include_rating_reasons;

    // The typed RPC. Multi-output analysis uses AnalyzeImage so one image upload/provider
    // request can return both understanding and rating. The in-flight slot is held across
    // the call; an RAII guard releases it (and clears the published request_id +
    // am_in_flight_) on scope exit — including if the provider throws, which would
    // otherwise leave the service-wide gate locked and the job stuck in-flight. The
    // provider call is wrapped so a thrown RPC becomes an item error instead of escaping.
    ImageAnalysisItemResult r;
    r.item      = e.item;
    r.request_id = req.request_id;
    r.rendition = req.rendition;
    {
      ScopeExit slot_guard([&] {
        job->am_in_flight_.store(false);
        in_flight_gate->ClearRequestId();
        in_flight_gate->Release();
      });
      try {
        if (options.task == ImageAnalysisTask::kAnalyze) {
          auto res     = analysis_client->AnalyzeImage(req, options.timeout);
          r.request_id = res.request_id.empty() ? req.request_id : res.request_id;
          if (res.ok && res.has_understanding) {
            r.understanding = std::move(res.understanding);
          }
          if (res.ok && res.has_rating) {
            r.rating = std::move(res.rating);
          }
          if (!res.ok) {
            r.understanding.request_id = r.request_id;
            r.understanding.ok         = false;
            r.understanding.status     = res.status;
            r.understanding.error_code = res.error_code;
            r.understanding.error      = res.error;
            r.understanding.provider   = res.provider;
            r.understanding.model_id   = res.model_id;
            r.rating.request_id        = r.request_id;
            r.rating.ok                = false;
            r.rating.status            = res.status;
            r.rating.error_code        = res.error_code;
            r.rating.error             = res.error;
            r.rating.provider          = res.provider;
            r.rating.model_id          = res.model_id;
          }
        } else if (options.task == ImageAnalysisTask::kScore) {
          auto res     = analysis_client->ScoreImage(req, options.timeout);
          r.request_id = res.request_id.empty() ? req.request_id : res.request_id;
          r.rating     = std::move(res);
        } else {
          auto res        = analysis_client->DescribeImage(req, options.timeout);
          r.request_id    = res.request_id.empty() ? req.request_id : res.request_id;
          r.understanding = std::move(res);
        }
      } catch (const std::exception& ex) {
        r.status = ImageAnalysisItemStatus::kError;
        r.error  = std::string("image analysis rpc failed: ") + ex.what();
        job->AppendResult(std::move(r));
        update_and_dispatch([&](ImageAnalysisProgress& p) { p.failed += 1; });
        ++consumed;
        continue;  // slot_guard releases the slot on scope exit
      }
    }  // slot_guard releases the slot here on the success path

    // Post-RPC cancel check: if canceled during the call, discard the result even if the
    // provider succeeded — this is the correctness guarantee, not CancelTask.
    if (job->IsCanceled()) {
      r.status = ImageAnalysisItemStatus::kCanceled;
      job->AppendResult(std::move(r));
      update_and_dispatch([&](ImageAnalysisProgress& p) { p.canceled += 1; });
      ++consumed;
      continue;
    }

    const bool rating_valid = r.rating.rating >= 1 && r.rating.rating <= 5;
    const bool ok = (options.task == ImageAnalysisTask::kScore)
                        ? (r.rating.ok && rating_valid)
                        : (options.task == ImageAnalysisTask::kAnalyze
                               ? (r.understanding.ok && r.rating.ok && rating_valid)
                               : r.understanding.ok);
    if (ok) {
      r.status = ImageAnalysisItemStatus::kAnalyzed;
      // Prefer the sidecar-echoed rendition when present (records what was analyzed).
      if (r.understanding.rendition.width != 0 || r.understanding.rendition.height != 0 ||
          r.rating.rendition.width != 0 || r.rating.rendition.height != 0) {
        r.rendition = (options.task == ImageAnalysisTask::kScore) ? r.rating.rendition
                                                                  : r.understanding.rendition;
      }
      update_and_dispatch([&](ImageAnalysisProgress& p) { p.analyzed += 1; });
    } else {
      r.status = ImageAnalysisItemStatus::kError;
      if (options.task == ImageAnalysisTask::kScore) {
        r.error = r.rating.error.empty()
                      ? ProviderFailureMessage(r.rating.status, r.rating.error_code,
                                               r.rating.provider, r.rating.model_id)
                      : r.rating.error;
      } else if (options.task == ImageAnalysisTask::kAnalyze) {
        if (!r.understanding.ok && !r.understanding.error.empty()) {
          r.error = r.understanding.error;
        } else if (!r.rating.ok && !r.rating.error.empty()) {
          r.error = r.rating.error;
        } else if (!r.understanding.ok) {
          r.error = ProviderFailureMessage(r.understanding.status, r.understanding.error_code,
                                           r.understanding.provider, r.understanding.model_id);
        } else {
          r.error = ProviderFailureMessage(r.rating.status, r.rating.error_code,
                                           r.rating.provider, r.rating.model_id);
        }
      } else {
        r.error = r.understanding.error.empty()
                      ? ProviderFailureMessage(r.understanding.status, r.understanding.error_code,
                                               r.understanding.provider, r.understanding.model_id)
                      : r.understanding.error;
      }
      if ((options.task == ImageAnalysisTask::kScore || options.task == ImageAnalysisTask::kAnalyze)
          && r.rating.ok && !rating_valid) {
        r.rating.ok = false;
        r.error     = "invalid image rating returned by provider: " +
                    std::to_string(r.rating.rating) + " (expected 1..5)";
      }
      update_and_dispatch([&](ImageAnalysisProgress& p) { p.failed += 1; });
    }
    job->AppendResult(std::move(r));
    ++consumed;
  }

  // The producer has finished (or been canceled) by the time the consumer exits: Pop only
  // returns nullopt on producer_done + empty, and on cancel the producer's Push /
  // WaitForOneThumbnail observes the flag within the 25ms poll. Join it so no producer
  // thread outlives RunJob; the destructor's producer_ join is then inert.
  if (job->producer_.joinable()) {
    job->producer_.join();
  }

  // Finalizer: emit canceled results for every item the consumer never processed — the
  // items the producer never reached (cancel mid-prep) plus any encoded items left in the
  // queue at cancel time (their bytes are freed when `queue` is destroyed). This preserves
  // the invariant that every input item yields exactly one result, in order, with no
  // database writes.
  if (consumed < items.size()) {
    for (size_t j = consumed; j < items.size(); ++j) {
      ImageAnalysisItemResult r;
      r.item      = items[j];
      r.request_id = MakeRequestId(items[j], options.task);
      r.status    = ImageAnalysisItemStatus::kCanceled;
      job->AppendResult(std::move(r));
    }
    update_and_dispatch(
        [&](ImageAnalysisProgress& p) { p.canceled += (items.size() - consumed); });
  }

  // Revoke the credential BEFORE signaling finish. `on_finished` queues `Finish`
  // onto the controller's (GUI) thread; `Finish` releases the sidecar lease, and
  // when the lease refcount hits zero the sidecar Stops. If the revoke is left to
  // the `credential_revoke_guard` below (which fires only when RunJob returns —
  // after the GUI thread has already run Finish and stopped the sidecar), the
  // revoke no-ops against a not-ready runtime and the handle leaks in the sidecar
  // vault until its TTL. Revoking here — while the GUI thread still holds the
  // lease and the sidecar is guaranteed ready — makes the revoke deterministic.
  if (!credential_ref.empty()) {
    bool        revoked = false;
    std::string revoke_error;
    if (!analysis_client->RevokeCredential(credential_ref, options.timeout, &revoked,
                                           &revoke_error) &&
        !revoke_error.empty()) {
      qCWarning(diag::semanticLog).noquote()
          << QStringLiteral("image_analysis.credential_revoke_failed error=%1")
                 .arg(QString::fromStdString(revoke_error));
    }
    credential_ref.clear();  // the ScopeExit guard below is now a no-op
  }

  if (on_finished) {
    on_finished(job->Results());
  }
  job->Finish();
}

}  // namespace alcedo
