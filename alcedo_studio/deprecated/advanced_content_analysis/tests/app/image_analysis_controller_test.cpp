//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6d — album image-analysis job controller. Drives ImageAnalysisController
// with fakes (IImageAnalysisEnvironment + AiProviderProfileController on temp
// files) so the 6d-required cases (empty selection, one/multi success, cancel,
// retry, provider error, schema error, missing credential, score, shared-concurrency limit
// serialization) run without a live project or sidecar.

#include "ui/alcedo_main/album_backend/image_analysis_controller.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QString>
#include <QTemporaryDir>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <opencv2/core.hpp>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "app/ai_provider_profile.hpp"
#include "app/image_analysis_service.hpp"
#include "image/image_buffer.hpp"

namespace alcedo::ui {
namespace {

constexpr int kStatusOk               = 1;
constexpr int kStatusUnauthenticated  = 6;
constexpr int kStatusProviderError    = 9;
constexpr int kStatusUnsupportedTask  = 10;
constexpr int kErrorMissingCredential = 2;
constexpr int kErrorTaskUnknown       = 9;
constexpr int kErrorPayloadDecode     = 10;

#ifdef _WIN32
auto FsPathFromQStringForTest(const QString& path) -> std::filesystem::path {
  return std::filesystem::path(path.toStdWString());
}
#else
auto FsPathFromQStringForTest(const QString& path) -> std::filesystem::path {
  return std::filesystem::path(path.toStdString());
}
#endif

// Synchronous thumbnail provider: returns a ready guard holding a small CPU mat so
// the encoder produces real JPEG bytes. No ThumbnailService / pipeline required.
class FakeThumbProvider : public alcedo::IAnalysisRenditionProvider {
 public:
  void RequestRendition(sl_element_id_t element_id, image_id_t image_id,
                        alcedo::ThumbnailResolution     resolution,
                        alcedo::ThumbnailResultCallback callback) override {
    alcedo::ImageAnalysisItem item{};
    item.element_id = element_id;
    item.image_id   = image_id;
    ++request_count_;
    alcedo::ThumbnailRequestResult r;
    r.key    = alcedo::ThumbnailCacheKey{item.element_id, resolution};
    r.status = alcedo::ThumbnailRequestStatus::kReady;
    cv::Mat mat(16, 16, CV_8UC3, cv::Scalar(100, 150, 200));
    r.guard                    = std::make_shared<alcedo::ThumbnailGuard>();
    r.guard->thumbnail_buffer_ = std::make_unique<alcedo::ImageBuffer>(std::move(mat));
    callback(std::move(r));
  }
  void CancelRendition(const alcedo::ThumbnailCacheKey&) override {}
  void ReleaseRendition(const alcedo::ThumbnailCacheKey&) override { ++release_count_; }
  auto RequestCount() const -> int { return request_count_.load(); }
  auto ReleaseCount() const -> int { return release_count_.load(); }

 private:
  std::atomic<int> request_count_{0};
  std::atomic<int> release_count_{0};
};

class FakeClient : public alcedo::IImageAnalysisClient {
 public:
  enum class Outcome {
    kSuccess,
    kMissingCredential,
    kUnsupported,
    kSchemaError,
    kEmptyProviderError
  };

  void SetDescribeOutcome(Outcome o) { describe_outcome_ = o; }
  void SetScoreOutcome(Outcome o) { score_outcome_ = o; }
  // When false, success results carry no usage / provider_request_id (exercises the
  // itemsWithoutUsage branch of the Phase 7a usage aggregate).
  void SetUsageEnabled(bool enabled) { usage_enabled_ = enabled; }
  void SetBlockMode(bool block) {
    block_mode_      = block;
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
  auto RegisterCredential(const std::string&, const std::string&, int64_t ttl_ms,
                          std::chrono::milliseconds, std::string* handle, std::string*)
      -> bool override {
    ++register_calls_;
    last_registered_ttl_ms_ = ttl_ms;
    if (handle) {
      *handle = "fake-handle";
    }
    return true;
  }
  auto RevokeCredential(const std::string&, std::chrono::milliseconds, bool*, std::string*)
      -> bool override {
    ++revoke_calls_;
    return true;
  }
  auto DescribeImage(const alcedo::ImageAnalysisRequest& req, std::chrono::milliseconds)
      -> alcedo::ImageAnalysisUnderstandingResult override {
    ++describe_calls_;
    last_output_language_ = req.output_language;
    if (block_mode_.load()) {
      std::unique_lock lk(block_mutex_);
      block_cv_.wait(lk, [this] { return release_blocked_.load(); });
    }
    return MakeUnderstanding(req, describe_outcome_);
  }
  auto ScoreImage(const alcedo::ImageAnalysisRequest& req, std::chrono::milliseconds)
      -> alcedo::ImageAnalysisRatingResult override {
    ++score_calls_;
    last_output_language_ = req.output_language;
    last_rating_severity_ = req.rating_severity;
    last_camera_context_  = req.camera_context;
    if (block_mode_.load()) {
      std::unique_lock lk(block_mutex_);
      block_cv_.wait(lk, [this] { return release_blocked_.load(); });
    }
    return MakeRating(req, score_outcome_);
  }
  auto AnalyzeImage(const alcedo::ImageAnalysisRequest& req, std::chrono::milliseconds)
      -> alcedo::ImageAnalysisCombinedResult override {
    ++analyze_calls_;
    last_output_language_ = req.output_language;
    last_rating_severity_ = req.rating_severity;
    last_camera_context_  = req.camera_context;
    if (block_mode_.load()) {
      std::unique_lock lk(block_mutex_);
      block_cv_.wait(lk, [this] { return release_blocked_.load(); });
    }
    alcedo::ImageAnalysisCombinedResult r;
    r.request_id = req.request_id;
    r.ok         = describe_outcome_ == Outcome::kSuccess && score_outcome_ == Outcome::kSuccess;
    r.status     = r.ok ? kStatusOk : kStatusProviderError;
    r.provider   = "fake";
    r.model_id   = req.model_id.empty() ? std::string("fake-model") : req.model_id;
    r.rendition  = req.rendition;
    r.has_understanding = true;
    r.has_rating        = true;
    r.understanding     = MakeUnderstanding(req, describe_outcome_);
    r.rating            = MakeRating(req, score_outcome_);
    if (!r.ok) {
      r.error = !r.understanding.error.empty() ? r.understanding.error : r.rating.error;
    }
    return r;
  }

  auto BatchAnalyzeImage(const std::vector<alcedo::ImageAnalysisRequest>& requests,
                         std::chrono::milliseconds                        timeout)
      -> std::vector<alcedo::ImageAnalysisCombinedResult> override {
    std::vector<alcedo::ImageAnalysisCombinedResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      results.push_back(AnalyzeImage(request, timeout));
    }
    return results;
  }
  auto CancelTask(const std::string&, std::chrono::milliseconds, bool* cancelled, std::string*)
      -> bool override {
    ++cancel_calls_;
    if (cancelled) {
      *cancelled = true;
    }
    return true;
  }
  auto ListModels(const std::string&, const std::string& credential_ref, std::chrono::milliseconds)
      -> alcedo::ImageAnalysisListModelsResult override {
    ++list_models_calls_;
    last_list_models_credential_ref_ = credential_ref;
    alcedo::ImageAnalysisListModelsResult r;
    r.ok     = true;
    r.status = kStatusOk;
    r.models.push_back(alcedo::AiDiscoveredModel{.model_id = "fake-model"});
    return r;
  }

  auto DescribeCalls() const -> int { return describe_calls_.load(); }
  auto ScoreCalls() const -> int { return score_calls_.load(); }
  auto AnalyzeCalls() const -> int { return analyze_calls_.load(); }
  auto CancelCalls() const -> int { return cancel_calls_.load(); }
  auto RegisterCalls() const -> int { return register_calls_.load(); }
  auto LastRegisteredTtlMs() const -> int64_t { return last_registered_ttl_ms_.load(); }
  auto ListModelsCalls() const -> int { return list_models_calls_.load(); }
  auto LastListModelsCredentialRef() const -> std::string {
    return last_list_models_credential_ref_;
  }
  auto LastOutputLanguage() const -> std::string { return last_output_language_; }
  auto LastRatingSeverity() const -> std::string { return last_rating_severity_; }
  auto LastCameraContext() const -> std::string { return last_camera_context_; }

 private:
  auto MakeUnderstanding(const alcedo::ImageAnalysisRequest& req, Outcome o)
      -> alcedo::ImageAnalysisUnderstandingResult {
    alcedo::ImageAnalysisUnderstandingResult r;
    r.request_id = req.request_id;
    r.rendition  = req.rendition;
    r.provider   = "fake";
    r.model_id   = req.model_id.empty() ? std::string("fake-model") : req.model_id;
    if (o == Outcome::kSuccess) {
      r.ok                = true;
      r.status            = kStatusOk;
      r.caption           = "a caption";
      r.tags              = {"tag1", "tag2"};
      r.scene             = "scene";
      r.confidence        = 0.9;
      r.prompt_profile_id = "profile-v1";
      if (usage_enabled_) {
        r.provider_request_id = "req-desc-" + req.request_id;
        r.usage.input_tokens  = 10;
        r.usage.output_tokens = 20;
        r.usage.total_tokens  = 30;
      }
    } else if (o == Outcome::kMissingCredential) {
      r.ok         = false;
      r.status     = kStatusUnauthenticated;
      r.error_code = kErrorMissingCredential;
      r.error      = "credential missing";
    } else if (o == Outcome::kUnsupported) {
      r.ok         = false;
      r.status     = kStatusUnsupportedTask;
      r.error_code = kErrorTaskUnknown;
      r.error      = "unsupported provider";
    } else if (o == Outcome::kEmptyProviderError) {
      r.ok         = false;
      r.status     = kStatusProviderError;
      r.error_code = kErrorPayloadDecode;
    } else {
      r.ok         = false;
      r.status     = kStatusProviderError;
      r.error_code = kErrorPayloadDecode;
      r.error      = "schema validation failed";
    }
    return r;
  }
  auto MakeRating(const alcedo::ImageAnalysisRequest& req, Outcome o)
      -> alcedo::ImageAnalysisRatingResult {
    alcedo::ImageAnalysisRatingResult r;
    r.request_id = req.request_id;
    r.rendition  = req.rendition;
    r.provider   = "fake";
    r.model_id   = req.model_id.empty() ? std::string("fake-model") : req.model_id;
    if (o == Outcome::kSuccess) {
      r.ok                = true;
      r.status            = kStatusOk;
      r.rating            = 4;
      r.rubric_id         = "general";
      r.rubric_version    = "1.0";
      r.prompt_profile_id = "profile-v1";
      r.reasons           = "strong composition";
      if (usage_enabled_) {
        r.provider_request_id = "req-score-" + req.request_id;
        r.usage.input_tokens  = 5;
        r.usage.output_tokens = 7;
        r.usage.total_tokens  = 12;
      }
    } else if (o == Outcome::kEmptyProviderError) {
      r.ok         = false;
      r.status     = kStatusProviderError;
      r.error_code = kErrorPayloadDecode;
    } else {
      r.ok     = false;
      r.status = kStatusProviderError;
      r.error  = "failed";
    }
    return r;
  }

  std::atomic<int>        describe_calls_{0};
  std::atomic<int>        score_calls_{0};
  std::atomic<int>        analyze_calls_{0};
  std::atomic<int>        cancel_calls_{0};
  std::atomic<int>        register_calls_{0};
  std::atomic<int>        list_models_calls_{0};
  std::atomic<int>        revoke_calls_{0};
  std::atomic<int64_t>    last_registered_ttl_ms_{-1};
  std::string             last_list_models_credential_ref_;
  std::string             last_output_language_;
  std::string             last_rating_severity_;
  std::string             last_camera_context_;
  Outcome                 describe_outcome_ = Outcome::kSuccess;
  Outcome                 score_outcome_    = Outcome::kSuccess;
  bool                    usage_enabled_    = true;
  std::atomic<bool>       block_mode_{false};
  std::atomic<bool>       release_blocked_{false};
  std::mutex              block_mutex_;
  std::condition_variable block_cv_;
};

class CountingCredentialStore final : public alcedo::IAiCredentialStore {
 public:
  auto SaveCredential(const std::string& slot, const std::string& secret, std::string* error)
      -> bool override {
    return inner_.SaveCredential(slot, secret, error);
  }
  auto LoadCredential(const std::string& slot, std::string* secret, std::string* error)
      -> bool override {
    ++load_calls_;
    return inner_.LoadCredential(slot, secret, error);
  }
  auto DeleteCredential(const std::string& slot, std::string* error) -> bool override {
    return inner_.DeleteCredential(slot, error);
  }
  auto HasCredential(const std::string& slot) -> bool override {
    ++has_calls_;
    return inner_.HasCredential(slot);
  }

  auto LoadCalls() const -> int { return load_calls_.load(); }
  auto HasCalls() const -> int { return has_calls_.load(); }

 private:
  alcedo::InMemoryAiCredentialStore inner_;
  std::atomic<int>                  load_calls_{0};
  std::atomic<int>                  has_calls_{0};
};

// Phase 7a fake IImageAnalysisSink — records every call so "no upsert on failure /
// cancel" is a one-liner assertion, and so the controller's job-end persistence wiring
// (PersistUnderstanding / PersistRatingReasons + ApplyStarRating, then Flush / Notify)
// can be verified without a live project / DB.
class FakeSink : public IImageAnalysisSink {
 public:
  struct StarCall {
    uint32_t elementId;
    uint32_t imageId;
    int      rating;
  };

  bool PersistUnderstanding(const alcedo::ImageAnalysisItemResult& r) override {
    std::lock_guard lk(mu_);
    ++persist_understanding_calls_;
    understanding_element_ids_.push_back(r.item.element_id);
    return persist_ok_;
  }
  bool PersistRatingReasons(const alcedo::ImageAnalysisItemResult& r) override {
    std::lock_guard lk(mu_);
    ++persist_reasons_calls_;
    reasons_element_ids_.push_back(r.item.element_id);
    return persist_ok_;
  }
  bool ApplyStarRating(uint32_t elementId, uint32_t imageId, int rating) override {
    std::lock_guard lk(mu_);
    ++apply_star_calls_;
    star_calls_.push_back({elementId, imageId, rating});
    return true;
  }
  void FlushPendingStarRatings() override {
    std::lock_guard lk(mu_);
    ++flush_calls_;
  }
  void NotifySearchDocumentChanged() override {
    std::lock_guard lk(mu_);
    ++notify_calls_;
  }

  void SetPersistOk(bool ok) { persist_ok_ = ok; }

  auto PersistUnderstandingCalls() const -> int {
    std::lock_guard lk(mu_);
    return persist_understanding_calls_;
  }
  auto PersistReasonsCalls() const -> int {
    std::lock_guard lk(mu_);
    return persist_reasons_calls_;
  }
  auto ApplyStarCalls() const -> int {
    std::lock_guard lk(mu_);
    return apply_star_calls_;
  }
  auto FlushCalls() const -> int {
    std::lock_guard lk(mu_);
    return flush_calls_;
  }
  auto NotifyCalls() const -> int {
    std::lock_guard lk(mu_);
    return notify_calls_;
  }
  auto TotalCalls() const -> int {
    std::lock_guard lk(mu_);
    return persist_understanding_calls_ + persist_reasons_calls_ + apply_star_calls_ +
           flush_calls_ + notify_calls_;
  }
  auto StarCalls() const -> std::vector<StarCall> {
    std::lock_guard lk(mu_);
    return star_calls_;
  }
  auto UnderstandingElementIds() const -> std::vector<uint32_t> {
    std::lock_guard lk(mu_);
    return understanding_element_ids_;
  }
  auto ReasonsElementIds() const -> std::vector<uint32_t> {
    std::lock_guard lk(mu_);
    return reasons_element_ids_;
  }

 private:
  mutable std::mutex    mu_;
  int                   persist_understanding_calls_ = 0;
  int                   persist_reasons_calls_       = 0;
  int                   apply_star_calls_            = 0;
  int                   flush_calls_                 = 0;
  int                   notify_calls_                = 0;
  bool                  persist_ok_                  = true;
  std::vector<StarCall> star_calls_;
  std::vector<uint32_t> understanding_element_ids_;
  std::vector<uint32_t> reasons_element_ids_;
};
class FakeEnv : public IImageAnalysisEnvironment {
 public:
  FakeEnv(std::shared_ptr<FakeThumbProvider> thumbs, std::shared_ptr<FakeClient> client,
          std::shared_ptr<alcedo::ImageAnalysisConcurrencyLimit> concurrency_limit,
          std::shared_ptr<alcedo::IAiCredentialStore>            store)
      : thumbs_(std::move(thumbs)),
        client_(std::move(client)),
        concurrency_limit_(std::move(concurrency_limit)),
        store_(std::move(store)) {}

  auto ThumbnailProvider() -> std::shared_ptr<alcedo::IAnalysisRenditionProvider> override {
    return thumbs_;
  }
  auto AnalysisClient() -> std::shared_ptr<alcedo::IImageAnalysisClient> override {
    return client_;
  }
  auto CredentialStore() -> std::shared_ptr<alcedo::IAiCredentialStore> override { return store_; }
  auto ConcurrencyLimit() -> std::shared_ptr<alcedo::ImageAnalysisConcurrencyLimit> override {
    return concurrency_limit_;
  }
  auto CameraContextForItem(const alcedo::ImageAnalysisItem& item) -> std::string override {
    const auto it = camera_contexts_.find(item.image_id);
    return it == camera_contexts_.end() ? std::string{} : it->second;
  }
  auto AcquireSidecarLease() -> std::shared_ptr<void> override {
    sidecar_lease_count_.fetch_add(1);
    return std::shared_ptr<void>(new int(0), [this](int* token) {
      delete token;
      sidecar_lease_count_.fetch_sub(1);
    });
  }
  auto EnsureSidecarReady(bool provider_configs_dirty, std::string*) -> bool override {
    last_provider_configs_dirty_ = provider_configs_dirty;
    sidecar_ensured_             = true;
    return sidecar_ready_;
  }
  void SetSidecarReady(bool r) { sidecar_ready_ = r; }
  void SetCameraContext(uint32_t image_id, std::string context) {
    camera_contexts_[image_id] = std::move(context);
  }
  auto SidecarEnsured() const -> bool { return sidecar_ensured_.load(); }
  auto LastProviderConfigsDirty() const -> bool { return last_provider_configs_dirty_.load(); }
  auto SidecarLeaseCount() const -> int { return sidecar_lease_count_.load(); }

 private:
  std::shared_ptr<FakeThumbProvider>                 thumbs_;
  std::shared_ptr<FakeClient>                        client_;
  std::shared_ptr<alcedo::ImageAnalysisConcurrencyLimit> concurrency_limit_;
  std::shared_ptr<alcedo::IAiCredentialStore>        store_;
  std::unordered_map<uint32_t, std::string>          camera_contexts_;
  std::atomic<bool>                                  sidecar_ensured_{false};
  std::atomic<bool>                                  last_provider_configs_dirty_{false};
  std::atomic<int>                                   sidecar_lease_count_{0};
  bool                                               sidecar_ready_ = true;
};

auto Targets(std::vector<std::pair<uint, uint>> ids) -> QVariantList {
  QVariantList out;
  for (auto [eid, iid] : ids) {
    QVariantMap m;
    m.insert("elementId", eid);
    m.insert("imageId", iid);
    out.push_back(m);
  }
  return out;
}

// Pump the event loop until the controller is no longer running (the worker's
// on_finished is delivered to the main thread via Qt::QueuedConnection) or the
// timeout elapses. Returns true if the controller finished.
auto WaitForFinished(ImageAnalysisController& controller, std::chrono::milliseconds timeout)
    -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (controller.Running()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  QCoreApplication::processEvents();
  return true;
}

auto SpinWaitFor(std::function<bool()> pred, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents();
    if (pred()) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  QCoreApplication::processEvents();
  return pred();
}

struct EnvBundle {
  QTemporaryDir                            temp_dir;
  std::shared_ptr<CountingCredentialStore> store = std::make_shared<CountingCredentialStore>();
  alcedo::AiProviderProfileController      profiles;
  std::shared_ptr<FakeThumbProvider>       thumbs = std::make_shared<FakeThumbProvider>();
  std::shared_ptr<FakeClient>              client = std::make_shared<FakeClient>();
  std::shared_ptr<alcedo::ImageAnalysisConcurrencyLimit> concurrency_limit =
      std::make_shared<alcedo::ImageAnalysisConcurrencyLimit>();
  std::shared_ptr<FakeSink> sink = std::make_shared<FakeSink>();
  std::shared_ptr<FakeEnv>  env;

  EnvBundle()
      : profiles(FsPathFromQStringForTest(temp_dir.filePath(QStringLiteral("ai_providers.json"))),
                 FsPathFromQStringForTest(temp_dir.filePath(QStringLiteral("provider_configs"))),
                 store) {
    const QString id = profiles.AddProfileFromTemplate(QStringLiteral("opencode_go_anthropic"));
    EXPECT_FALSE(id.isEmpty());
    QVariantMap discovered;
    discovered.insert(QStringLiteral("modelId"), QStringLiteral("qwen3.7-plus"));
    discovered.insert(QStringLiteral("displayName"), QStringLiteral("Qwen3.7 Plus"));
    discovered.insert(QStringLiteral("supportsVision"), true);
    discovered.insert(QStringLiteral("supportsStructuredOutput"), true);
    profiles.SetDiscoveredModels(id, QVariantList{discovered});
    profiles.SetProfileField(id, QStringLiteral("modelId"), QStringLiteral("qwen3.7-plus"));
    store->SaveCredential(ActiveCredentialSlot(), "sk-fake-test-key", nullptr);
    env = std::make_shared<FakeEnv>(thumbs, client, concurrency_limit, store);
  }

  auto ActiveCredentialSlot() const -> std::string {
    return profiles.Profile(profiles.ActiveProfileId())
        .value(QStringLiteral("credentialSlot"))
        .toString()
        .toStdString();
  }
};

class ImageAnalysisControllerTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    static int    argc = 0;
    static char** argv = nullptr;
    if (app_ == nullptr) {
      app_ = new QCoreApplication(argc, argv);
    }
    QCoreApplication::setOrganizationName(QStringLiteral("PuerhLabTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ImageAnalysisControllerTest"));
  }

  // A fresh controller + env per case. The profile controller lives in the
  // bundle (kept alive in bundles_) so the controller's raw pointer stays valid.
  auto MakeController() -> std::unique_ptr<ImageAnalysisController> {
    auto bundle = std::make_shared<EnvBundle>();
    bundles_.push_back(bundle);
    last_bundle_ = bundle;
    return std::make_unique<ImageAnalysisController>(bundle->env, &bundle->profiles, bundle->sink);
  }

  static QCoreApplication*                app_;
  std::vector<std::shared_ptr<EnvBundle>> bundles_;
  std::shared_ptr<EnvBundle>              last_bundle_;
};

QCoreApplication* ImageAnalysisControllerTest::app_ = nullptr;

// ── Required 6d cases ────────────────────────────────────────────────────────

TEST_F(ImageAnalysisControllerTest, EmptySelectionSetsErrorAndDoesNotStart) {
  auto controller = MakeController();
  controller->StartDescribeForTargets(QVariantList{});
  EXPECT_FALSE(controller->Running());
  EXPECT_FALSE(controller->LastError().isEmpty());
  EXPECT_EQ(last_bundle_->client->DescribeCalls(), 0);
  EXPECT_FALSE(last_bundle_->env->SidecarEnsured());
}

TEST_F(ImageAnalysisControllerTest, RefreshCredentialStateUsesHasCredentialWithoutLoadingSecret) {
  auto controller = MakeController();
  EXPECT_TRUE(controller->CredentialAvailable());
  EXPECT_EQ(last_bundle_->store->LoadCalls(), 0);
  EXPECT_GE(last_bundle_->store->HasCalls(), 1);

  controller->RefreshCredentialState();
  EXPECT_TRUE(controller->CredentialAvailable());
  EXPECT_EQ(last_bundle_->store->LoadCalls(), 0);
  EXPECT_GE(last_bundle_->store->HasCalls(), 2);
}

TEST_F(ImageAnalysisControllerTest, ErrorClearsPreviousResultsAndCounts) {
  auto controller = MakeController();
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  ASSERT_EQ(controller->Analyzed(), 1);
  ASSERT_EQ(controller->LastResults().size(), 1);

  controller->StartDescribeForTargets(QVariantList{});
  EXPECT_FALSE(controller->Running());
  EXPECT_FALSE(controller->LastError().isEmpty());
  EXPECT_EQ(controller->Total(), 0);
  EXPECT_EQ(controller->Analyzed(), 0);
  EXPECT_EQ(controller->Failed(), 0);
  EXPECT_EQ(controller->Canceled(), 0);
  EXPECT_FALSE(controller->CanRetry());
  EXPECT_TRUE(controller->LastResults().isEmpty());
}

TEST_F(ImageAnalysisControllerTest, ValidateConnectionMissingCredentialDoesNotStartSidecar) {
  auto controller = MakeController();
  ASSERT_TRUE(last_bundle_->store->DeleteCredential(last_bundle_->ActiveCredentialSlot(), nullptr));

  controller->ValidateConnection();
  EXPECT_FALSE(controller->Running());
  EXPECT_FALSE(controller->LastError().isEmpty());
  EXPECT_FALSE(controller->CredentialAvailable());
  EXPECT_FALSE(last_bundle_->env->SidecarEnsured());
  EXPECT_EQ(last_bundle_->client->RegisterCalls(), 0);
  EXPECT_EQ(last_bundle_->client->ListModelsCalls(), 0);
}

TEST_F(ImageAnalysisControllerTest, ValidateConnectionForNoneCredentialProfileRefreshesModels) {
  auto          controller = MakeController();
  const QString profile_id =
      last_bundle_->profiles.AddProfileFromTemplate(QStringLiteral("ccswitch_openai"));
  ASSERT_FALSE(profile_id.isEmpty());
  ASSERT_TRUE(last_bundle_->profiles.SetActiveProfile(profile_id));

  controller->ValidateConnectionForProfile(profile_id);

  ASSERT_TRUE(SpinWaitFor([&] { return !controller->ConnectionStatus().isEmpty(); },
                          std::chrono::seconds(5)));
  EXPECT_TRUE(controller->LastError().isEmpty());
  EXPECT_TRUE(controller->CredentialAvailable());
  EXPECT_TRUE(last_bundle_->env->SidecarEnsured());
  EXPECT_EQ(last_bundle_->client->RegisterCalls(), 0);
  EXPECT_EQ(last_bundle_->client->ListModelsCalls(), 1);
  EXPECT_EQ(last_bundle_->client->LastListModelsCredentialRef(), "");
  EXPECT_FALSE(controller->DiscoveredModels().isEmpty());
}

TEST_F(ImageAnalysisControllerTest, OneImageDescribeSucceeds) {
  auto controller = MakeController();
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 1);
  EXPECT_EQ(controller->Failed(), 0);
  EXPECT_EQ(last_bundle_->client->DescribeCalls(), 1);
  ASSERT_EQ(controller->LastResults().size(), 1);
  const auto r = controller->LastResults().front().toMap();
  EXPECT_EQ(r.value("status").toString(), QStringLiteral("analyzed"));
  EXPECT_EQ(r.value("caption").toString(), QStringLiteral("a caption"));
}

TEST_F(ImageAnalysisControllerTest, AnalysisJobUsesSidecarDefaultCredentialTtl) {
  auto controller = MakeController();
  controller->StartAnalyzeForTargets(Targets({{1, 100}, {2, 200}, {3, 300}, {4, 400}}), true);
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(15)));
  EXPECT_EQ(controller->Analyzed(), 4);
  EXPECT_EQ(controller->Failed(), 0);
  EXPECT_EQ(last_bundle_->client->RegisterCalls(), 1);
  EXPECT_EQ(last_bundle_->client->LastRegisteredTtlMs(), 0);
}

TEST_F(ImageAnalysisControllerTest, MultiImageDescribeSucceeds) {
  auto controller = MakeController();
  controller->StartDescribeForTargets(Targets({{1, 100}, {2, 200}, {3, 300}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(15)));
  EXPECT_EQ(controller->Analyzed(), 3);
  EXPECT_EQ(controller->Failed(), 0);
  EXPECT_EQ(last_bundle_->client->DescribeCalls(), 3);
}

TEST_F(ImageAnalysisControllerTest, CancelRunningAnalysisDiscardsResult) {
  auto controller = MakeController();
  last_bundle_->client->SetBlockMode(true);
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  // Wait until the provider call has started (the item holds the concurrency limit).
  ASSERT_TRUE(SpinWaitFor([&] { return last_bundle_->client->DescribeCalls() >= 1; },
                          std::chrono::seconds(5)));
  controller->CancelAnalysis();
  last_bundle_->client->ReleaseBlock();
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 0);
  EXPECT_GE(controller->Canceled(), 1);
}

TEST_F(ImageAnalysisControllerTest, RetryLastReRunsTargets) {
  auto controller = MakeController();
  // First run: schema error -> failed, canRetry true.
  last_bundle_->client->SetDescribeOutcome(FakeClient::Outcome::kSchemaError);
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 0);
  EXPECT_EQ(controller->Failed(), 1);
  EXPECT_TRUE(controller->CanRetry());

  // Retry: switch to success and re-run the same targets.
  last_bundle_->client->SetDescribeOutcome(FakeClient::Outcome::kSuccess);
  controller->RetryLast();
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 1);
  EXPECT_EQ(controller->Failed(), 0);
}

TEST_F(ImageAnalysisControllerTest, ProviderErrorPropagatesAndNoActiveAnnotation) {
  auto controller = MakeController();
  last_bundle_->client->SetDescribeOutcome(FakeClient::Outcome::kUnsupported);
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 0);
  EXPECT_EQ(controller->Failed(), 1);
  ASSERT_EQ(controller->LastResults().size(), 1);
  EXPECT_EQ(controller->LastResults().front().toMap().value("status").toString(),
            QStringLiteral("error"));
}

TEST_F(ImageAnalysisControllerTest, SchemaErrorPropagatesAndNoActiveAnnotation) {
  auto controller = MakeController();
  last_bundle_->client->SetDescribeOutcome(FakeClient::Outcome::kSchemaError);
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 0);
  EXPECT_EQ(controller->Failed(), 1);
  EXPECT_EQ(controller->LastResults().front().toMap().value("status").toString(),
            QStringLiteral("error"));
}

TEST_F(ImageAnalysisControllerTest, EmptyProviderErrorStillSurfacesDiagnosticFields) {
  auto controller = MakeController();
  last_bundle_->client->SetDescribeOutcome(FakeClient::Outcome::kEmptyProviderError);
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 0);
  EXPECT_EQ(controller->Failed(), 1);
  ASSERT_EQ(controller->LastResults().size(), 1);
  const auto r = controller->LastResults().front().toMap();
  EXPECT_EQ(r.value("status").toString(), QStringLiteral("error"));
  EXPECT_EQ(r.value("providerStatus").toInt(), kStatusProviderError);
  EXPECT_EQ(r.value("providerErrorCode").toInt(), kErrorPayloadDecode);
  EXPECT_TRUE(r.value("error").toString().contains(QStringLiteral("status")));
  EXPECT_TRUE(r.value("error").toString().contains(QStringLiteral("fake")));
}

TEST_F(ImageAnalysisControllerTest, StructuredOutputNoneProviderDoesNotStartSidecar) {
  auto          controller = MakeController();
  const QString profile_id =
      last_bundle_->profiles.AddProfileFromTemplate(QStringLiteral("custom"));
  ASSERT_FALSE(profile_id.isEmpty());
  ASSERT_TRUE(last_bundle_->profiles.SetProfileField(
      profile_id, QStringLiteral("structuredOutputMode"), QStringLiteral("none")));
  ASSERT_TRUE(last_bundle_->profiles.SetActiveProfile(profile_id));

  controller->StartScoreForTargets(Targets({{1, 100}}), true);

  EXPECT_FALSE(controller->Running());
  EXPECT_FALSE(controller->LastError().isEmpty());
  EXPECT_TRUE(controller->LastError().contains(QStringLiteral("structured output")));
  EXPECT_FALSE(last_bundle_->env->SidecarEnsured());
  EXPECT_EQ(last_bundle_->client->ScoreCalls(), 0);
}

TEST_F(ImageAnalysisControllerTest, MissingCredentialSetsErrorAndDoesNotStart) {
  auto controller = MakeController();
  // Wipe the credential so LoadCredential fails before the sidecar is touched.
  ASSERT_TRUE(last_bundle_->store->DeleteCredential(last_bundle_->ActiveCredentialSlot(), nullptr));
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  EXPECT_FALSE(controller->Running());
  EXPECT_FALSE(controller->LastError().isEmpty());
  EXPECT_FALSE(controller->CredentialAvailable());
  EXPECT_FALSE(last_bundle_->env->SidecarEnsured());
  EXPECT_EQ(last_bundle_->client->DescribeCalls(), 0);
}

TEST_F(ImageAnalysisControllerTest, ScoreTaskReturnsRating) {
  auto controller = MakeController();
  controller->StartScoreForTargets(Targets({{1, 100}}), true);
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 1);
  EXPECT_EQ(last_bundle_->client->ScoreCalls(), 1);
  ASSERT_EQ(controller->LastResults().size(), 1);
  const auto r = controller->LastResults().front().toMap();
  EXPECT_EQ(r.value("status").toString(), QStringLiteral("analyzed"));
  const int rating = r.value("rating").toInt();
  EXPECT_GE(rating, 1);
  EXPECT_LE(rating, 5);
}

TEST_F(ImageAnalysisControllerTest, AnalyzeTaskUsesSingleCallAndPersistsBothOutputs) {
  auto controller = MakeController();
  controller->StartAnalyzeForTargets(Targets({{1, 100}}), true);
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));

  EXPECT_EQ(controller->Analyzed(), 1);
  EXPECT_EQ(last_bundle_->client->AnalyzeCalls(), 1);
  EXPECT_EQ(last_bundle_->client->DescribeCalls(), 0);
  EXPECT_EQ(last_bundle_->client->ScoreCalls(), 0);
  ASSERT_EQ(controller->LastResults().size(), 1);
  const auto r = controller->LastResults().front().toMap();
  EXPECT_EQ(r.value("status").toString(), QStringLiteral("analyzed"));
  EXPECT_EQ(r.value("caption").toString(), QStringLiteral("a caption"));
  EXPECT_EQ(r.value("rating").toInt(), 4);
  EXPECT_EQ(r.value("reasons").toString(), QStringLiteral("strong composition"));

  auto& sink = *last_bundle_->sink;
  EXPECT_EQ(sink.PersistUnderstandingCalls(), 1);
  EXPECT_EQ(sink.PersistReasonsCalls(), 1);
  EXPECT_EQ(sink.ApplyStarCalls(), 1);
  EXPECT_EQ(sink.NotifyCalls(), 1);
  EXPECT_EQ(sink.FlushCalls(), 1);
}

TEST_F(ImageAnalysisControllerTest, SharedGateSerializesTwoConcurrentRuns) {
  // Two controllers over ONE shared env (shared concurrency limit + shared fake client). The
  // Phase 6d mandate: remote calls serialize app-wide, not per service instance.
  auto bundle = std::make_shared<EnvBundle>();
  bundles_.push_back(bundle);

  ImageAnalysisController a(bundle->env, &bundle->profiles, bundle->sink);
  ImageAnalysisController b(bundle->env, &bundle->profiles, bundle->sink);

  bundle->client->SetBlockMode(true);
  a.StartDescribeForTargets(Targets({{1, 100}}));
  // Wait until A's provider call is in flight (A holds the shared concurrency limit).
  ASSERT_TRUE(
      SpinWaitFor([&] { return bundle->client->DescribeCalls() >= 1; }, std::chrono::seconds(5)));

  b.StartDescribeForTargets(Targets({{2, 200}}));
  // B must NOT reach the provider while A holds the concurrency limit.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  QCoreApplication::processEvents();
  EXPECT_EQ(bundle->client->DescribeCalls(), 1);

  // Release A; B may then acquire the slot and run.
  bundle->client->ReleaseBlock();
  ASSERT_TRUE(WaitForFinished(a, std::chrono::seconds(10)));
  ASSERT_TRUE(WaitForFinished(b, std::chrono::seconds(10)));
  EXPECT_EQ(a.Analyzed(), 1);
  EXPECT_EQ(b.Analyzed(), 1);
  EXPECT_EQ(bundle->client->DescribeCalls(), 2);
}

// ── Phase 7a: persistence sink wiring ────────────────────────────────────────

// (a) Describe success persists one understanding per analyzed image and notifies the
// search document at job end; no rating path is touched.
TEST_F(ImageAnalysisControllerTest, DescribeSuccessPersistsUnderstandingAndNotifiesSearch) {
  auto controller = MakeController();
  controller->StartDescribeForTargets(Targets({{1, 100}, {2, 200}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(15)));
  EXPECT_EQ(controller->Analyzed(), 2);

  auto& sink = *last_bundle_->sink;
  EXPECT_EQ(sink.PersistUnderstandingCalls(), 2);
  EXPECT_EQ(sink.UnderstandingElementIds(), (std::vector<uint32_t>{1, 2}));
  EXPECT_EQ(sink.PersistReasonsCalls(), 0);
  EXPECT_EQ(sink.ApplyStarCalls(), 0);
  EXPECT_EQ(sink.FlushCalls(), 0);
  EXPECT_EQ(sink.NotifyCalls(), 1);
}

// (b) Score success persists reasons + applies the star per analyzed image, then flushes
// the batched star writes at job end; no understanding / search-notify path is touched.
TEST_F(ImageAnalysisControllerTest, ScoreSuccessPersistsReasonsAppliesStarAndFlushes) {
  auto controller = MakeController();
  controller->StartScoreForTargets(Targets({{7, 700}, {8, 800}}), true);
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(15)));
  EXPECT_EQ(controller->Analyzed(), 2);

  auto& sink = *last_bundle_->sink;
  EXPECT_EQ(sink.PersistReasonsCalls(), 2);
  EXPECT_EQ(sink.ReasonsElementIds(), (std::vector<uint32_t>{7, 8}));
  EXPECT_EQ(sink.ApplyStarCalls(), 2);
  const auto stars = sink.StarCalls();
  ASSERT_EQ(stars.size(), 2u);
  EXPECT_EQ(stars[0].rating, 4);
  EXPECT_GE(stars[0].rating, 1);
  EXPECT_LE(stars[0].rating, 5);
  EXPECT_EQ(sink.FlushCalls(), 1);
  EXPECT_EQ(sink.PersistUnderstandingCalls(), 0);
  EXPECT_EQ(sink.NotifyCalls(), 0);
}

// (c) A provider error, schema error, and cancellation each produce ZERO sink calls — no
// active annotation is left behind.
TEST_F(ImageAnalysisControllerTest, ProviderErrorMakesNoSinkCalls) {
  auto controller = MakeController();
  last_bundle_->client->SetDescribeOutcome(FakeClient::Outcome::kUnsupported);
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 0);
  EXPECT_EQ(controller->Failed(), 1);
  EXPECT_EQ(last_bundle_->sink->TotalCalls(), 0);
}

TEST_F(ImageAnalysisControllerTest, SchemaErrorMakesNoSinkCalls) {
  auto controller = MakeController();
  last_bundle_->client->SetDescribeOutcome(FakeClient::Outcome::kSchemaError);
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(controller->Analyzed(), 0);
  EXPECT_EQ(controller->Failed(), 1);
  EXPECT_EQ(last_bundle_->sink->TotalCalls(), 0);
}

TEST_F(ImageAnalysisControllerTest, CancelMakesNoSinkCalls) {
  auto controller = MakeController();
  last_bundle_->client->SetBlockMode(true);
  controller->StartScoreForTargets(Targets({{1, 100}}), true);
  ASSERT_TRUE(SpinWaitFor([&] { return last_bundle_->client->ScoreCalls() >= 1; },
                          std::chrono::seconds(5)));
  controller->CancelAnalysis();
  last_bundle_->client->ReleaseBlock();
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_GE(controller->Canceled(), 1);
  EXPECT_EQ(last_bundle_->sink->TotalCalls(), 0);
}

// (d) lastUsage aggregates tokens + provider request ids and counts items with/without
// usage metadata.
TEST_F(ImageAnalysisControllerTest, LastUsageAggregatesTokensAndRequestIds) {
  auto controller = MakeController();
  controller->StartDescribeForTargets(Targets({{1, 100}, {2, 200}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(15)));
  const auto usage = controller->LastUsage();
  EXPECT_EQ(usage.value("inputTokens").toLongLong(), 20);   // 10 * 2
  EXPECT_EQ(usage.value("outputTokens").toLongLong(), 40);  // 20 * 2
  EXPECT_EQ(usage.value("totalTokens").toLongLong(), 60);   // 30 * 2
  EXPECT_EQ(usage.value("itemsWithUsage").toInt(), 2);
  EXPECT_EQ(usage.value("itemsWithoutUsage").toInt(), 0);
  const auto ids = usage.value("providerRequestIds").toList();
  EXPECT_EQ(ids.size(), 2);
}

TEST_F(ImageAnalysisControllerTest, LastUsageCountsItemsWithoutUsageMetadata) {
  auto controller = MakeController();
  last_bundle_->client->SetUsageEnabled(false);
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  const auto usage = controller->LastUsage();
  EXPECT_EQ(usage.value("totalTokens").toLongLong(), 0);
  EXPECT_EQ(usage.value("itemsWithUsage").toInt(), 0);
  EXPECT_EQ(usage.value("itemsWithoutUsage").toInt(), 1);
  EXPECT_EQ(usage.value("providerRequestIds").toList().size(), 0);
}

// (e) lastResults carries promptProfileId and providerRequestId on each item.
TEST_F(ImageAnalysisControllerTest, LastResultsCarryPromptProfileAndProviderRequestId) {
  auto controller = MakeController();
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  ASSERT_EQ(controller->LastResults().size(), 1);
  const auto r = controller->LastResults().front().toMap();
  EXPECT_EQ(r.value("promptProfileId").toString(), QStringLiteral("profile-v1"));
  EXPECT_TRUE(r.value("providerRequestId").toString().startsWith(QStringLiteral("req-desc-")));
}

// ── Output language (Frontend 1) ─────────────────────────────────────────────

TEST_F(ImageAnalysisControllerTest, OutputLanguageFollowResolvesToAppLanguage) {
  // Default profile output_language is "follow"; the test app has no ui/language
  // set, so QSettings returns "system" -> QLocale::system() (English on the CI
  // host). Either way it must resolve to "" / "en" / "zh", never "follow".
  auto controller = MakeController();
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  const std::string lang = last_bundle_->client->LastOutputLanguage();
  EXPECT_TRUE(lang == "en" || lang == "zh") << "follow must resolve to en/zh, got: " << lang;
  EXPECT_NE(lang, "follow");
}

TEST_F(ImageAnalysisControllerTest, OutputLanguageExplicitZhReachesProvider) {
  auto controller = MakeController();
  last_bundle_->profiles.SetOutputLanguage(QStringLiteral("zh"));
  controller->StartDescribeForTargets(Targets({{1, 100}}));
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(last_bundle_->client->LastOutputLanguage(), "zh");
}

TEST_F(ImageAnalysisControllerTest, OutputLanguageExplicitEnReachesProvider) {
  auto controller = MakeController();
  last_bundle_->profiles.SetOutputLanguage(QStringLiteral("en"));
  controller->StartScoreForTargets(Targets({{1, 100}}), true);
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  EXPECT_EQ(last_bundle_->client->LastOutputLanguage(), "en");
}

// ── Rating severity (评价严苛程度) ───────────────────────────────────────────

TEST_F(ImageAnalysisControllerTest, RatingSeverityDefaultsToNormal) {
  // Clear any residue from a prior test so the ctor reads the unset/default path.
  QSettings().remove(QStringLiteral("ai/analysis/ratingSeverity"));
  auto controller = MakeController();
  EXPECT_EQ(controller->RatingSeverity(), QStringLiteral("normal"));
}

TEST_F(ImageAnalysisControllerTest, SetRatingSeverityClampsUnknownToNormal) {
  QSettings().remove(QStringLiteral("ai/analysis/ratingSeverity"));
  auto controller = MakeController();
  EXPECT_TRUE(controller->SetRatingSeverity(QStringLiteral("bogus")));
  EXPECT_EQ(controller->RatingSeverity(), QStringLiteral("normal"));
  EXPECT_TRUE(controller->SetRatingSeverity(QStringLiteral("x_high")));
  EXPECT_EQ(controller->RatingSeverity(), QStringLiteral("xhigh"));
  EXPECT_TRUE(controller->SetRatingSeverity(QStringLiteral("HIGH")));
  EXPECT_EQ(controller->RatingSeverity(), QStringLiteral("high"));
  EXPECT_TRUE(controller->SetRatingSeverity(QStringLiteral("max")));
  EXPECT_EQ(controller->RatingSeverity(), QStringLiteral("max"));
  EXPECT_TRUE(controller->SetRatingSeverity(QStringLiteral("LITE")));
  EXPECT_EQ(controller->RatingSeverity(), QStringLiteral("lite"));
  // Cleanup so the persisted value does not leak into other cases.
  QSettings().remove(QStringLiteral("ai/analysis/ratingSeverity"));
}

TEST_F(ImageAnalysisControllerTest, RatingSeverityPersistsAndReachesScoreProvider) {
  QSettings().remove(QStringLiteral("ai/analysis/ratingSeverity"));
  auto controller = MakeController();
  ASSERT_TRUE(controller->SetRatingSeverity(QStringLiteral("max")));
  controller->StartScoreForTargets(Targets({{1, 100}}), true);
  ASSERT_TRUE(WaitForFinished(*controller, std::chrono::seconds(10)));
  // The severity is threaded into the ScoreImage request and reaches the provider.
  EXPECT_EQ(last_bundle_->client->LastRatingSeverity(), "max");

  // A fresh controller in the same process reads the persisted value back.
  auto controller2 = MakeController();
  EXPECT_EQ(controller2->RatingSeverity(), QStringLiteral("max"));
  // Cleanup so the persisted value does not leak into other cases.
  QSettings().remove(QStringLiteral("ai/analysis/ratingSeverity"));
}

TEST_F(ImageAnalysisControllerTest, CameraContextOnlyReachesGearSensitiveRatingRequests) {
  QSettings().remove(QStringLiteral("ai/analysis/ratingSeverity"));

  auto normal = MakeController();
  last_bundle_->env->SetCameraContext(100, "Camera/EXIF metadata: camera_make=Leica.");
  normal->StartScoreForTargets(Targets({{1, 100}}), true);
  ASSERT_TRUE(WaitForFinished(*normal, std::chrono::seconds(10)));
  EXPECT_TRUE(last_bundle_->client->LastCameraContext().empty());

  auto high = MakeController();
  last_bundle_->env->SetCameraContext(101, "Camera/EXIF metadata: camera_make=Hasselblad.");
  ASSERT_TRUE(high->SetRatingSeverity(QStringLiteral("high")));
  high->StartScoreForTargets(Targets({{1, 101}}), true);
  ASSERT_TRUE(WaitForFinished(*high, std::chrono::seconds(10)));
  EXPECT_TRUE(last_bundle_->client->LastCameraContext().empty());

  auto xhigh = MakeController();
  last_bundle_->env->SetCameraContext(102, "Camera/EXIF metadata: camera_make=Hasselblad.");
  ASSERT_TRUE(xhigh->SetRatingSeverity(QStringLiteral("xhigh")));
  xhigh->StartScoreForTargets(Targets({{1, 102}}), true);
  ASSERT_TRUE(WaitForFinished(*xhigh, std::chrono::seconds(10)));
  EXPECT_EQ(last_bundle_->client->LastCameraContext(),
            "Camera/EXIF metadata: camera_make=Hasselblad.");

  auto max = MakeController();
  last_bundle_->env->SetCameraContext(103, "Camera/EXIF metadata: camera_make=Leica.");
  ASSERT_TRUE(max->SetRatingSeverity(QStringLiteral("max")));
  max->StartScoreForTargets(Targets({{1, 103}}), true);
  ASSERT_TRUE(WaitForFinished(*max, std::chrono::seconds(10)));
  EXPECT_EQ(last_bundle_->client->LastCameraContext(), "Camera/EXIF metadata: camera_make=Leica.");

  auto describe = MakeController();
  last_bundle_->env->SetCameraContext(104, "Camera/EXIF metadata: camera_make=Leica.");
  ASSERT_TRUE(describe->SetRatingSeverity(QStringLiteral("max")));
  describe->StartDescribeForTargets(Targets({{1, 104}}));
  ASSERT_TRUE(WaitForFinished(*describe, std::chrono::seconds(10)));
  EXPECT_TRUE(last_bundle_->client->LastCameraContext().empty());

  QSettings().remove(QStringLiteral("ai/analysis/ratingSeverity"));
}

// ── Discovered models surface (Frontend 1) ───────────────────────────────────

TEST_F(ImageAnalysisControllerTest, ValidateConnectionPopulatesDiscoveredModels) {
  auto controller = MakeController();
  controller->ValidateConnection();
  ASSERT_TRUE(SpinWaitFor([&] { return !controller->ConnectionStatus().isEmpty(); },
                          std::chrono::seconds(5)));
  EXPECT_FALSE(controller->DiscoveredModels().isEmpty());
  const auto first = controller->DiscoveredModels().first().toMap();
  EXPECT_EQ(first.value("modelId").toString(), QStringLiteral("fake-model"));
  EXPECT_TRUE(controller->ConnectionStatus().contains(QStringLiteral("Connected")));
}

}  // namespace
}  // namespace alcedo::ui
