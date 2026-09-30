//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/image_analysis_service.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <future>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/core.hpp>

#include "image/image_buffer.hpp"

namespace alcedo {
namespace {

// AIResponseStatus / AiErrorCode values (proto/ai_common.proto). Hardcoded as ints to
// keep the test proto-free, matching the input_kinds convention on AiSidecarCapability.
constexpr int kStatusOk                 = 1;
constexpr int kStatusDeadlineExceeded   = 3;
constexpr int kStatusUnauthenticated    = 6;
constexpr int kStatusProviderError      = 9;
constexpr int kStatusUnsupportedTask    = 10;
constexpr int kErrorMissingCredential   = 2;
constexpr int kErrorTaskUnknown         = 9;
constexpr int kErrorPayloadDecode       = 10;

#ifndef ALCEDO_REPO_ROOT
#define ALCEDO_REPO_ROOT "."
#endif

auto ScratchDir(const std::string& tag) -> std::filesystem::path {
  return std::filesystem::temp_directory_path() / ("alcedo_ia_svc_test_" + tag);
}

auto EnvFileValue(const std::filesystem::path& path, const std::string& key) -> std::string {
  std::ifstream in(path);
  if (!in.is_open()) {
    return {};
  }
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos || line[first] == '#') {
      continue;
    }
    auto key_pos = first;
    if (line.compare(first, 7, "export ") == 0) {
      key_pos = first + 7;
    }
    if (line.compare(key_pos, key.size(), key) != 0 ||
        line.size() <= key_pos + key.size() || line[key_pos + key.size()] != '=') {
      continue;
    }
    std::string value = line.substr(key_pos + key.size() + 1);
    const auto  a = value.find_first_not_of(" \t");
    const auto  b = value.find_last_not_of(" \t");
    if (a == std::string::npos) {
      return {};
    }
    value = value.substr(a, b - a + 1);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                              (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    return value;
  }
  return {};
}

auto EnvOrFileValue(const std::filesystem::path& env_path,
                    const std::vector<std::string>& names) -> std::string {
  for (const auto& name : names) {
    if (const char* value = std::getenv(name.c_str());
        value != nullptr && value[0] != '\0') {
      return std::string(value);
    }
  }
  for (const auto& name : names) {
    auto value = EnvFileValue(env_path, name);
    if (!value.empty()) {
      return value;
    }
  }
  return {};
}

// Synchronous thumbnail provider: returns a ready guard holding a small CPU mat so the
// encoder produces real JPEG bytes. No ThumbnailService / pipeline required. A block mode
// holds callbacks without delivering them, so a Phase 5e test can stall the producer (the
// "consumer waiting for an encoded item" case): cancel then unblocks the producer via
// RunJob's 25ms poll, and the held callbacks are simply discarded on provider teardown.
class FakeThumbnailProvider : public IAnalysisRenditionProvider {
 public:
  void RequestRendition(sl_element_id_t element_id, image_id_t image_id,
                        ThumbnailResolution resolution, ThumbnailResultCallback callback) override {
    ImageAnalysisItem item{};
    item.element_id = element_id;
    item.image_id   = image_id;
    ++request_count_;
    if (block_mode_.load()) {
      std::unique_lock lk(block_mutex_);
      pending_.push_back(Pending{item, resolution, std::move(callback)});
      block_cv_.notify_one();
      return;
    }
    DeliverReady(item, resolution, std::move(callback));
  }
  void CancelRendition(const ThumbnailCacheKey& /*key*/) override { ++cancel_count_; }
  void ReleaseRendition(const ThumbnailCacheKey& /*key*/) override { ++release_count_; }

  void SetBlockMode(bool block) {
    std::unique_lock lk(block_mutex_);
    block_mode_ = block;
  }
  // Blocks until at least one request is pending (the producer has entered RequestRendition
  // and is now blocked waiting for the callback). Returns false on timeout.
  auto WaitForPending(std::chrono::milliseconds timeout) -> bool {
    std::unique_lock lk(block_mutex_);
    return block_cv_.wait_for(lk, timeout, [this] { return !pending_.empty(); });
  }
  auto PendingCount() const -> int {
    std::unique_lock lk(block_mutex_);
    return static_cast<int>(pending_.size());
  }

  auto RequestCount() const -> int { return request_count_.load(); }
  auto ReleaseCount() const -> int { return release_count_.load(); }

 private:
  struct Pending {
    ImageAnalysisItem               item;
    ThumbnailResolution             resolution;
    ThumbnailResultCallback         callback;
  };

  static void DeliverReady(const ImageAnalysisItem& item, ThumbnailResolution resolution,
                           ThumbnailResultCallback callback) {
    ThumbnailRequestResult r;
    r.key    = ThumbnailCacheKey{item.element_id, resolution};
    r.status = ThumbnailRequestStatus::kReady;
    cv::Mat  mat(16, 16, CV_8UC3, cv::Scalar(100, 150, 200));
    r.guard  = std::make_shared<ThumbnailGuard>();
    r.guard->thumbnail_buffer_ = std::make_unique<ImageBuffer>(std::move(mat));
    callback(std::move(r));
  }

  std::atomic<int> request_count_{0};
  std::atomic<int> cancel_count_{0};
  std::atomic<int> release_count_{0};

  std::atomic<bool>              block_mode_{false};
  mutable std::mutex             block_mutex_;
  std::condition_variable        block_cv_;
  std::deque<Pending>            pending_;
};

class FakeImageAnalysisClient : public IImageAnalysisClient {
 public:
  enum class Outcome {
    kSuccess,
    kMissingCredential,
    kUnsupported,
    kTimeout,
    kSchemaError,
    kEmptyProviderError,
    kInvalidRating
  };

  void SetDescribeOutcome(Outcome o) { describe_outcome_ = o; }
  void SetScoreOutcome(Outcome o) { score_outcome_ = o; }
  // When true, DescribeImage throws instead of returning — simulates a provider / RPC
  // wrapper that propagates an exception. Used to exercise the RunJob exception-safety
  // guard around the in-flight slot.
  void SetThrowOnDescribe(bool t) { throw_on_describe_ = t; }
  void SetBlockMode(bool block) {
    block_mode_ = block;
    release_blocked_ = false;
  }
  void ReleaseBlock() {
    {
      std::unique_lock lk(block_mutex_);
      release_blocked_ = true;
    }
    block_cv_.notify_all();
  }

  auto Ready() -> bool override { return true; }

  auto RegisterCredential(const std::string& provider_id, const std::string& secret,
                          int64_t /*ttl_ms*/, std::chrono::milliseconds /*timeout*/,
                          std::string* handle, std::string* /*error*/) -> bool override {
    ++register_calls_;
    std::unique_lock lk(record_mutex_);
    registered_provider_ = provider_id;
    registered_secret_   = secret;
    if (handle) *handle = "fake-handle";
    return true;
  }

  auto RevokeCredential(const std::string& handle, std::chrono::milliseconds /*timeout*/,
                        bool* revoked, std::string* /*error*/) -> bool override {
    ++revoke_calls_;
    std::unique_lock lk(record_mutex_);
    last_revoked_handle_ = handle;
    if (revoked) *revoked = (handle == "fake-handle");
    return true;
  }

  auto DescribeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds /*timeout*/)
      -> ImageAnalysisUnderstandingResult override {
    ++describe_calls_;
    {
      std::unique_lock lk(record_mutex_);
      last_describe_request_ = request;
    }
    if (block_mode_.load()) {
      std::unique_lock lk(block_mutex_);
      block_cv_.wait(lk, [this] { return release_blocked_.load(); });
    }
    if (throw_on_describe_.load()) {
      throw std::runtime_error("simulated provider rpc crash");
    }
    return MakeUnderstanding(request, describe_outcome_);
  }

  auto ScoreImage(const ImageAnalysisRequest& request, std::chrono::milliseconds /*timeout*/)
      -> ImageAnalysisRatingResult override {
    ++score_calls_;
    {
      std::unique_lock lk(record_mutex_);
      last_score_request_ = request;
    }
    if (block_mode_.load()) {
      std::unique_lock lk(block_mutex_);
      block_cv_.wait(lk, [this] { return release_blocked_.load(); });
    }
    return MakeRating(request, score_outcome_);
  }

  auto AnalyzeImage(const ImageAnalysisRequest& request, std::chrono::milliseconds /*timeout*/)
      -> ImageAnalysisCombinedResult override {
    ++analyze_calls_;
    {
      std::unique_lock lk(record_mutex_);
      last_analyze_request_ = request;
    }
    if (block_mode_.load()) {
      std::unique_lock lk(block_mutex_);
      block_cv_.wait(lk, [this] { return release_blocked_.load(); });
    }
    ImageAnalysisCombinedResult result;
    result.request_id          = request.request_id;
    result.ok                  = describe_outcome_ == Outcome::kSuccess &&
                 (score_outcome_ == Outcome::kSuccess || score_outcome_ == Outcome::kInvalidRating);
    result.status              = result.ok ? kStatusOk : kStatusProviderError;
    result.provider            = "fake";
    result.model_id            = request.model_id.empty() ? "fake-model" : request.model_id;
    result.rendition           = request.rendition;
    result.has_understanding   = true;
    result.has_rating          = true;
    result.understanding       = MakeUnderstanding(request, describe_outcome_);
    result.rating              = MakeRating(request, score_outcome_);
    if (!result.ok) {
      result.error = !result.understanding.error.empty() ? result.understanding.error
                                                         : result.rating.error;
    }
    return result;
  }

  auto BatchAnalyzeImage(const std::vector<ImageAnalysisRequest>& requests,
                         std::chrono::milliseconds /*timeout*/)
      -> std::vector<ImageAnalysisCombinedResult> override {
    ++batch_analyze_calls_;
    {
      std::unique_lock lk(record_mutex_);
      last_batch_analyze_requests_ = requests;
      batch_sizes_.push_back(requests.size());
    }
    if (block_mode_.load()) {
      std::unique_lock lk(block_mutex_);
      block_cv_.wait(lk, [this] { return release_blocked_.load(); });
    }
    std::vector<ImageAnalysisCombinedResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      results.push_back(AnalyzeImage(request, std::chrono::milliseconds(0)));
    }
    return results;
  }

  auto CancelTask(const std::string& request_id, std::chrono::milliseconds /*timeout*/,
                  bool* cancelled, std::string* /*error*/) -> bool override {
    ++cancel_calls_;
    std::unique_lock lk(record_mutex_);
    last_cancelled_id_ = request_id;
    if (cancelled) *cancelled = true;
    return true;
  }

  // Phase 6c: dry-run discovery seam used by the credential controller's
  // validate-connection flow. Returns a canned candidate list (or a typed
  // failure) without persisting anything.
  auto ListModels(const std::string& provider_id, const std::string& credential_ref,
                  std::chrono::milliseconds /*timeout*/) -> ImageAnalysisListModelsResult override {
    ++list_models_calls_;
    std::unique_lock lk(record_mutex_);
    last_list_models_provider_ = provider_id;
    last_list_models_credential_ref_ = credential_ref;
    ImageAnalysisListModelsResult result;
    result.ok = (list_models_outcome_ == Outcome::kSuccess);
    if (!result.ok) {
      result.error = "fake list-models failure";
      return result;
    }
    result.status = 1;  // AI_STATUS_OK
    result.models = list_models_canned_;
    return result;
  }
  void SetListModelsOutcome(Outcome o) { list_models_outcome_ = o; }
  void SetListModelsCanned(std::vector<AiDiscoveredModel> models) {
    list_models_canned_ = std::move(models);
  }
  auto ListModelsCalls() const -> int { return list_models_calls_.load(); }
  auto LastListModelsCredentialRef() const -> std::string {
    std::unique_lock lk(record_mutex_);
    return last_list_models_credential_ref_;
  }

  auto WaitForDescribeEntered(std::chrono::milliseconds timeout) -> bool {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (describe_calls_.load() < 1) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
  }

  auto DescribeCalls() const -> int { return describe_calls_.load(); }
  auto ScoreCalls() const -> int { return score_calls_.load(); }
  auto AnalyzeCalls() const -> int { return analyze_calls_.load(); }
  auto BatchAnalyzeCalls() const -> int { return batch_analyze_calls_.load(); }
  auto BatchSizes() const -> std::vector<size_t> {
    std::unique_lock lk(record_mutex_);
    return batch_sizes_;
  }
  auto CancelCalls() const -> int { return cancel_calls_.load(); }
  auto RegisterCalls() const -> int { return register_calls_.load(); }
  auto RevokeCalls() const -> int { return revoke_calls_.load(); }
  auto RegisteredSecret() const -> std::string {
    std::unique_lock lk(record_mutex_);
    return registered_secret_;
  }
  auto LastRevokedHandle() const -> std::string {
    std::unique_lock lk(record_mutex_);
    return last_revoked_handle_;
  }
  auto LastDescribeRequest() const -> ImageAnalysisRequest {
    std::unique_lock lk(record_mutex_);
    return last_describe_request_;
  }
  auto LastAnalyzeRequest() const -> ImageAnalysisRequest {
    std::unique_lock lk(record_mutex_);
    return last_analyze_request_;
  }
  auto LastCancelledId() const -> std::string {
    std::unique_lock lk(record_mutex_);
    return last_cancelled_id_;
  }

 private:
  static auto MakeUnderstanding(const ImageAnalysisRequest& req, Outcome o)
      -> ImageAnalysisUnderstandingResult {
    ImageAnalysisUnderstandingResult r;
    r.request_id = req.request_id;
    r.rendition  = req.rendition;
    r.provider   = "fake";
    r.model_id   = req.model_id.empty() ? std::string("fake-model") : req.model_id;
    switch (o) {
      case Outcome::kSuccess:
        r.ok = true;
        r.status = kStatusOk;
        r.caption = "a caption";
        r.tags = {"tag1", "tag2"};
        r.scene = "scene";
        r.confidence = 0.9;
        break;
      case Outcome::kMissingCredential:
        r.ok = false;
        r.status = kStatusUnauthenticated;
        r.error_code = kErrorMissingCredential;
        r.error = "credential missing";
        break;
      case Outcome::kUnsupported:
        r.ok = false;
        r.status = kStatusUnsupportedTask;
        r.error_code = kErrorTaskUnknown;
        r.error = "unsupported provider";
        break;
      case Outcome::kTimeout:
        r.ok = false;
        r.status = kStatusDeadlineExceeded;
        r.error = "timeout";
        break;
      case Outcome::kSchemaError:
        r.ok = false;
        r.status = kStatusProviderError;
        r.error_code = kErrorPayloadDecode;
        r.error = "schema validation failed";
        break;
      case Outcome::kEmptyProviderError:
        r.ok = false;
        r.status = kStatusProviderError;
        r.error_code = kErrorPayloadDecode;
        break;
    }
    return r;
  }

  static auto MakeRating(const ImageAnalysisRequest& req, Outcome o) -> ImageAnalysisRatingResult {
    ImageAnalysisRatingResult r;
    r.request_id = req.request_id;
    r.rendition  = req.rendition;
    r.provider   = "fake";
    r.model_id   = req.model_id.empty() ? std::string("fake-model") : req.model_id;
    if (o == Outcome::kSuccess || o == Outcome::kInvalidRating) {
      r.ok = true;
      r.status = kStatusOk;
      // 1..=5 integer star rating (Phase 5f contract); no scores array, no confidence.
      r.rating = (o == Outcome::kInvalidRating) ? 0 : 4;
      r.rubric_id = "alcedo-default-v1";
      r.rubric_version = "1";
    } else {
      r.ok = false;
      r.status = (o == Outcome::kTimeout) ? kStatusDeadlineExceeded : kStatusProviderError;
      r.error = "failed";
    }
    return r;
  }

  std::atomic<int> describe_calls_{0};
  std::atomic<int> score_calls_{0};
  std::atomic<int> analyze_calls_{0};
  std::atomic<int> batch_analyze_calls_{0};
  std::atomic<int> cancel_calls_{0};
  std::atomic<int> register_calls_{0};
  std::atomic<int> list_models_calls_{0};
  std::atomic<int> revoke_calls_{0};

  Outcome describe_outcome_             = Outcome::kSuccess;
  Outcome score_outcome_                = Outcome::kSuccess;
  Outcome list_models_outcome_          = Outcome::kSuccess;
  std::vector<AiDiscoveredModel> list_models_canned_;

  std::atomic<bool> throw_on_describe_{false};
  std::atomic<bool> block_mode_{false};
  std::atomic<bool> release_blocked_{false};
  std::mutex        block_mutex_;
  std::condition_variable block_cv_;

  mutable std::mutex record_mutex_;
  std::string        registered_provider_;
  std::string        registered_secret_;
  ImageAnalysisRequest last_describe_request_;
  ImageAnalysisRequest last_score_request_;
  ImageAnalysisRequest last_analyze_request_;
  std::vector<ImageAnalysisRequest> last_batch_analyze_requests_;
  std::vector<size_t>   batch_sizes_;
  std::string        last_cancelled_id_;
  std::string        last_list_models_provider_;
  std::string        last_list_models_credential_ref_;
  std::string        last_revoked_handle_;
};

auto BaseDescribeOpts(const std::string& tag) -> ImageAnalysisOptions {
  ImageAnalysisOptions opts;
  opts.task                = ImageAnalysisTask::kDescribe;
  opts.thumbnail_resolution = ThumbnailResolution::k1024;
  opts.jpeg_quality        = 90;
  opts.timeout             = std::chrono::milliseconds(5000);
  opts.provider_id         = "openrouter";
  opts.temp_dir            = ScratchDir(tag);
  std::filesystem::remove_all(opts.temp_dir);
  std::filesystem::create_directories(opts.temp_dir);
  return opts;
}

auto RunDescribe(std::shared_ptr<IAnalysisRenditionProvider>    provider,
                 std::shared_ptr<IImageAnalysisClient>          client,
                 std::shared_ptr<ImageAnalysisConcurrencyLimit> concurrency_limit,
                 const std::string& tag) -> std::vector<ImageAnalysisItemResult> {
  ImageAnalysisService service(provider, client, concurrency_limit);
  auto                 opts = BaseDescribeOpts(tag);
  auto                 job  = service.StartAnalysis({ImageAnalysisItem{1, 100}}, opts, {}, {});
  job->Wait();
  return job->Results();
}

// Bounded wait: returns true if the job finishes within `timeout`. On timeout it cancels
// the job (waking any blocked Acquire) so a leaked concurrency limit does not hang the test
// process, then returns false. The async future's destructor blocks until Wait() returns, which the
// Cancel guarantees, so no thread is leaked either.
auto WaitWithTimeout(const std::shared_ptr<ImageAnalysisJob>& job,
                     std::chrono::milliseconds                timeout) -> bool {
  auto fut = std::async(std::launch::async, [&] { job->Wait(); });
  if (fut.wait_for(timeout) == std::future_status::ready) {
    return true;
  }
  job->Cancel();
  return false;
}

// Polls `pred` every 5ms until it holds or `timeout` elapses. Returns true if pred held.
// Used to deterministically observe producer/consumer race points (the fake provider is
// synchronous, so the producer reaches a stable blocking state within a few polls).
template <typename Pred>
auto SpinWaitFor(Pred pred, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return pred();
}

TEST(ImageAnalysisServiceTest, ThrowingRpcReleasesGateAndFinishesJob) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetThrowOnDescribe(true);
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();

  ImageAnalysisService service(provider, client, concurrency_limit);
  auto                 opts = BaseDescribeOpts("throw");
  // Two items: the second can only enter Acquire (and reach the provider) if the first
  // item's thrown RPC released the slot. A leaked concurrency limit would block the second item
  // forever and WaitWithTimeout would time out.
  auto job = service.StartAnalysis({ImageAnalysisItem{1, 100}, ImageAnalysisItem{2, 200}},
                                   opts, {}, {});
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)))
      << "job did not finish; in-flight concurrency_limit was likely not released after the throw";

  auto results = job->Results();
  ASSERT_EQ(results.size(), 2u);
  for (const auto& r : results) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kError);
    EXPECT_NE(r.error.find("image analysis rpc failed"), std::string::npos);
  }
  // Both items reached the provider — the second proves the concurrency limit was released after
  // the first throw, and the slot is not stuck.
  EXPECT_EQ(client->DescribeCalls(), 2);
  EXPECT_TRUE(concurrency_limit->CurrentRequestId().empty());
  std::filesystem::remove_all(ScratchDir("throw"));
}

TEST(ImageAnalysisServiceTest, DescribeSuccessReturnsAnalyzedResult) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  auto results  = RunDescribe(provider, client, nullptr, "success");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kAnalyzed);
  EXPECT_TRUE(results[0].understanding.ok);
  EXPECT_EQ(results[0].understanding.caption, "a caption");
  EXPECT_EQ(client->DescribeCalls(), 1);
  // Host controls rendition + records it in result metadata.
  EXPECT_EQ(results[0].rendition.width, 16u);
  EXPECT_EQ(results[0].rendition.height, 16u);
  EXPECT_EQ(results[0].rendition.max_edge, 16u);
  EXPECT_EQ(results[0].rendition.kind, "thumbnail");
  EXPECT_EQ(results[0].understanding.rendition.max_edge, 16u);
  std::filesystem::remove_all(ScratchDir("success"));
}

TEST(ImageAnalysisServiceTest, AnalyzeSuccessUsesSingleCombinedProviderCall) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();

  ImageAnalysisService service(provider, client, concurrency_limit);
  auto                 opts = BaseDescribeOpts("analyze-combined");
  opts.task                = ImageAnalysisTask::kAnalyze;
  opts.rubric_id           = "general";
  auto job = service.StartAnalysis({ImageAnalysisItem{1, 100}}, opts, {}, {});
  job->Wait();

  auto results = job->Results();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kAnalyzed);
  EXPECT_TRUE(results[0].understanding.ok);
  EXPECT_TRUE(results[0].rating.ok);
  EXPECT_EQ(results[0].understanding.caption, "a caption");
  EXPECT_EQ(results[0].rating.rating, 4);
  EXPECT_EQ(client->BatchAnalyzeCalls(), 1);
  EXPECT_EQ(client->AnalyzeCalls(), 1);
  EXPECT_EQ(client->DescribeCalls(), 0);
  EXPECT_EQ(client->ScoreCalls(), 0);
  const auto request = client->LastAnalyzeRequest();
  EXPECT_TRUE(request.include_understanding);
  EXPECT_TRUE(request.include_rating);
  std::filesystem::remove_all(ScratchDir("analyze-combined"));
}

TEST(ImageAnalysisServiceTest, AnalyzeBatchesThreeImagesPerRemoteCall) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();

  ImageAnalysisService service(provider, client, concurrency_limit);
  auto                 opts = BaseDescribeOpts("analyze-batch-three");
  opts.task                = ImageAnalysisTask::kAnalyze;
  opts.rubric_id           = "general";
  auto job = service.StartAnalysis({ImageAnalysisItem{1, 100}, ImageAnalysisItem{2, 200},
                                    ImageAnalysisItem{3, 300}, ImageAnalysisItem{4, 400},
                                    ImageAnalysisItem{5, 500}},
                                   opts, {}, {});
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));

  auto results = job->Results();
  ASSERT_EQ(results.size(), 5u);
  for (const auto& result : results) {
    EXPECT_EQ(result.status, ImageAnalysisItemStatus::kAnalyzed);
    EXPECT_TRUE(result.understanding.ok);
    EXPECT_TRUE(result.rating.ok);
  }
  EXPECT_EQ(client->BatchAnalyzeCalls(), 2);
  EXPECT_EQ(client->BatchSizes(), (std::vector<size_t>{3u, 2u}));
  std::filesystem::remove_all(ScratchDir("analyze-batch-three"));
}

TEST(ImageAnalysisServiceTest, ScoreSuccessRequiresOneToFiveRating) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetScoreOutcome(FakeImageAnalysisClient::Outcome::kInvalidRating);

  ImageAnalysisService service(provider, client, nullptr);
  auto                 opts = BaseDescribeOpts("invalid-score-rating");
  opts.task                 = ImageAnalysisTask::kScore;
  auto job = service.StartAnalysis({ImageAnalysisItem{1, 100}}, opts, {}, {});
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));

  auto results = job->Results();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kError);
  EXPECT_FALSE(results[0].rating.ok);
  EXPECT_EQ(results[0].rating.rating, 0);
  EXPECT_NE(results[0].error.find("invalid image rating"), std::string::npos);
  EXPECT_EQ(client->ScoreCalls(), 1);
  std::filesystem::remove_all(ScratchDir("invalid-score-rating"));
}

TEST(ImageAnalysisServiceTest, OversizedImageBytesRejectedBeforeProviderCall) {
  // Phase 6d: a preset `max_image_bytes` cap smaller than the encoded JPEG must
  // fail closed — the item is a prep failure (no provider call, no pin held),
  // never an analyzed result.
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();

  ImageAnalysisService service(provider, client, concurrency_limit);
  auto                 opts = BaseDescribeOpts("oversized");
  opts.max_image_bytes      = 1;  // smaller than any real encoded JPEG
  auto job = service.StartAnalysis({ImageAnalysisItem{1, 100}}, opts, {}, {});
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));
  auto results = job->Results();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kError);
  EXPECT_NE(results[0].error.find("exceeds preset limit"), std::string::npos);
  // The provider was never called — the cap rejects before the concurrency limit/RPC.
  EXPECT_EQ(client->DescribeCalls(), 0);
  std::filesystem::remove_all(ScratchDir("oversized"));
}

TEST(ImageAnalysisServiceTest, MissingCredentialPropagatesAsError) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetDescribeOutcome(FakeImageAnalysisClient::Outcome::kMissingCredential);
  auto results = RunDescribe(provider, client, nullptr, "missingcred");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kError);
  EXPECT_FALSE(results[0].understanding.ok);
  EXPECT_EQ(results[0].understanding.status, kStatusUnauthenticated);
  EXPECT_EQ(results[0].understanding.error_code, kErrorMissingCredential);
  std::filesystem::remove_all(ScratchDir("missingcred"));
}

TEST(ImageAnalysisServiceTest, InvalidProviderConfigPropagatesAsError) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetDescribeOutcome(FakeImageAnalysisClient::Outcome::kUnsupported);
  auto results = RunDescribe(provider, client, nullptr, "invalid");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kError);
  EXPECT_FALSE(results[0].understanding.ok);
  EXPECT_EQ(results[0].understanding.status, kStatusUnsupportedTask);
  EXPECT_EQ(results[0].understanding.error_code, kErrorTaskUnknown);
  std::filesystem::remove_all(ScratchDir("invalid"));
}

TEST(ImageAnalysisServiceTest, TimeoutPropagatesAsError) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetDescribeOutcome(FakeImageAnalysisClient::Outcome::kTimeout);
  auto results = RunDescribe(provider, client, nullptr, "timeout");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kError);
  EXPECT_EQ(results[0].understanding.status, kStatusDeadlineExceeded);
  std::filesystem::remove_all(ScratchDir("timeout"));
}

TEST(ImageAnalysisServiceTest, SchemaErrorPropagatesAsErrorWithoutActiveResult) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetDescribeOutcome(FakeImageAnalysisClient::Outcome::kSchemaError);
  auto results = RunDescribe(provider, client, nullptr, "schema");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kError);
  EXPECT_FALSE(results[0].understanding.ok);
  EXPECT_EQ(results[0].understanding.status, kStatusProviderError);
  EXPECT_EQ(results[0].understanding.error_code, kErrorPayloadDecode);
  // No active annotation: caption stays empty on a schema failure.
  EXPECT_TRUE(results[0].understanding.caption.empty());
  std::filesystem::remove_all(ScratchDir("schema"));
}

TEST(ImageAnalysisServiceTest, EmptyProviderErrorSynthesizesDiagnosticMessage) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetDescribeOutcome(FakeImageAnalysisClient::Outcome::kEmptyProviderError);
  auto results = RunDescribe(provider, client, nullptr, "empty-provider-error");
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kError);
  EXPECT_FALSE(results[0].understanding.ok);
  EXPECT_EQ(results[0].understanding.status, kStatusProviderError);
  EXPECT_EQ(results[0].understanding.error_code, kErrorPayloadDecode);
  EXPECT_NE(results[0].error.find("provider call failed"), std::string::npos);
  EXPECT_NE(results[0].error.find("status"), std::string::npos);
  EXPECT_NE(results[0].error.find("fake"), std::string::npos);
  std::filesystem::remove_all(ScratchDir("empty-provider-error"));
}

TEST(ImageAnalysisServiceTest, CancelRunningJobCallsCancelTaskAndDiscardsResult) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetBlockMode(true);
  ImageAnalysisService service(provider, client);
  auto                 opts = BaseDescribeOpts("cancel-running");
  auto                 job  = service.StartAnalysis({ImageAnalysisItem{1, 100}}, opts, {}, {});
  ASSERT_TRUE(client->WaitForDescribeEntered(std::chrono::seconds(2)));
  EXPECT_EQ(client->DescribeCalls(), 1);

  job->Cancel();
  EXPECT_EQ(client->CancelCalls(), 1);
  EXPECT_EQ(client->LastCancelledId(), "image-analysis-describe-1-100");

  // Release the blocked RPC so the worker observes the cancel and finishes.
  client->ReleaseBlock();
  job->Wait();

  auto results = job->Results();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kCanceled);
  // No extra provider call beyond the one that was in flight.
  EXPECT_EQ(client->DescribeCalls(), 1);
  std::filesystem::remove_all(ScratchDir("cancel-running"));
}

TEST(ImageAnalysisServiceTest, TwoJobsSharingGateRunSerially) {
  auto concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();

  auto provider_a = std::make_shared<FakeThumbnailProvider>();
  auto client_a   = std::make_shared<FakeImageAnalysisClient>();
  client_a->SetBlockMode(true);
  ImageAnalysisService service_a(provider_a, client_a, concurrency_limit);

  auto provider_b = std::make_shared<FakeThumbnailProvider>();
  auto client_b   = std::make_shared<FakeImageAnalysisClient>();
  ImageAnalysisService service_b(provider_b, client_b, concurrency_limit);

  auto opts = BaseDescribeOpts("queue-serial");
  auto job_a = service_a.StartAnalysis({ImageAnalysisItem{1, 100}}, opts, {}, {});
  ASSERT_TRUE(client_a->WaitForDescribeEntered(std::chrono::seconds(2)));
  EXPECT_EQ(client_a->DescribeCalls(), 1);

  auto job_b = service_b.StartAnalysis({ImageAnalysisItem{2, 200}}, opts, {}, {});
  // B must wait for A to release the concurrency limit: its provider call must not start.
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(client_b->DescribeCalls(), 0);

  client_a->ReleaseBlock();  // A finishes and releases the slot
  job_a->Wait();
  ASSERT_TRUE(client_b->WaitForDescribeEntered(std::chrono::seconds(2)));
  EXPECT_EQ(client_b->DescribeCalls(), 1);
  job_b->Wait();

  auto rb = job_b->Results();
  ASSERT_EQ(rb.size(), 1u);
  EXPECT_EQ(rb[0].status, ImageAnalysisItemStatus::kAnalyzed);
  auto ra = job_a->Results();
  ASSERT_EQ(ra.size(), 1u);
  EXPECT_EQ(ra[0].status, ImageAnalysisItemStatus::kAnalyzed);
  std::filesystem::remove_all(ScratchDir("queue-serial"));
}

TEST(ImageAnalysisServiceTest, CancelQueuedJobDoesNotStartProviderCall) {
  auto concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();

  auto provider_a = std::make_shared<FakeThumbnailProvider>();
  auto client_a   = std::make_shared<FakeImageAnalysisClient>();
  client_a->SetBlockMode(true);
  ImageAnalysisService service_a(provider_a, client_a, concurrency_limit);

  auto provider_b = std::make_shared<FakeThumbnailProvider>();
  auto client_b   = std::make_shared<FakeImageAnalysisClient>();
  ImageAnalysisService service_b(provider_b, client_b, concurrency_limit);

  auto opts = BaseDescribeOpts("cancel-queued");
  auto job_a = service_a.StartAnalysis({ImageAnalysisItem{1, 100}}, opts, {}, {});
  ASSERT_TRUE(client_a->WaitForDescribeEntered(std::chrono::seconds(2)));

  auto job_b = service_b.StartAnalysis({ImageAnalysisItem{2, 200}}, opts, {}, {});
  std::this_thread::sleep_for(std::chrono::milliseconds(150));
  EXPECT_EQ(client_b->DescribeCalls(), 0);  // B is queued behind A

  job_b->Cancel();
  client_a->ReleaseBlock();  // A finishes; B should skip its RPC entirely
  job_a->Wait();
  job_b->Wait();

  EXPECT_EQ(client_a->DescribeCalls(), 1);
  EXPECT_EQ(client_b->DescribeCalls(), 0);  // canceled queued job -> no provider call
  auto rb = job_b->Results();
  ASSERT_EQ(rb.size(), 1u);
  EXPECT_EQ(rb[0].status, ImageAnalysisItemStatus::kCanceled);
  std::filesystem::remove_all(ScratchDir("cancel-queued"));
}

TEST(ImageAnalysisServiceTest, SecretReachesOnlyRegisterCredentialNotDescribeImage) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  ImageAnalysisService service(provider, client);
  auto                 opts = BaseDescribeOpts("noleak");
  opts.credential.provider_id = "openrouter";
  opts.credential.secret      = "sk-DO-NOT-LEAK-SENTINEL-7c3f9a1e";

  auto job = service.StartAnalysis({ImageAnalysisItem{1, 100}}, opts, {}, {});
  job->Wait();
  auto results = job->Results();
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kAnalyzed);

  // The secret traveled only to the loopback RegisterCredential call.
  EXPECT_EQ(client->RegisterCalls(), 1);
  EXPECT_EQ(client->RegisteredSecret(), "sk-DO-NOT-LEAK-SENTINEL-7c3f9a1e");
  EXPECT_EQ(client->RevokeCalls(), 1);
  EXPECT_EQ(client->LastRevokedHandle(), "fake-handle");
  // Only the opaque handle (not the secret) reached DescribeImage.
  const auto req = client->LastDescribeRequest();
  EXPECT_EQ(req.credential_ref, "fake-handle");
  EXPECT_EQ(req.credential_ref.find("sk-DO-NOT-LEAK"), std::string::npos);
  EXPECT_EQ(req.image_format_hint.find("sk-DO-NOT-LEAK"), std::string::npos);
  // The result must not carry the secret.
  EXPECT_EQ(results[0].understanding.error.find("sk-DO-NOT-LEAK"), std::string::npos);
  EXPECT_EQ(results[0].understanding.caption.find("sk-DO-NOT-LEAK"), std::string::npos);
  std::filesystem::remove_all(ScratchDir("noleak"));
}

TEST(ImageAnalysisServiceTest, ValidateConnectionLoadsCredentialListsModelsAndRevokesHandle) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetListModelsCanned({
      AiDiscoveredModel{"gpt-4o", "GPT-4o", "opencode_go_openai"},
      AiDiscoveredModel{"gpt-4o-mini", "GPT-4o mini", "opencode_go_openai"},
  });
  ImageAnalysisService     service(provider, client);
  InMemoryAiCredentialStore store;
  ASSERT_TRUE(store.SaveCredential("opencode_api_key", "sk-VALIDATE-CONNECTION-7c3f", nullptr));

  ImageAnalysisConnectionValidationOptions opts;
  opts.provider_id     = "opencode_go_openai";
  opts.credential_slot = "opencode_api_key";
  opts.timeout         = std::chrono::milliseconds(5000);

  const auto result = service.ValidateConnection(opts, &store);

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.models.size(), 2u);
  EXPECT_EQ(client->RegisterCalls(), 1);
  EXPECT_EQ(client->RegisteredSecret(), "sk-VALIDATE-CONNECTION-7c3f");
  EXPECT_EQ(client->ListModelsCalls(), 1);
  EXPECT_EQ(client->LastListModelsCredentialRef(), "fake-handle");
  EXPECT_EQ(client->RevokeCalls(), 1);
  EXPECT_EQ(client->LastRevokedHandle(), "fake-handle");
  EXPECT_TRUE(result.credential_revoked);
  EXPECT_EQ(result.error.find("sk-VALIDATE-CONNECTION"), std::string::npos);
}

TEST(ImageAnalysisServiceTest, ValidateConnectionWithoutCredentialListsModelsDirectly) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetListModelsCanned({
      AiDiscoveredModel{"ccswitch-routed", "CC Switch routed model", "ccswitch_openai"},
  });
  ImageAnalysisService service(provider, client);

  ImageAnalysisConnectionValidationOptions opts;
  opts.provider_id          = "ccswitch_openai";
  opts.requires_credential = false;
  opts.timeout              = std::chrono::milliseconds(5000);

  const auto result = service.ValidateConnection(opts, nullptr);

  EXPECT_TRUE(result.ok) << result.error;
  ASSERT_EQ(result.models.size(), 1u);
  EXPECT_EQ(result.models[0].model_id, "ccswitch-routed");
  EXPECT_EQ(client->RegisterCalls(), 0);
  EXPECT_EQ(client->ListModelsCalls(), 1);
  EXPECT_EQ(client->LastListModelsCredentialRef(), "");
  EXPECT_EQ(client->RevokeCalls(), 0);
  EXPECT_FALSE(result.credential_revoked);
}

TEST(ImageAnalysisServiceTest, ValidateConnectionMissingCredentialFailsBeforeSidecar) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  ImageAnalysisService     service(provider, client);
  InMemoryAiCredentialStore store;

  ImageAnalysisConnectionValidationOptions opts;
  opts.provider_id     = "opencode_go_openai";
  opts.credential_slot = "missing_slot";

  const auto result = service.ValidateConnection(opts, &store);

  EXPECT_FALSE(result.ok);
  EXPECT_FALSE(result.error.empty());
  EXPECT_EQ(client->RegisterCalls(), 0);
  EXPECT_EQ(client->ListModelsCalls(), 0);
  EXPECT_EQ(client->RevokeCalls(), 0);
}

TEST(ImageAnalysisServiceTest, ValidateConnectionListModelsFailureStillRevokesHandle) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetListModelsOutcome(FakeImageAnalysisClient::Outcome::kMissingCredential);
  ImageAnalysisService     service(provider, client);
  InMemoryAiCredentialStore store;
  ASSERT_TRUE(store.SaveCredential("opencode_api_key", "sk-VALIDATE-FAILURE-4d2a", nullptr));

  ImageAnalysisConnectionValidationOptions opts;
  opts.provider_id     = "opencode_go_openai";
  opts.credential_slot = "opencode_api_key";

  const auto result = service.ValidateConnection(opts, &store);

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "fake list-models failure");
  EXPECT_EQ(client->RegisterCalls(), 1);
  EXPECT_EQ(client->ListModelsCalls(), 1);
  EXPECT_EQ(client->RevokeCalls(), 1);
  EXPECT_TRUE(result.credential_revoked);
  EXPECT_EQ(result.error.find("sk-VALIDATE-FAILURE"), std::string::npos);
}

TEST(ImageAnalysisServiceLiveTest, ValidateConnectionDiscoversOpencodeModels) {
  const std::filesystem::path repo_root = std::filesystem::path(ALCEDO_REPO_ROOT);
  const std::filesystem::path env_path =
      std::getenv("ALCEDO_OPENCODE_ENV_TEST_PATH") != nullptr
          ? std::filesystem::path(std::getenv("ALCEDO_OPENCODE_ENV_TEST_PATH"))
          : repo_root / "rust" / "puerh_mind" / ".env.test";
  const std::string api_key =
      EnvOrFileValue(env_path, {"ALCEDO_OPENCODE_API_KEY", "OPENCODE_API_KEY"});
  if (api_key.empty()) {
    GTEST_SKIP() << "Set ALCEDO_OPENCODE_API_KEY or OPENCODE_API_KEY in " << env_path
                 << " to run the live Opencode validate-connection check.";
  }

  std::filesystem::path runtime_path =
      std::getenv("ALCEDO_OPENCODE_LIVE_RUNTIME_PATH") != nullptr
          ? std::filesystem::path(std::getenv("ALCEDO_OPENCODE_LIVE_RUNTIME_PATH"))
          : repo_root / "rust" / "puerh_mind" / "target" / "debug" / "alcedo_mind.exe";
  if (!std::filesystem::exists(runtime_path)) {
    GTEST_SKIP() << "Sidecar binary not found at " << runtime_path
                 << "; build rust/puerh_mind target debug alcedo_mind first.";
  }

  auto runtime = std::make_shared<AiSidecarRuntimeService>();
  AiSidecarRuntimeOptions runtime_opts;
  runtime_opts.runtime_binary        = runtime_path;
  runtime_opts.model_root            = std::filesystem::temp_directory_path() /
                            "alcedo_opencode_validate_modelroot";
  runtime_opts.model_id              = "plhery/mobileclip2-onnx:s2";
  runtime_opts.device                = "cpu";
  runtime_opts.allow_download        = false;
  runtime_opts.require_model_info    = false;
  runtime_opts.startup_timeout       = std::chrono::milliseconds(120000);
  runtime_opts.health_poll_interval  = std::chrono::milliseconds(100);
  runtime_opts.graceful_stop_timeout = std::chrono::milliseconds(2000);
  runtime_opts.kill_timeout          = std::chrono::milliseconds(3000);
  std::filesystem::create_directories(runtime_opts.model_root);
  ASSERT_TRUE(runtime->StartAndWait(runtime_opts)) << runtime->Status().message;

  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<AiSidecarRuntimeImageAnalysisClient>(runtime);
  ImageAnalysisService     service(provider, client);
  InMemoryAiCredentialStore store;
  ASSERT_TRUE(store.SaveCredential("opencode_api_key", api_key, nullptr));

  ImageAnalysisConnectionValidationOptions opts;
  opts.provider_id     = std::getenv("ALCEDO_OPENCODE_LIVE_PROVIDER_ID") != nullptr
                             ? std::string(std::getenv("ALCEDO_OPENCODE_LIVE_PROVIDER_ID"))
                             : std::string("opencode_go_openai");
  opts.credential_slot = "opencode_api_key";
  opts.timeout         = std::chrono::milliseconds(120000);
  opts.credential_ttl_ms = 120000;

  const auto result = service.ValidateConnection(opts, &store);
  runtime->Stop();
  std::error_code ec;
  std::filesystem::remove_all(runtime_opts.model_root, ec);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_TRUE(result.credential_revoked);
  ASSERT_TRUE(result.list_models.ok) << result.list_models.error;
  ASSERT_FALSE(result.models.empty()) << "Opencode model discovery returned no models";

  std::cout << "opencode validate connection ok; provider=" << opts.provider_id
            << "; discovered_models=" << result.models.size() << "\n";
  const size_t limit = std::min<size_t>(result.models.size(), 20);
  for (size_t i = 0; i < limit; ++i) {
    std::cout << "opencode model[" << i << "]: id=" << result.models[i].model_id
              << "; display=" << result.models[i].display_name
              << "; source=" << result.models[i].source_provider_id << "\n";
  }
}

// === Phase 5e: producer/consumer prefill pipeline ============================================

// Pipeline overlap: while image 1 is blocked in the fake remote call, image 2 must already
// be thumbnail-requested AND encoded. Encoding is proven by the pin release (the producer
// calls ReleaseRendition only after EncodeThumbnailForRemoteAnalysis), so ReleaseCount == 2
// while DescribeCalls == 1 means image 2 was fully prepped locally behind image 1's RPC.
TEST(ImageAnalysisServiceTest, PrefillPipelinePreparesImage2WhileImage1BlockedInRpc) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetBlockMode(true);
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();
  ImageAnalysisService service(provider, client, concurrency_limit);

  auto opts  = BaseDescribeOpts("prefill-overlap");
  opts.prefetch = 1;
  auto job   = service.StartAnalysis({ImageAnalysisItem{1, 100}, ImageAnalysisItem{2, 200}}, opts, {}, {});
  ASSERT_TRUE(client->WaitForDescribeEntered(std::chrono::seconds(2)));  // image 1 in RPC

  ASSERT_TRUE(SpinWaitFor(
      [&] { return provider->RequestCount() >= 2 && provider->ReleaseCount() >= 2; },
      std::chrono::seconds(2)));
  EXPECT_EQ(provider->RequestCount(), 2);
  EXPECT_EQ(provider->ReleaseCount(), 2);
  EXPECT_EQ(client->DescribeCalls(), 1);  // only image 1 reached the provider

  client->ReleaseBlock();
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));
  auto results = job->Results();
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kAnalyzed);
  EXPECT_EQ(results[1].status, ImageAnalysisItemStatus::kAnalyzed);
  std::filesystem::remove_all(ScratchDir("prefill-overlap"));
}

// Bounded queue: with prefetch=2 and image 1 in flight, the producer can hold at most
// prefetch + 2 thumbnails (1 in flight + 2 queued + 1 blocked on a full push) = 4. It must
// NOT request the whole album while a single remote call is outstanding.
TEST(ImageAnalysisServiceTest, PrefetchBoundedQueueDoesNotRequestWholeAlbum) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetBlockMode(true);
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();
  ImageAnalysisService service(provider, client, concurrency_limit);

  auto opts  = BaseDescribeOpts("prefill-bound");
  opts.prefetch = 2;
  std::vector<ImageAnalysisItem> items = {
      {1, 100}, {2, 200}, {3, 300}, {4, 400}, {5, 500}, {6, 600}};
  auto job   = service.StartAnalysis(items, opts, {}, {});
  ASSERT_TRUE(client->WaitForDescribeEntered(std::chrono::seconds(2)));

  ASSERT_TRUE(SpinWaitFor([&] { return provider->RequestCount() >= 4; }, std::chrono::seconds(3)));
  // The producer is now blocked on a full push; the held RPC keeps the queue full, so the
  // count must not climb past prefetch + 2 = 4. Settle to prove it does not advance.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(provider->RequestCount(), 4);
  EXPECT_LT(provider->RequestCount(), static_cast<int>(items.size()));
  EXPECT_EQ(client->DescribeCalls(), 1);

  client->ReleaseBlock();
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(15)));
  auto results = job->Results();
  ASSERT_EQ(results.size(), items.size());
  for (const auto& r : results) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kAnalyzed);
  }
  EXPECT_EQ(client->DescribeCalls(), static_cast<int>(items.size()));
  std::filesystem::remove_all(ScratchDir("prefill-bound"));
}

// Regression (Phase 5e review): prefetch is clamped to an upper bound, not just >= 1. A
// caller requesting an oversized prefetch must NOT let the producer encode most of a large
// album while one RPC is blocked - the clamp preserves the bounded-memory promise. With
// prefetch clamped to kMaxImageAnalysisPrefetch over 10 items and image 1 blocked in the
// fake remote call, the producer requests at most kMaxImageAnalysisPrefetch + 2 thumbnails
// (1 in flight + max queued + 1 blocked on a full push), never the whole album.
TEST(ImageAnalysisServiceTest, OversizedPrefetchClampedToUpperBound) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetBlockMode(true);
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();
  ImageAnalysisService service(provider, client, concurrency_limit);

  auto opts  = BaseDescribeOpts("prefetch-clamp");
  opts.prefetch = 1000;  // far above kMaxImageAnalysisPrefetch; must be clamped down
  std::vector<ImageAnalysisItem> items = {
      {1, 100}, {2, 200}, {3, 300}, {4, 400}, {5, 500},
      {6, 600}, {7, 700}, {8, 800}, {9, 900}, {10, 1000}};
  ASSERT_GT(static_cast<int>(items.size()), kMaxImageAnalysisPrefetch + 2)
      << "item count must exceed the clamped bound to prove bounding";
  auto job   = service.StartAnalysis(items, opts, {}, {});
  ASSERT_TRUE(client->WaitForDescribeEntered(std::chrono::seconds(2)));

  ASSERT_TRUE(SpinWaitFor(
      [&] { return provider->RequestCount() >= kMaxImageAnalysisPrefetch + 2; },
      std::chrono::seconds(3)));
  // The producer is now blocked on a full push; the held RPC keeps the queue full, so the
  // count must not climb past kMaxImageAnalysisPrefetch + 2. Settle to prove it does not
  // advance toward the whole album.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(provider->RequestCount(), kMaxImageAnalysisPrefetch + 2);
  EXPECT_LT(provider->RequestCount(), static_cast<int>(items.size()));
  EXPECT_EQ(client->DescribeCalls(), 1);

  client->ReleaseBlock();
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(15)));
  auto results = job->Results();
  ASSERT_EQ(results.size(), items.size());
  for (const auto& r : results) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kAnalyzed);
  }
  EXPECT_EQ(client->DescribeCalls(), static_cast<int>(items.size()));
  std::filesystem::remove_all(ScratchDir("prefetch-clamp"));
}

// Pin lifetime: with prefetch=2 and image 1 in flight, the producer encodes+releases images
// 2 and 3 (ReleaseCount == 3) while only image 1 has reached the provider (DescribeCalls ==
// 1). The gap proves the pin is released after encode and BEFORE the encoded item waits
// behind the remote concurrency limit — images 2 and 3 are released but have not yet been sent.
TEST(ImageAnalysisServiceTest, PinReleasedAfterEncodeBeforeWaitingBehindGate) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetBlockMode(true);
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();
  ImageAnalysisService service(provider, client, concurrency_limit);

  auto opts  = BaseDescribeOpts("pin-lifetime");
  opts.prefetch = 2;
  auto job   = service.StartAnalysis(
      {ImageAnalysisItem{1, 100}, ImageAnalysisItem{2, 200}, ImageAnalysisItem{3, 300}}, opts, {}, {});
  ASSERT_TRUE(client->WaitForDescribeEntered(std::chrono::seconds(2)));

  ASSERT_TRUE(SpinWaitFor([&] { return provider->ReleaseCount() >= 3; }, std::chrono::seconds(3)));
  EXPECT_EQ(provider->ReleaseCount(), 3);
  EXPECT_EQ(client->DescribeCalls(), 1);

  client->ReleaseBlock();
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));
  auto results = job->Results();
  ASSERT_EQ(results.size(), 3u);
  for (const auto& r : results) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kAnalyzed);
  }
  std::filesystem::remove_all(ScratchDir("pin-lifetime"));
}

// Cancellation bullet: cancel while the consumer is waiting for an encoded item (the
// producer is stalled on a held thumbnail callback). Neither a thumbnail callback nor a
// remote call is needed: the 25ms polls observe the cancel and both threads exit cleanly.
TEST(ImageAnalysisServiceTest, CancelWhileConsumerWaitsForEncodedItem) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  provider->SetBlockMode(true);  // stall the producer; never deliver a thumbnail
  auto client = std::make_shared<FakeImageAnalysisClient>();
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();
  ImageAnalysisService service(provider, client, concurrency_limit);

  auto opts  = BaseDescribeOpts("cancel-wait-item");
  opts.prefetch = 1;
  auto job   = service.StartAnalysis({ImageAnalysisItem{1, 100}, ImageAnalysisItem{2, 200}}, opts, {}, {});
  ASSERT_TRUE(provider->WaitForPending(std::chrono::seconds(2)));  // producer entered + blocked

  job->Cancel();
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));

  auto results = job->Results();
  ASSERT_EQ(results.size(), 2u);
  for (const auto& r : results) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kCanceled);
  }
  EXPECT_EQ(client->DescribeCalls(), 0);  // no RPC ever reached the provider
  EXPECT_EQ(client->CancelCalls(), 0);    // nothing was in flight to cancel
  std::filesystem::remove_all(ScratchDir("cancel-wait-item"));
}

// Cancellation bullet: cancel while the producer is waiting for queue capacity. prefetch=1
// fills the queue with image 2, then the producer blocks on image 3's push. Cancel wakes the
// blocked push (returns false); the consumer, blocked in image 1's RPC, finalizes every
// item as canceled once the RPC returns.
TEST(ImageAnalysisServiceTest, CancelWhileProducerWaitsForQueueCapacity) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetBlockMode(true);
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();
  ImageAnalysisService service(provider, client, concurrency_limit);

  auto opts  = BaseDescribeOpts("cancel-queue-capacity");
  opts.prefetch = 1;
  std::vector<ImageAnalysisItem> items = {{1, 100}, {2, 200}, {3, 300}, {4, 400}};
  auto job   = service.StartAnalysis(items, opts, {}, {});
  ASSERT_TRUE(client->WaitForDescribeEntered(std::chrono::seconds(2)));
  // The producer has requested image 3 (1 in flight + 1 queued + 1 about to block on push).
  ASSERT_TRUE(SpinWaitFor([&] { return provider->RequestCount() >= 3; }, std::chrono::seconds(3)));

  job->Cancel();  // producer blocked on a full push; consumer blocked in the RPC

  client->ReleaseBlock();  // let the in-flight RPC return so the worker exits
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));

  auto results = job->Results();
  ASSERT_EQ(results.size(), items.size());
  for (const auto& r : results) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kCanceled);
  }
  EXPECT_EQ(client->DescribeCalls(), 1);  // only image 1 reached the provider
  std::filesystem::remove_all(ScratchDir("cancel-queue-capacity"));
}

// Cancellation bullet: cancel while a remote request is in flight, with a prefilled item
// behind it. The post-RPC discard drops image 1 even though the provider would have
// succeeded, and the prefilled image 2 is discarded without a remote call.
TEST(ImageAnalysisServiceTest, CancelWhileRemoteRequestInFlightDiscardsResult) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetBlockMode(true);
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();
  ImageAnalysisService service(provider, client, concurrency_limit);

  auto opts  = BaseDescribeOpts("cancel-in-flight");
  opts.prefetch = 1;
  auto job   = service.StartAnalysis({ImageAnalysisItem{1, 100}, ImageAnalysisItem{2, 200}}, opts, {}, {});
  ASSERT_TRUE(client->WaitForDescribeEntered(std::chrono::seconds(2)));
  EXPECT_EQ(client->DescribeCalls(), 1);

  job->Cancel();
  EXPECT_EQ(client->CancelCalls(), 1);
  EXPECT_EQ(client->LastCancelledId(), "image-analysis-describe-1-100");

  client->ReleaseBlock();  // the blocked RPC returns; post-RPC discard drops it
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));

  auto results = job->Results();
  ASSERT_EQ(results.size(), 2u);
  EXPECT_EQ(results[0].status, ImageAnalysisItemStatus::kCanceled);
  EXPECT_EQ(results[1].status, ImageAnalysisItemStatus::kCanceled);
  EXPECT_EQ(client->DescribeCalls(), 1);  // only image 1 reached the provider
  std::filesystem::remove_all(ScratchDir("cancel-in-flight"));
}

// Cancellation bullet: cancel after some encoded-but-not-sent items exist. prefetch=2 lets
// images 2 and 3 sit encoded in the queue behind image 1's in-flight RPC. Cancel discards
// the in-flight result (post-RPC) and the queued renditions without any extra remote call.
TEST(ImageAnalysisServiceTest, CancelAfterPrefilledNotSentItemsDropsQueuedRenditions) {
  auto provider = std::make_shared<FakeThumbnailProvider>();
  auto client   = std::make_shared<FakeImageAnalysisClient>();
  client->SetBlockMode(true);
  auto                 concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();
  ImageAnalysisService service(provider, client, concurrency_limit);

  auto opts  = BaseDescribeOpts("cancel-prefilled");
  opts.prefetch = 2;
  std::vector<ImageAnalysisItem> items = {{1, 100}, {2, 200}, {3, 300}, {4, 400}};
  auto job   = service.StartAnalysis(items, opts, {}, {});
  ASSERT_TRUE(client->WaitForDescribeEntered(std::chrono::seconds(2)));
  // Let the producer prefill images 2 and 3 (encoded) behind the in-flight image 1.
  ASSERT_TRUE(SpinWaitFor([&] { return provider->ReleaseCount() >= 3; }, std::chrono::seconds(3)));

  job->Cancel();

  client->ReleaseBlock();  // image 1's RPC returns; post-RPC discard drops it
  ASSERT_TRUE(WaitWithTimeout(job, std::chrono::seconds(10)));

  auto results = job->Results();
  ASSERT_EQ(results.size(), items.size());
  for (const auto& r : results) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kCanceled);
  }
  EXPECT_EQ(client->DescribeCalls(), 1);  // only image 1 ever reached the provider
  std::filesystem::remove_all(ScratchDir("cancel-prefilled"));
}

// Regression: two jobs sharing one ImageAnalysisConcurrencyLimit still serialize remote RPCs even
// when both locally prefill their queues. While A holds the concurrency limit in a blocked RPC, B
// locally prepares renditions (RequestCount_b >= 1) but makes ZERO remote calls; B only runs after
// A releases the slot.
TEST(ImageAnalysisServiceTest, TwoJobsSharingGateSerializeRpcsWithPrefill) {
  auto concurrency_limit = std::make_shared<ImageAnalysisConcurrencyLimit>();

  auto provider_a = std::make_shared<FakeThumbnailProvider>();
  auto client_a   = std::make_shared<FakeImageAnalysisClient>();
  client_a->SetBlockMode(true);  // A holds the concurrency limit in a blocked RPC
  ImageAnalysisService service_a(provider_a, client_a, concurrency_limit);

  auto provider_b = std::make_shared<FakeThumbnailProvider>();
  auto client_b   = std::make_shared<FakeImageAnalysisClient>();
  ImageAnalysisService service_b(provider_b, client_b, concurrency_limit);

  auto                 opts              = BaseDescribeOpts("concurrency_limit-prefill-regression");
  opts.prefetch = 2;
  std::vector<ImageAnalysisItem> items_a = {{1, 100}, {2, 200}, {3, 300}};
  std::vector<ImageAnalysisItem> items_b = {{4, 400}, {5, 500}, {6, 600}};

  auto job_a = service_a.StartAnalysis(items_a, opts, {}, {});
  ASSERT_TRUE(
      client_a->WaitForDescribeEntered(std::chrono::seconds(2)));  // A holds concurrency limit

  auto job_b = service_b.StartAnalysis(items_b, opts, {}, {});
  // B prefills locally but its consumer must block in Acquire behind A: zero B remote calls.
  ASSERT_TRUE(SpinWaitFor([&] { return provider_b->RequestCount() >= 1; }, std::chrono::seconds(3)));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // settle: prove B does not slip in
  EXPECT_EQ(client_b->DescribeCalls(), 0);
  EXPECT_GE(provider_b->RequestCount(), 1);  // B did local prep despite the held concurrency limit

  client_a->ReleaseBlock();  // A finishes and releases the slot
  ASSERT_TRUE(WaitWithTimeout(job_a, std::chrono::seconds(10)));
  ASSERT_TRUE(client_b->WaitForDescribeEntered(std::chrono::seconds(5)));  // B now runs
  ASSERT_TRUE(WaitWithTimeout(job_b, std::chrono::seconds(15)));

  auto ra = job_a->Results();
  ASSERT_EQ(ra.size(), items_a.size());
  auto rb = job_b->Results();
  ASSERT_EQ(rb.size(), items_b.size());
  for (const auto& r : ra) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kAnalyzed);
  }
  for (const auto& r : rb) {
    EXPECT_EQ(r.status, ImageAnalysisItemStatus::kAnalyzed);
  }
  EXPECT_EQ(client_a->DescribeCalls(), 3);
  EXPECT_EQ(client_b->DescribeCalls(), 3);
  std::filesystem::remove_all(ScratchDir("concurrency_limit-prefill-regression"));
}

}  // namespace
}  // namespace alcedo
