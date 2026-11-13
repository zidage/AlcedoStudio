//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_sidecar_runtime_service.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QMetaObject>
#include <QThread>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "app/project_service.hpp"
#include "sidecar_client/dto/model_manager.hpp"
#include "sidecar_client/dto/semantic_embedding.hpp"
#include "support/test_temp_directory.hpp"
#include "utils/diagnostics/app_logging.hpp"

namespace alcedo {
namespace {

struct FakeSidecarState {
  std::atomic<bool> ready{true};
  std::atomic<bool> model_info_ready{true};
  std::atomic<int>  ping_count{0};
  std::atomic<int>  get_model_info_calls{0};
  // Ping blocks this long, like a gRPC call to a process that does not listen yet.
  std::atomic<int>  ping_delay_ms{0};
  std::atomic<int>  embed_text_calls{0};
  std::atomic<int>  embed_image_batch_calls{0};
};

class FakeModelManagerClient final : public sidecar_client::ModelManagerClient {
 public:
  auto ListModelProfiles(const std::string& model_root, std::chrono::milliseconds timeout,
                         std::string* error) -> std::vector<SemanticModelProfileInfo> override {
    (void)timeout;
    (void)error;
    SemanticModelProfileInfo mobile;
    mobile.profile_id          = "mobileclip2-s2-en";
    mobile.model_id            = "plhery/mobileclip2-onnx:s2";
    mobile.language            = "en";
    mobile.embedding_dimension = 512;
    mobile.local_root          = model_root + "/mobileclip2-s2-en";

    SemanticModelProfileInfo multilingual;
    multilingual.profile_id                 = "jina-clip-v2-int8-multilingual";
    multilingual.model_id                   = "jinaai/jina-clip-v2";
    multilingual.language                   = "multilingual";
    multilingual.embedding_dimension        = 512;
    multilingual.native_embedding_dimension = 1024;
    multilingual.embedding_transform        = "matryoshka_truncate_then_l2_normalize";
    multilingual.local_root                 = model_root + "/jina-clip-v2-int8-multilingual";
    return {mobile, multilingual};
  }

  auto ListInstalledModels(const std::string& model_root, std::chrono::milliseconds timeout,
                           std::string* error) -> std::vector<SemanticModelProfileInfo> override {
    auto profiles = ListModelProfiles(model_root, timeout, error);
    profiles.resize(1);
    profiles[0].installed = true;
    return profiles;
  }

  auto ValidateModel(const std::string& profile_id, const std::string& model_root,
                     std::chrono::milliseconds timeout) -> SemanticModelManagerResult override {
    (void)timeout;
    SemanticModelManagerResult result;
    result.ok                          = true;
    result.status                      = "installed";
    result.profile.profile_id          = profile_id;
    result.profile.local_root          = model_root + "/" + profile_id;
    result.profile.embedding_dimension = 512;
    result.manifest                    = SemanticResolvedModelManifest{
                           .profile_id                 = profile_id,
                           .model_id                   = "test/mobileclip",
                           .revision                   = "rev-a",
                           .engine_profile_id          = "mobileclip2-openclip",
                           .language                   = "en",
                           .embedding_dimension        = 512,
                           .native_embedding_dimension = 512,
                           .image_size                 = 256,
                           .embedding_transform        = "l2_normalize",
                           .model_root                 = result.profile.local_root,
                           .assets                     = {},
    };
    return result;
  }

  auto DeleteModel(const std::string& profile_id, const std::string& model_root,
                   std::chrono::milliseconds timeout) -> SemanticModelManagerResult override {
    (void)model_root;
    (void)timeout;
    SemanticModelManagerResult result;
    result.ok                 = true;
    result.status             = "deleted";
    result.profile.profile_id = profile_id;
    return result;
  }
};

class FakeSemanticEmbeddingClient final : public sidecar_client::SemanticEmbeddingClient {
 public:
  explicit FakeSemanticEmbeddingClient(std::shared_ptr<FakeSidecarState> state)
      : state_(std::move(state)) {}

  auto Ping(std::chrono::milliseconds timeout, std::string* error) -> bool override {
    (void)timeout;
    state_->ping_count.fetch_add(1);
    if (const int delay_ms = state_->ping_delay_ms.load(); delay_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    if (!state_->ready.load()) {
      if (error) *error = "fake runtime is not ready";
      return false;
    }
    return true;
  }

  auto GetRuntimeStatus(std::chrono::milliseconds timeout, AiSidecarRuntimeRemoteStatus* status,
                        std::string* error) -> bool override {
    (void)timeout;
    (void)error;
    if (status) {
      status->state               = "ready";
      status->provider            = "fake";
      status->image_batch_cap     = 8;
      status->image_batch_wait_ms = 2;
      status->uptime_ms           = 42;
    }
    return true;
  }

  auto GetModelInfo(std::chrono::milliseconds timeout, AiSidecarRuntimeModelInfo* info,
                    std::string* error) -> bool override {
    (void)timeout;
    state_->get_model_info_calls.fetch_add(1);
    if (!state_->model_info_ready.load()) {
      if (error) *error = "fake semantic model is unavailable";
      return false;
    }
    if (info) {
      info->profile_id                 = "mobileclip2-s2-en";
      info->model_id                   = "test/mobileclip";
      info->revision                   = "rev-a";
      info->engine_profile_id          = "mobileclip2-openclip";
      info->language                   = "en";
      info->embedding_dimension        = 512;
      info->native_embedding_dimension = 512;
      info->image_size                 = 256;
      info->embedding_transform        = "l2_normalize";
      info->provider                   = "fake";
      info->model_root                 = "test-model-root";
      info->prototype_config_hash      = "hash-a";
    }
    return true;
  }

  auto EmbedText(const std::string& request_id, const std::string& text,
                 std::chrono::milliseconds timeout) -> SemanticEmbeddingResult override {
    (void)text;
    (void)timeout;
    state_->embed_text_calls.fetch_add(1);
    return {.request_id = request_id,
            .embedding  = {1.0f, 0.0f},
            .dimension  = 2,
            .model_name = "test/mobileclip",
            .ok         = true};
  }

  auto EmbedTextBatch(const std::vector<SemanticTextEmbeddingRequest>& requests,
                      std::chrono::milliseconds                        timeout)
      -> std::vector<SemanticEmbeddingResult> override {
    (void)timeout;
    std::vector<SemanticEmbeddingResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      results.push_back(EmbedText(request.request_id, request.text, timeout));
    }
    return results;
  }

  auto EmbedImage(const std::string& request_id, const std::vector<uint8_t>& rgba8_image,
                  const std::string& format_hint, std::chrono::milliseconds timeout)
      -> SemanticEmbeddingResult override {
    (void)rgba8_image;
    (void)format_hint;
    (void)timeout;
    return {.request_id = request_id,
            .embedding  = {0.0f, 1.0f},
            .dimension  = 2,
            .model_name = "test/mobileclip",
            .ok         = true};
  }

  auto EmbedImageBatch(std::vector<SemanticImageEmbeddingRequest> requests,
                       std::chrono::milliseconds                  timeout)
      -> std::vector<SemanticEmbeddingResult> override {
    (void)timeout;
    state_->embed_image_batch_calls.fetch_add(1);
    std::vector<SemanticEmbeddingResult> results;
    results.reserve(requests.size());
    for (const auto& request : requests) {
      results.push_back(
          EmbedImage(request.request_id, request.rgba8_image, request.format_hint, timeout));
    }
    return results;
  }

 private:
  std::shared_ptr<FakeSidecarState> state_;
};

class FakeSidecarClient final : public sidecar_client::Client {
 public:
  explicit FakeSidecarClient(bool ready = true)
      : state_(std::make_shared<FakeSidecarState>()), semantic_(state_) {
    state_->ready = ready;
  }

  auto endpoint() const -> const std::string& override { return endpoint_; }
  auto models() -> sidecar_client::ModelManagerClient& override { return models_; }
  auto semantic() -> sidecar_client::SemanticEmbeddingClient& override { return semantic_; }

  void SetEndpoint(std::string endpoint) { endpoint_ = std::move(endpoint); }
  void SetReady(bool ready) { state_->ready.store(ready); }
  void SetModelInfoReady(bool ready) { state_->model_info_ready.store(ready); }
  void SetPingDelay(std::chrono::milliseconds delay) {
    state_->ping_delay_ms.store(static_cast<int>(delay.count()));
  }
  auto PingCount() const -> int { return state_->ping_count.load(); }
  auto GetModelInfoCalls() const -> int { return state_->get_model_info_calls.load(); }
  auto EmbedTextCalls() const -> int { return state_->embed_text_calls.load(); }
  auto EmbedImageBatchCalls() const -> int { return state_->embed_image_batch_calls.load(); }

 private:
  std::shared_ptr<FakeSidecarState> state_;
  std::string                       endpoint_;
  FakeModelManagerClient            models_;
  FakeSemanticEmbeddingClient       semantic_;
};

auto FakeFactory(const std::shared_ptr<FakeSidecarClient>& client) -> AiSidecarClientFactory {
  return [client](const std::string& endpoint) -> std::shared_ptr<sidecar_client::Client> {
    client->SetEndpoint(endpoint);
    return client;
  };
}

auto FakeRuntimePath() -> std::filesystem::path {
  return std::filesystem::path(ALCEDO_AI_SIDECAR_FAKE_RUNTIME_PATH);
}

auto BaseOptions() -> AiSidecarRuntimeOptions {
  AiSidecarRuntimeOptions options;
  options.runtime_binary  = FakeRuntimePath();
  options.model_root      = test::TestTempRoot() / test::CurrentTestTempDirectoryName() / "model";
  options.model_id        = "test/mobileclip";
  options.revision        = "rev-a";
  options.device          = "cpu";
  options.batch_cap       = 8;
  options.batch_wait_ms   = 2;
  options.startup_timeout = std::chrono::milliseconds(1000);
  options.health_poll_interval  = std::chrono::milliseconds(20);
  options.graceful_stop_timeout = std::chrono::milliseconds(100);
  options.kill_timeout          = std::chrono::milliseconds(1000);
  return options;
}

void SetModelRootEnv(const std::string& value) {
#ifdef _WIN32
  _putenv_s("ALCEDO_MIND_MODEL_ROOT", value.c_str());
#else
  if (value.empty()) {
    unsetenv("ALCEDO_MIND_MODEL_ROOT");
  } else {
    setenv("ALCEDO_MIND_MODEL_ROOT", value.c_str(), 1);
  }
#endif
}

struct CapturedLogMessage {
  QtMsgType   type = QtDebugMsg;
  std::string text;
};

/// Collects the `semantic.runtime.stdout` messages of `alcedo.semantic` while it exists.
class ScopedSidecarStdoutCapture final {
 public:
  ScopedSidecarStdoutCapture() {
    {
      std::lock_guard lock(Mutex());
      Store() = {};
    }
    previous_handler_ = qInstallMessageHandler(&ScopedSidecarStdoutCapture::Capture);
  }
  ~ScopedSidecarStdoutCapture() { qInstallMessageHandler(previous_handler_); }

  ScopedSidecarStdoutCapture(const ScopedSidecarStdoutCapture&)            = delete;
  ScopedSidecarStdoutCapture& operator=(const ScopedSidecarStdoutCapture&) = delete;

  auto                        Messages() const -> std::vector<CapturedLogMessage> {
    std::lock_guard lock(Mutex());
    return Store();
  }

 private:
  static auto Mutex() -> std::mutex& {
    static std::mutex mutex;
    return mutex;
  }
  static auto Store() -> std::vector<CapturedLogMessage>& {
    static std::vector<CapturedLogMessage> messages;
    return messages;
  }
  static void Capture(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    if (context.category == nullptr ||
        std::string_view(context.category) != std::string_view("alcedo.semantic") ||
        !message.startsWith(QStringLiteral("semantic.runtime.stdout"))) {
      return;
    }
    std::lock_guard lock(Mutex());
    Store().push_back(CapturedLogMessage{.type = type, .text = message.toStdString()});
  }

  QtMessageHandler previous_handler_ = nullptr;
};

/// Runs the owner thread event loop until the stdout tail of @p service holds @p marker.
auto WaitForStdoutTail(AiSidecarRuntimeService& service, std::string_view marker) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    if (service.Status().stdout_tail.find(marker) != std::string::npos) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

class ScopedModelRootEnv final {
 public:
  explicit ScopedModelRootEnv(const std::string& value) {
    const char* previous = std::getenv("ALCEDO_MIND_MODEL_ROOT");
    if (previous != nullptr) {
      previous_value_ = previous;
    }
    SetModelRootEnv(value);
  }

  ~ScopedModelRootEnv() { SetModelRootEnv(previous_value_.value_or(std::string{})); }

  ScopedModelRootEnv(const ScopedModelRootEnv&)            = delete;
  ScopedModelRootEnv& operator=(const ScopedModelRootEnv&) = delete;

 private:
  std::optional<std::string> previous_value_;
};

}  // namespace

TEST(AiSidecarRuntimeServiceTest, StartStopReportsReadyAndStopped) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));

  auto                    options = BaseOptions();
  options.extra_arguments         = {"--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kReady);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kNone);
  EXPECT_FALSE(status.endpoint.empty());
  EXPECT_GT(status.process_id, 0);
  ASSERT_TRUE(status.model_info.has_value());
  EXPECT_EQ(status.model_info->profile_id, "mobileclip2-s2-en");
  EXPECT_EQ(status.model_info->embedding_dimension, 512U);
  EXPECT_EQ(status.model_info->native_embedding_dimension, 512U);
  EXPECT_EQ(status.model_info->embedding_transform, "l2_normalize");
  ASSERT_TRUE(status.remote_status.has_value());
  EXPECT_EQ(status.remote_status->provider, "fake");
  EXPECT_EQ(service.ClientSession(), client);
  EXPECT_EQ(client->endpoint(), status.endpoint);

  service.Stop();
  status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kStopped);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kNone);
  EXPECT_EQ(status.process_id, 0);
  EXPECT_EQ(service.ClientSession(), nullptr);
}

TEST(AiSidecarRuntimeServiceTest, RunningSidecarReusesExistingProcessForDifferentOptions) {
  const test::TestTempDirectory temp_dir;
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));

  // A second lease is held, so the model identity change does not restart the process.
  auto                    other_lease = service.AcquireLease();
  auto                    lease   = service.AcquireLease();
  auto                    options = BaseOptions();
  options.extra_arguments         = {"--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  const auto first_pid = service.Status().process_id;
  ASSERT_GT(first_pid, 0);

  auto different_options       = options;
  different_options.model_id   = "another/model";
  different_options.revision   = "rev-b";
  different_options.model_root = temp_dir.Path() / "another_model_root";
  ASSERT_TRUE(service.StartAndWait(different_options));
  EXPECT_EQ(service.Status().process_id, first_pid);
  EXPECT_EQ(service.Options().model_id, "test/mobileclip");

  lease.reset();
  EXPECT_EQ(service.Status().state, AiSidecarRuntimeState::kReady);
  other_lease.reset();
  EXPECT_EQ(service.Status().state, AiSidecarRuntimeState::kStopped);
}

TEST(AiSidecarRuntimeServiceTest, LeasesStopRuntimeOnlyAfterLastRelease) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));

  auto                    lease_a = service.AcquireLease();
  auto                    lease_b = service.AcquireLease();
  auto                    options = BaseOptions();
  options.extra_arguments         = {"--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  const auto pid = service.Status().process_id;
  ASSERT_GT(pid, 0);

  lease_a.reset();
  EXPECT_EQ(service.Status().state, AiSidecarRuntimeState::kReady);
  EXPECT_EQ(service.Status().process_id, pid);

  lease_b.reset();
  EXPECT_EQ(service.Status().state, AiSidecarRuntimeState::kStopped);
  EXPECT_EQ(service.Status().process_id, 0);
}

TEST(AiSidecarRuntimeServiceTest, MissingBinaryFailsBeforeProcessStart) {
  const test::TestTempDirectory temp_dir;
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  auto                    options = BaseOptions();
  options.runtime_binary = temp_dir.Path() / "missing_alcedo_mind.exe";

  EXPECT_FALSE(service.StartAndWait(options));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kFailed);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kBinaryMissing);
  EXPECT_EQ(service.ClientSession(), nullptr);
}

TEST(AiSidecarRuntimeServiceTest, RuntimeExitBeforeReadyIsReported) {
  auto                    client = std::make_shared<FakeSidecarClient>(false);
  AiSidecarRuntimeService service(FakeFactory(client));
  auto                    options = BaseOptions();
  options.extra_arguments         = {"--exit-now", "--exit-code", "7"};

  EXPECT_FALSE(service.StartAndWait(options));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kFailed);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kRuntimeCrashed);
  EXPECT_NE(status.stderr_tail.find(""), std::string::npos);
}

TEST(AiSidecarRuntimeServiceTest, MissingClientSessionReportsClientUnavailable) {
  AiSidecarRuntimeService service(
      [](const std::string&) -> std::shared_ptr<sidecar_client::Client> { return nullptr; });
  auto options            = BaseOptions();
  options.extra_arguments = {"--sleep-ms", "30000"};

  EXPECT_FALSE(service.StartAndWait(options));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kFailed);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kClientUnavailable);
  EXPECT_EQ(status.process_id, 0);
  EXPECT_EQ(service.ClientSession(), nullptr);
}

TEST(AiSidecarRuntimeServiceTest, PingWithoutModelInfoDoesNotBecomeReady) {
  auto client = std::make_shared<FakeSidecarClient>();
  client->SetModelInfoReady(false);
  AiSidecarRuntimeService service(FakeFactory(client));

  auto                    options = BaseOptions();
  options.extra_arguments         = {"--sleep-ms", "30000"};
  options.startup_timeout         = std::chrono::milliseconds(120);
  options.health_poll_interval    = std::chrono::milliseconds(20);

  EXPECT_FALSE(service.StartAndWait(options));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kFailed);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kReadinessTimeout);
  EXPECT_NE(status.message.find("fake semantic model is unavailable"), std::string::npos);
  EXPECT_FALSE(status.model_info.has_value());
  EXPECT_EQ(service.ClientSession(), nullptr);
}

TEST(AiSidecarRuntimeServiceTest, ModelManagerRuntimeCanStartWithoutModelInfo) {
  auto client = std::make_shared<FakeSidecarClient>();
  client->SetModelInfoReady(false);
  AiSidecarRuntimeService service(FakeFactory(client));

  auto                    options = BaseOptions();
  options.extra_arguments         = {"--sleep-ms", "30000"};
  options.require_model_info      = false;

  ASSERT_TRUE(service.StartAndWait(options));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kReady);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kNone);
  EXPECT_FALSE(status.model_info.has_value());
  EXPECT_NE(status.message.find("model manager"), std::string::npos);
  service.Stop();
}

// GetModelInfo makes the sidecar prepare the CLIP model profile (#518): a start without the
// model information requirement (AI Mask) must not ask for it, and a later start that requires
// it asks the running process.
TEST(AiSidecarRuntimeServiceTest, StartWithoutModelInfoRequirementDoesNotRequestModelInfo) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));

  auto                    options = BaseOptions();
  options.extra_arguments         = {"--sleep-ms", "30000"};
  options.require_model_info      = false;

  ASSERT_TRUE(service.StartAndWait(options));
  EXPECT_GT(client->PingCount(), 0);
  EXPECT_EQ(client->GetModelInfoCalls(), 0);
  EXPECT_FALSE(service.Status().model_info.has_value());

  options.require_model_info = true;
  ASSERT_TRUE(service.StartAndWait(options));
  EXPECT_EQ(client->GetModelInfoCalls(), 1);
  const auto status = service.Status();
  ASSERT_TRUE(status.model_info.has_value());
  EXPECT_EQ(status.model_info->profile_id, "mobileclip2-s2-en");
  service.Stop();
}

TEST(AiSidecarRuntimeServiceTest, ReadyRuntimeSelfExitBecomesUiVisibleFailure) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  auto                    options = BaseOptions();
  options.extra_arguments         = {"--exit-after-ms", "100", "--exit-code", "9"};

  ASSERT_TRUE(service.StartAndWait(options));
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kFailed);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kRuntimeCrashed);
  EXPECT_NE(status.stderr_tail.find("self-exit"), std::string::npos);
  EXPECT_EQ(service.ClientSession(), nullptr);
}

TEST(AiSidecarRuntimeServiceTest, StopKillsHungRuntime) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  auto                    options = BaseOptions();
  options.extra_arguments         = {"--ignore-terminate", "--sleep-ms", "30000"};
  options.graceful_stop_timeout   = std::chrono::milliseconds(1);
  options.kill_timeout            = std::chrono::milliseconds(1000);

  ASSERT_TRUE(service.StartAndWait(options));
  service.Stop();
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kStopped);
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kNone);
  EXPECT_EQ(service.ClientSession(), nullptr);
}

TEST(AiSidecarRuntimeServiceTest, RuntimeArgumentsCarryModelAndDeviceConfiguration) {
  const test::TestTempDirectory temp_dir;
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  const auto record_path = temp_dir.Path() / "semantic_runtime_args.txt";
  std::filesystem::remove(record_path);

  auto options            = BaseOptions();
  options.model_root      = std::filesystem::path("C:/models/mobileclip");
  options.model_id        = "repo/model:s2";
  options.revision        = "abc123";
  options.device             = "directml:0";
  options.vision_device      = "directml:1";
  options.vision_idle_unload = std::chrono::seconds(45);
  options.extra_arguments    = {"--record-args", record_path.string(), "--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  service.Stop();

  std::ifstream in(record_path);
  ASSERT_TRUE(in.is_open());
  const std::string args((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  EXPECT_NE(args.find("--model-root\nC:/models/mobileclip"), std::string::npos);
  EXPECT_NE(args.find("--model-id\nrepo/model:s2"), std::string::npos);
  EXPECT_NE(args.find("--revision\nabc123"), std::string::npos);
  EXPECT_NE(args.find("--device\ndirectml:0"), std::string::npos);
  EXPECT_NE(args.find("--text-device\ncpu\n"), std::string::npos);
  EXPECT_NE(args.find("--vision-device\ndirectml:1\n"), std::string::npos);
  // No text idle time in the options and idle stop delay 0: the text encoder unloads after
  // each request.
  EXPECT_NE(args.find("--text-idle-unload-seconds\n0\n"), std::string::npos);
  EXPECT_NE(args.find("--vision-idle-unload-seconds\n45\n"), std::string::npos);
  EXPECT_NE(args.find("--no-download"), std::string::npos);
  // Advanced Content Analysis is removed (issue #710): no provider config directory.
  EXPECT_EQ(args.find("--provider-config-dir"), std::string::npos);
  std::filesystem::remove(record_path);
}

// A project without an active CLIP model starts the sidecar without a model (AI Models plan,
// P10): no --model-id and no --revision on the command line.
TEST(AiSidecarRuntimeServiceTest, RuntimeArgumentsOmitModelIdWithoutActiveModel) {
  const test::TestTempDirectory temp_dir;
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  const auto record_path = temp_dir.Path() / "semantic_runtime_no_model_args.txt";
  std::filesystem::remove(record_path);

  auto options               = BaseOptions();
  options.model_id           = {};
  options.revision           = {};
  options.require_model_info = false;
  options.extra_arguments    = {"--record-args", record_path.string(), "--sleep-ms", "30000"};
  EXPECT_TRUE(AiSidecarRuntimeOptions{}.model_id.empty());

  ASSERT_TRUE(service.StartAndWait(options));
  service.Stop();

  std::ifstream in(record_path);
  ASSERT_TRUE(in.is_open());
  const std::string args((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  EXPECT_EQ(args.find("--model-id"), std::string::npos) << args;
  EXPECT_EQ(args.find("--revision"), std::string::npos) << args;
  EXPECT_NE(args.find("--model-root\n"), std::string::npos) << args;
  std::filesystem::remove(record_path);
}

// The sidecar log of the session is next to the application log (#823): the host passes
// `--log-file <log dir>/<application log stem>_mind.log`.
TEST(AiSidecarRuntimeServiceTest, ArgumentsNameTheSidecarLogFileNextToTheApplicationLog) {
  const test::TestTempDirectory temp_dir;
  const auto                    record_path     = temp_dir.Path() / "semantic_runtime_log_args.txt";
  const QString                 application_log = diag::InitializeApplicationLogging(
      QString::fromStdWString((temp_dir.Path() / "logs").wstring()));
  ASSERT_FALSE(application_log.isEmpty());
  ASSERT_TRUE(application_log.endsWith(QStringLiteral(".log")));
  const QString expected_sidecar_log = application_log.chopped(4) + QStringLiteral("_mind.log");

  auto          client               = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  auto                    options = BaseOptions();
  options.extra_arguments         = {"--record-args", record_path.string(), "--sleep-ms", "30000"};
  ASSERT_TRUE(service.StartAndWait(options));
  service.Stop();
  diag::ShutdownApplicationLogging();

  std::ifstream in(record_path);
  ASSERT_TRUE(in.is_open());
  const std::string args((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  EXPECT_NE(args.find("--log-file\n" + expected_sidecar_log.toStdString() + "\n"),
            std::string::npos)
      << args;
}

// Without an open application log the sidecar writes to stderr only: no `--log-file`.
TEST(AiSidecarRuntimeServiceTest, ArgumentsHaveNoLogFileWithoutApplicationLog) {
  const test::TestTempDirectory temp_dir;
  const auto                    record_path = temp_dir.Path() / "semantic_runtime_no_log_args.txt";
  ASSERT_TRUE(diag::CompanionLogFilePath(QStringLiteral("mind")).isEmpty());

  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  auto                    options = BaseOptions();
  options.extra_arguments         = {"--record-args", record_path.string(), "--sleep-ms", "30000"};
  ASSERT_TRUE(service.StartAndWait(options));
  service.Stop();

  std::ifstream in(record_path);
  ASSERT_TRUE(in.is_open());
  const std::string args((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  EXPECT_EQ(args.find("--log-file"), std::string::npos) << args;
}

// A stdout chunk with an INFO, a WARN, and an ERROR line of the sidecar `tracing` output writes
// two application log messages: the WARN line as a warning and the ERROR line as a critical
// message (#823). The stdout tail keeps every line.
TEST(AiSidecarRuntimeServiceTest, StdoutChunkCopiesOnlyWarnAndErrorLinesToTheApplicationLog) {
  const ScopedSidecarStdoutCapture capture;
  auto                             client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService          service(FakeFactory(client));
  auto                             options = BaseOptions();
  options.extra_arguments                  = {"--print-tracing-lines", "--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  ASSERT_TRUE(WaitForStdoutTail(service, "marker-error"));
  const auto messages = capture.Messages();
  service.Stop();

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].type, QtWarningMsg);
  EXPECT_NE(messages[0].text.find("WARN fake warn line marker-warn"), std::string::npos)
      << messages[0].text;
  EXPECT_EQ(messages[1].type, QtCriticalMsg);
  EXPECT_NE(messages[1].text.find("ERROR fake error line marker-error"), std::string::npos)
      << messages[1].text;
  const auto status = service.Status();
  EXPECT_NE(status.stdout_tail.find("marker-info"), std::string::npos);
  EXPECT_NE(status.stdout_tail.find("marker-warn"), std::string::npos);
}

// A WARN line that arrives in two chunks is one message, and the continuation line of the message
// without a level has the WARN level of the line before it.
TEST(AiSidecarRuntimeServiceTest, StdoutLineSplitAcrossChunksIsOneApplicationLogMessage) {
  const ScopedSidecarStdoutCapture capture;
  auto                             client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService          service(FakeFactory(client));
  auto                             options = BaseOptions();
  options.extra_arguments                  = {"--print-split-warn-line", "--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  ASSERT_TRUE(WaitForStdoutTail(service, "marker-end"));
  const auto messages = capture.Messages();
  service.Stop();

  ASSERT_EQ(messages.size(), 2u);
  EXPECT_EQ(messages[0].type, QtWarningMsg);
  EXPECT_NE(messages[0].text.find("fake split line first-half second-half"), std::string::npos)
      << messages[0].text;
  EXPECT_EQ(messages[1].type, QtWarningMsg);
  EXPECT_NE(messages[1].text.find("marker-continuation"), std::string::npos) << messages[1].text;
}

TEST(AiSidecarRuntimeServiceTest, TextIdleUnloadFollowsIdleStopDelay) {
  const test::TestTempDirectory temp_dir;
  const auto record_path = temp_dir.Path() / "semantic_runtime_idle_args.txt";
  const auto read_args = [&record_path] {
    std::ifstream in(record_path);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  };
  auto options            = BaseOptions();
  options.extra_arguments = {"--record-args", record_path.string(), "--sleep-ms", "30000"};

  for (const auto& [delay, expected] : {std::pair{std::chrono::milliseconds(300000), "300"},
                                        std::pair{std::chrono::milliseconds(-1), "-1"}}) {
    std::filesystem::remove(record_path);
    auto                    client = std::make_shared<FakeSidecarClient>();
    AiSidecarRuntimeService service(FakeFactory(client));
    service.SetIdleStopDelay(delay);
    ASSERT_TRUE(service.StartAndWait(options));
    service.StopForProjectClose();
    EXPECT_NE(read_args().find(std::string("--text-idle-unload-seconds\n") + expected + "\n"),
              std::string::npos)
        << expected;
  }

  // A value in the options takes precedence over the delay.
  std::filesystem::remove(record_path);
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  service.SetIdleStopDelay(std::chrono::seconds(300));
  options.text_idle_unload = std::chrono::seconds(15);
  ASSERT_TRUE(service.StartAndWait(options));
  service.StopForProjectClose();
  EXPECT_NE(read_args().find("--text-idle-unload-seconds\n15\n"), std::string::npos);
  std::filesystem::remove(record_path);
}

TEST(AiSidecarRuntimeServiceTest, ClientSessionExposesNarrowModulesWhenReady) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));

  auto                    options = BaseOptions();
  options.extra_arguments         = {"--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  auto session = service.ClientSession();
  ASSERT_NE(session, nullptr);

  std::string error;
  const auto  profiles =
      session->models().ListModelProfiles("C:/models", std::chrono::milliseconds(100), &error);
  ASSERT_EQ(profiles.size(), 2u) << error;
  EXPECT_EQ(profiles[1].profile_id, "jina-clip-v2-int8-multilingual");
  EXPECT_EQ(profiles[1].native_embedding_dimension, 1024u);

  const auto text =
      session->semantic().EmbedText("text-1", "a quiet portrait", std::chrono::milliseconds(100));
  EXPECT_TRUE(text.ok);
  EXPECT_EQ(text.request_id, "text-1");
  EXPECT_EQ(client->EmbedTextCalls(), 1);

  service.Stop();
}

TEST(AiSidecarRuntimeServiceTest, ClientSessionPreservesEmbeddingBatchRequestIds) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));

  auto                    options = BaseOptions();
  options.extra_arguments         = {"--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  auto session = service.ClientSession();
  ASSERT_NE(session, nullptr);

  std::vector<SemanticImageEmbeddingRequest> requests;
  for (const char* id : {"echo-1", "echo-2", "echo-3"}) {
    SemanticImageEmbeddingRequest req;
    req.request_id  = id;
    req.rgba8_image = std::vector<uint8_t>(4, 0);
    req.format_hint = "rgba8:1x1";
    requests.push_back(std::move(req));
  }

  const auto results =
      session->semantic().EmbedImageBatch(requests, std::chrono::milliseconds(100));
  ASSERT_EQ(results.size(), 3u);
  EXPECT_EQ(results[0].request_id, "echo-1");
  EXPECT_EQ(results[1].request_id, "echo-2");
  EXPECT_EQ(results[2].request_id, "echo-3");
  for (const auto& r : results) {
    EXPECT_TRUE(r.ok);
  }
  EXPECT_EQ(client->EmbedImageBatchCalls(), 1);
  service.Stop();
}

TEST(AiSidecarRuntimeServiceTest, EmptyModelRootUsesEnvironmentFallback) {
  const test::TestTempDirectory temp_dir;
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  const auto record_path = temp_dir.Path() / "semantic_runtime_env_model_args.txt";
  const auto model_root = temp_dir.Path() / "semantic_runtime_env_model_root";
  std::filesystem::remove(record_path);
  std::filesystem::create_directories(model_root);

  ScopedModelRootEnv model_root_env(model_root.string());

  auto               options = BaseOptions();
  options.model_root.clear();
  options.extra_arguments = {"--record-args", record_path.string(), "--sleep-ms", "30000"};

  ASSERT_TRUE(service.StartAndWait(options));
  service.Stop();

  std::ifstream in(record_path);
  ASSERT_TRUE(in.is_open());
  const std::string args((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  in.close();
  EXPECT_NE(args.find("--model-root\n" + model_root.string()), std::string::npos);
  std::filesystem::remove(record_path);
  std::filesystem::remove_all(model_root);
}

TEST(AiSidecarRuntimeServiceTest, ProjectServiceOwnsStoppedRuntimeService) {
  const test::TestTempDirectory temp_dir;
  const auto db_path   = temp_dir.Path() / "semantic_runtime_project.db";
  const auto meta_path = temp_dir.Path() / "semantic_runtime_project.json";
  std::filesystem::remove(db_path);
  std::filesystem::remove(meta_path);

  {
    ProjectService project(db_path, meta_path, ProjectOpenMode::kCreateNew);
    auto           runtime = project.GetAiSidecarRuntimeService();
    ASSERT_NE(runtime, nullptr);
    const auto status = runtime->Status();
    EXPECT_EQ(status.state, AiSidecarRuntimeState::kStopped);
  }

  std::filesystem::remove(db_path);
  std::filesystem::remove(meta_path);
}

TEST(AiSidecarRuntimeServiceTest, ProjectServiceCreatesRuntimeOnCallerThreadAfterBackgroundLoad) {
  const test::TestTempDirectory temp_dir;
  const auto db_path   = temp_dir.Path() / "semantic_runtime_thread.db";
  const auto meta_path = temp_dir.Path() / "semantic_runtime_thread.json";
  std::filesystem::remove(db_path);
  std::filesystem::remove(meta_path);

  std::shared_ptr<ProjectService> project;
  std::thread                     loader([&project, &db_path, &meta_path]() {
    project = std::make_shared<ProjectService>(db_path, meta_path, ProjectOpenMode::kCreateNew);
  });
  loader.join();

  ASSERT_NE(project, nullptr);
  auto runtime = project->GetAiSidecarRuntimeService();
  ASSERT_NE(runtime, nullptr);
  ASSERT_EQ(runtime->thread(), QThread::currentThread());
  const auto status = runtime->Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kStopped);

  project.reset();
  std::filesystem::remove(db_path);
  std::filesystem::remove(meta_path);
}

namespace {

using SteadyClock = std::chrono::steady_clock;

/// Runs the Qt event loop of the calling (owner) thread for @p duration.
void ProcessEventsFor(std::chrono::milliseconds duration) {
  const auto deadline = SteadyClock::now() + duration;
  while (SteadyClock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

/// Runs the Qt event loop until @p done returns true or @p timeout passes. Returns done().
template <typename Done>
auto ProcessEventsUntil(Done done, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = SteadyClock::now() + timeout;
  while (!done()) {
    if (SteadyClock::now() >= deadline) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  return true;
}

auto StateOf(AiSidecarRuntimeService& service) -> AiSidecarRuntimeState {
  return service.Status().state;
}

/// Base options with a fake runtime that runs until it is stopped.
auto LongRunningOptions() -> AiSidecarRuntimeOptions {
  auto options            = BaseOptions();
  options.extra_arguments = {"--sleep-ms", "30000"};
  return options;
}

/// A fake runtime that ignores the terminate request, so only the kill after
/// @p graceful_stop_timeout ends it.
auto TerminateIgnoringOptions(std::chrono::milliseconds graceful_stop_timeout)
    -> AiSidecarRuntimeOptions {
  auto options                  = BaseOptions();
  options.extra_arguments       = {"--ignore-terminate", "--sleep-ms", "30000"};
  options.graceful_stop_timeout = graceful_stop_timeout;
  options.kill_timeout          = std::chrono::milliseconds(2000);
  return options;
}

}  // namespace

TEST(AiSidecarRuntimeServiceTest, IdleDelayKeepsRuntimeRunningAfterLastRelease) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  service.SetIdleStopDelay(std::chrono::seconds(2));

  auto lease = service.AcquireLease();
  ASSERT_TRUE(service.StartAndWait(LongRunningOptions()));
  const auto pid = service.Status().process_id;
  ASSERT_GT(pid, 0);

  lease.reset();
  ProcessEventsFor(std::chrono::milliseconds(200));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kReady);
  EXPECT_EQ(status.process_id, pid);
  service.StopForProjectClose();
}

TEST(AiSidecarRuntimeServiceTest, IdleDelayStopsRuntimeWhenNoLeaseReturns) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  service.SetIdleStopDelay(std::chrono::milliseconds(300));

  auto lease = service.AcquireLease();
  ASSERT_TRUE(service.StartAndWait(LongRunningOptions()));
  lease.reset();
  EXPECT_EQ(StateOf(service), AiSidecarRuntimeState::kReady);

  EXPECT_TRUE(
      ProcessEventsUntil([&] { return StateOf(service) == AiSidecarRuntimeState::kStopped; },
                         std::chrono::seconds(3)));
  const auto status = service.Status();
  EXPECT_EQ(status.issue, AiSidecarRuntimeIssue::kNone);
  EXPECT_EQ(status.process_id, 0);
  EXPECT_EQ(service.ClientSession(), nullptr);
}

TEST(AiSidecarRuntimeServiceTest, NewLeaseCancelsIdleStop) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  service.SetIdleStopDelay(std::chrono::milliseconds(300));

  auto lease = service.AcquireLease();
  ASSERT_TRUE(service.StartAndWait(LongRunningOptions()));
  const auto pid = service.Status().process_id;
  lease.reset();
  ProcessEventsFor(std::chrono::milliseconds(100));
  lease = service.AcquireLease();

  ProcessEventsFor(std::chrono::milliseconds(1000));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kReady);
  EXPECT_EQ(status.process_id, pid);
  lease.reset();
  service.StopForProjectClose();
}

TEST(AiSidecarRuntimeServiceTest, NegativeDelayKeepsRuntimeUntilProjectClose) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  service.SetIdleStopDelay(std::chrono::milliseconds(-1));

  auto lease = service.AcquireLease();
  ASSERT_TRUE(service.StartAndWait(LongRunningOptions()));
  lease.reset();
  ProcessEventsFor(std::chrono::milliseconds(1000));
  EXPECT_EQ(StateOf(service), AiSidecarRuntimeState::kReady);

  service.StopForProjectClose();
  EXPECT_EQ(StateOf(service), AiSidecarRuntimeState::kStopped);
  EXPECT_EQ(service.Status().process_id, 0);
}

TEST(AiSidecarRuntimeServiceTest, IdleStopDoesNotBlockOwnerThread) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  service.SetIdleStopDelay(std::chrono::milliseconds(100));

  // A blocking stop of this runtime would hold the owner thread for the 1 s graceful stop time.
  auto lease = service.AcquireLease();
  ASSERT_TRUE(service.StartAndWait(TerminateIgnoringOptions(std::chrono::milliseconds(1000))));
  lease.reset();
  ASSERT_TRUE(
      ProcessEventsUntil([&] { return StateOf(service) == AiSidecarRuntimeState::kStopping; },
                         std::chrono::seconds(2)));

  const auto                             posted_at = SteadyClock::now();
  std::optional<SteadyClock::time_point> ran_at;
  QMetaObject::invokeMethod(
      &service, [&ran_at] { ran_at = SteadyClock::now(); }, Qt::QueuedConnection);
  ASSERT_TRUE(ProcessEventsUntil([&] { return ran_at.has_value(); }, std::chrono::seconds(2)));
  EXPECT_LT(*ran_at - posted_at, std::chrono::milliseconds(50));
  EXPECT_EQ(StateOf(service), AiSidecarRuntimeState::kStopping);

  // The kill after the graceful stop time completes the stop.
  EXPECT_TRUE(
      ProcessEventsUntil([&] { return StateOf(service) == AiSidecarRuntimeState::kStopped; },
                         std::chrono::seconds(4)));
  EXPECT_EQ(service.Status().issue, AiSidecarRuntimeIssue::kNone);
}

TEST(AiSidecarRuntimeServiceTest, StartDuringIdleStopWaitsAndStartsNewProcess) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  service.SetIdleStopDelay(std::chrono::milliseconds(100));

  const auto options = TerminateIgnoringOptions(std::chrono::milliseconds(1000));
  auto       lease   = service.AcquireLease();
  ASSERT_TRUE(service.StartAndWait(options));
  const auto first_pid = service.Status().process_id;
  lease.reset();
  ASSERT_TRUE(
      ProcessEventsUntil([&] { return StateOf(service) == AiSidecarRuntimeState::kStopping; },
                         std::chrono::seconds(2)));

  lease = service.AcquireLease();
  ASSERT_TRUE(service.StartAndWait(options));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kReady);
  EXPECT_GT(status.process_id, 0);
  EXPECT_NE(status.process_id, first_pid);

  // The finished signal of the old process does not stop the new one.
  ProcessEventsFor(std::chrono::milliseconds(300));
  EXPECT_EQ(StateOf(service), AiSidecarRuntimeState::kReady);
  EXPECT_EQ(service.Status().process_id, status.process_id);
  lease.reset();
  service.StopForProjectClose();
}

TEST(AiSidecarRuntimeServiceTest, ModelIdentityChangeRestartsProcessWithoutOtherLease) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));

  auto                    lease   = service.AcquireLease();
  const auto              options = LongRunningOptions();
  ASSERT_TRUE(service.StartAndWait(options));
  const auto first_pid   = service.Status().process_id;

  auto       other_model = options;
  other_model.model_id   = "another/model";
  ASSERT_TRUE(service.StartAndWait(other_model));
  const auto status = service.Status();
  EXPECT_EQ(status.state, AiSidecarRuntimeState::kReady);
  EXPECT_NE(status.process_id, first_pid);
  EXPECT_EQ(service.Options().model_id, "another/model");

  // Same identity: the process is reused.
  ASSERT_TRUE(service.StartAndWait(other_model));
  EXPECT_EQ(service.Status().process_id, status.process_id);
  lease.reset();
  EXPECT_EQ(StateOf(service), AiSidecarRuntimeState::kStopped);
}

TEST(AiSidecarRuntimeServiceTest, WorkerStartKeepsOwnerThreadResponsive) {
  auto                    client = std::make_shared<FakeSidecarClient>(/*ready=*/false);
  AiSidecarRuntimeService service(FakeFactory(client));
  auto                    lease = service.AcquireLease();

  std::atomic<bool>       worker_done{false};
  std::atomic<bool>       worker_result{false};
  std::thread             worker([&] {
    auto options            = LongRunningOptions();
    options.startup_timeout = std::chrono::milliseconds(5000);
    worker_result           = service.StartAndWaitInteractive(options);
    worker_done             = true;
  });

  // A second thread posts a call to the owner thread during the boot and measures how long it
  // waits; the owner thread runs the boot because the worker calls into it.
  std::atomic<int64_t>    post_wait_us{-1};
  std::thread             poster([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto posted_at = SteadyClock::now();
    QMetaObject::invokeMethod(
        &service,
        [&post_wait_us, posted_at] {
          post_wait_us =
              std::chrono::duration_cast<std::chrono::microseconds>(SteadyClock::now() - posted_at)
                  .count();
        },
        Qt::QueuedConnection);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    client->SetReady(true);
  });

  EXPECT_TRUE(ProcessEventsUntil([&] { return worker_done.load(); }, std::chrono::seconds(10)));
  poster.join();
  worker.join();
  EXPECT_TRUE(worker_result.load());
  ASSERT_GE(post_wait_us.load(), 0);
  EXPECT_LT(post_wait_us.load(), 50000);
  lease.reset();
}

// Issue #432: the readiness probe blocks in gRPC calls while the process boots. The
// interactive start runs the probe off the owner thread, so a slow Ping does not delay the
// events (paints, animation timers) of the owner thread.
TEST(AiSidecarRuntimeServiceTest, InteractiveStartRunsSlowProbeOffOwnerThread) {
  auto                    client = std::make_shared<FakeSidecarClient>(/*ready=*/false);
  client->SetPingDelay(std::chrono::milliseconds(300));
  AiSidecarRuntimeService service(FakeFactory(client));
  auto                    lease = service.AcquireLease();

  std::atomic<int64_t>    max_post_wait_us{-1};
  std::atomic<bool>       poster_done{false};
  std::thread             poster([&] {
    // Posts several calls during the boot and records the longest wait for the owner thread.
    for (int i = 0; i < 6; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(110));
      const auto posted_at = SteadyClock::now();
      QMetaObject::invokeMethod(
          &service,
          [&max_post_wait_us, posted_at] {
            const auto waited_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                       SteadyClock::now() - posted_at)
                                       .count();
            if (waited_us > max_post_wait_us.load()) {
              max_post_wait_us = waited_us;
            }
          },
          Qt::QueuedConnection);
    }
    client->SetPingDelay(std::chrono::milliseconds(0));
    client->SetReady(true);
    poster_done = true;
  });

  auto options            = LongRunningOptions();
  options.startup_timeout = std::chrono::milliseconds(5000);
  EXPECT_TRUE(service.StartAndWaitInteractive(options));
  EXPECT_TRUE(ProcessEventsUntil([&] { return poster_done.load(); }, std::chrono::seconds(10)));
  poster.join();
  QCoreApplication::processEvents();
  ASSERT_GE(max_post_wait_us.load(), 0);
  EXPECT_LT(max_post_wait_us.load(), 100000);
  lease.reset();
}

TEST(AiSidecarRuntimeServiceTest, ZeroDelayWhileIdleStopsRunningProcess) {
  auto                    client = std::make_shared<FakeSidecarClient>();
  AiSidecarRuntimeService service(FakeFactory(client));
  service.SetIdleStopDelay(std::chrono::seconds(300));

  auto lease = service.AcquireLease();
  ASSERT_TRUE(service.StartAndWait(LongRunningOptions()));
  lease.reset();
  EXPECT_EQ(StateOf(service), AiSidecarRuntimeState::kReady);

  service.SetIdleStopDelay(std::chrono::seconds(0));
  EXPECT_TRUE(
      ProcessEventsUntil([&] { return StateOf(service) == AiSidecarRuntimeState::kStopped; },
                         std::chrono::seconds(3)));
}

// The project service forwards the setting to the runtime service at once. That a delay of 0
// stops an idle running process is checked by ZeroDelayWhileIdleStopsRunningProcess.
TEST(AiSidecarRuntimeServiceTest, IdleStopSettingAppliesToRunningService) {
  const test::TestTempDirectory temp_dir;
  const auto db_path   = temp_dir.Path() / "semantic_runtime_idle.db";
  const auto meta_path = temp_dir.Path() / "semantic_runtime_idle.json";
  std::filesystem::remove(db_path);
  std::filesystem::remove(meta_path);
  {
    ProjectService project(db_path, meta_path, ProjectOpenMode::kCreateNew);
    auto           runtime = project.GetAiSidecarRuntimeService();
    project.SetAiSidecarIdleStopSeconds(300);
    EXPECT_EQ(runtime->IdleStopDelay(), std::chrono::seconds(300));
    project.SetAiSidecarIdleStopSeconds(0);
    EXPECT_EQ(runtime->IdleStopDelay(), std::chrono::seconds(0));
    project.SetAiSidecarIdleStopSeconds(-1);
    EXPECT_LT(runtime->IdleStopDelay().count(), 0);
  }
  std::filesystem::remove(db_path);
  std::filesystem::remove(meta_path);
}

TEST(AiSidecarRuntimeServiceTest, UnknownIdleStopSettingReadsAsDefault) {
  const test::TestTempDirectory temp_dir;
  EXPECT_EQ(ProjectService::NormalizedAiSidecarIdleStopSeconds(42), 30);
  EXPECT_EQ(ProjectService::NormalizedAiSidecarIdleStopSeconds(-7), 30);
  for (const int value : {0, 30, 60, 300, 900, -1}) {
    EXPECT_EQ(ProjectService::NormalizedAiSidecarIdleStopSeconds(value), value);
  }

  const auto db_path   = temp_dir.Path() / "semantic_runtime_unknown.db";
  const auto meta_path = temp_dir.Path() / "semantic_runtime_unknown.json";
  std::filesystem::remove(db_path);
  std::filesystem::remove(meta_path);
  {
    ProjectService project(db_path, meta_path, ProjectOpenMode::kCreateNew);
    project.SetAiSidecarIdleStopSeconds(42);
    EXPECT_EQ(project.GetAiSidecarRuntimeService()->IdleStopDelay(), std::chrono::seconds(30));
  }
  std::filesystem::remove(db_path);
  std::filesystem::remove(meta_path);
}

// The real Rust sidecar starts without --model-id (no active model in the open project), and
// a semantic request reports that no model is active (AI_ERROR_MODEL_NOT_READY in the response
// header) instead of an embedding.
TEST(AiSidecarRuntimeServiceLiveTest, RustRuntimeStartsWithoutModelIdAndRejectsSemanticRequests) {
  const test::TestTempDirectory temp_dir;
  const char* runtime_path_env = std::getenv("ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH");
  if (runtime_path_env == nullptr) {
    GTEST_SKIP() << "Set ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH to run against the Rust runtime.";
  }

  AiSidecarRuntimeService service;
  AiSidecarRuntimeOptions options;
  options.runtime_binary     = std::filesystem::path(runtime_path_env);
  options.model_root         = temp_dir.Path() / "alcedo_no_model_root";
  options.device             = "cpu";
  options.require_model_info = false;
  options.startup_timeout    = std::chrono::milliseconds(60000);
  ASSERT_TRUE(options.model_id.empty());

  ASSERT_TRUE(service.StartAndWait(options)) << service.Status().message;
  EXPECT_EQ(service.Status().state, AiSidecarRuntimeState::kReady);
  auto session = service.ClientSession();
  ASSERT_NE(session, nullptr);

  std::vector<uint8_t>          rgba8(4u * 4u * 4u, 128);
  SemanticImageEmbeddingRequest request;
  request.request_id  = "no-model-image-1";
  request.rgba8_image = std::move(rgba8);
  request.format_hint = "rgba8:4x4";
  const auto results =
      session->semantic().EmbedImageBatch({request}, std::chrono::milliseconds(30000));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_FALSE(results[0].ok);
  EXPECT_NE(results[0].error.find("no semantic model is active"), std::string::npos)
      << results[0].error;

  AiSidecarRuntimeModelInfo info;
  std::string               error;
  EXPECT_FALSE(session->semantic().GetModelInfo(std::chrono::milliseconds(30000), &info, &error));
  EXPECT_NE(error.find("no semantic model is active"), std::string::npos) << error;
  service.Stop();
}

TEST(AiSidecarRuntimeServiceLiveTest, DefaultGrpcClientEmbedsRawRgba8AgainstRustRuntime) {
  const char* runtime_path_env = std::getenv("ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH");
  const char* model_root_env   = std::getenv("ALCEDO_SEMANTIC_LIVE_MODEL_ROOT");
  if (runtime_path_env == nullptr || model_root_env == nullptr) {
    GTEST_SKIP() << "Set ALCEDO_SEMANTIC_LIVE_RUNTIME_PATH and "
                    "ALCEDO_SEMANTIC_LIVE_MODEL_ROOT to run the live Rust runtime check.";
  }

  const auto env_or = [](const char* name, const char* fallback) -> std::string {
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : std::string(fallback);
  };
  const auto env_u32_or = [](const char* name, uint32_t fallback) -> uint32_t {
    const char* value = std::getenv(name);
    if (value == nullptr || value[0] == '\0') {
      return fallback;
    }
    return static_cast<uint32_t>(std::stoul(value));
  };

  const auto model_id = env_or("ALCEDO_SEMANTIC_LIVE_MODEL_ID", "plhery/mobileclip2-onnx:s2");
  const auto revision =
      env_or("ALCEDO_SEMANTIC_LIVE_REVISION", "ba95759a5bdbaca53e9111e2550a76ec09c8fd9e");
  const auto expected_image_size = env_u32_or("ALCEDO_SEMANTIC_LIVE_EXPECTED_IMAGE_SIZE", 256u);

  AiSidecarRuntimeService service;
  AiSidecarRuntimeOptions options;
  options.runtime_binary        = std::filesystem::path(runtime_path_env);
  options.model_root            = std::filesystem::path(model_root_env);
  options.model_id              = model_id;
  options.revision              = revision;
  options.device                = "cpu";
  options.batch_cap             = 8;
  options.batch_wait_ms         = 2;
  options.startup_timeout       = std::chrono::milliseconds(120000);
  options.health_poll_interval  = std::chrono::milliseconds(100);
  options.graceful_stop_timeout = std::chrono::milliseconds(1000);
  options.kill_timeout          = std::chrono::milliseconds(2000);

  ASSERT_TRUE(service.StartAndWait(options)) << service.Status().message;
  const auto status = service.Status();
  ASSERT_TRUE(status.model_info.has_value());
  EXPECT_EQ(status.model_info->model_id, options.model_id);
  EXPECT_EQ(status.model_info->revision, options.revision);
  EXPECT_EQ(status.model_info->embedding_dimension, 512u);
  EXPECT_EQ(status.model_info->image_size, expected_image_size);
  auto session = service.ClientSession();
  ASSERT_NE(session, nullptr);

  std::vector<uint8_t> rgba8(256u * 256u * 4u);
  for (size_t i = 0; i < rgba8.size(); i += 4) {
    rgba8[i]     = 64;
    rgba8[i + 1] = 128;
    rgba8[i + 2] = 192;
    rgba8[i + 3] = 255;
  }

  SemanticImageEmbeddingRequest request;
  request.request_id  = "live-rgba8-image-1";
  request.rgba8_image = std::move(rgba8);
  request.format_hint = "rgba8:256x256";

  const auto results =
      session->semantic().EmbedImageBatch({request}, std::chrono::milliseconds(120000));
  ASSERT_EQ(results.size(), 1u);
  EXPECT_EQ(results[0].request_id, "live-rgba8-image-1");
  EXPECT_TRUE(results[0].ok) << results[0].error;
  EXPECT_EQ(results[0].dimension, 512u);
  EXPECT_EQ(results[0].embedding.size(), 512u);

  service.Stop();
}

}  // namespace alcedo

// The idle stop timer and the asynchronous stop need the Qt event loop of the owner thread.
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
