//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/ai_sidecar_runtime_service.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QHostAddress>
#include <QMetaObject>
#include <QPointer>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QTcpServer>
#include <QThread>
#include <chrono>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

#include "utils/diagnostics/app_logging.hpp"

#ifdef _WIN32
#include <QSettings>
#include <windows.h>
#endif

namespace alcedo {
namespace {

constexpr size_t kLogTailBytes              = 16 * 1024;
constexpr auto   kAiSidecarRuntimeBinaryEnv = "ALCEDO_MIND_BINARY";
constexpr auto   kSemanticModelRootEnv      = "ALCEDO_MIND_MODEL_ROOT";
constexpr auto   kDefaultModelDirectory     = "model";
#ifdef _WIN32
constexpr auto kAiSidecarRuntimeBinaryName = "alcedo_mind.exe";
#else
constexpr auto kAiSidecarRuntimeBinaryName = "alcedo_mind";
#endif

auto TailAppend(std::string* target, const QByteArray& bytes) -> void {
  target->append(bytes.constData(), static_cast<size_t>(bytes.size()));
  if (target->size() > kLogTailBytes) {
    target->erase(0, target->size() - kLogTailBytes);
  }
}

auto BuildEndpoint(const std::string& host, uint16_t port) -> std::string {
  return host + ":" + std::to_string(port);
}

auto FsPathFromQString(const QString& value) -> std::filesystem::path {
#ifdef _WIN32
  return std::filesystem::path(value.toStdWString());
#else
  return std::filesystem::path(value.toStdString());
#endif
}

auto DefaultProviderConfigDir() -> std::filesystem::path {
  QString root = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  if (root.isEmpty()) {
    root = QDir::homePath() + QStringLiteral("/.alcedo_studio");
  }
  return FsPathFromQString(root) / "ai_provider_configs";
}

auto DefaultRuntimeBinary() -> std::filesystem::path;
auto DefaultRuntimeModelRoot() -> std::filesystem::path;

/// @p path in a form where `C:/a/b` and `C:\a\b\` compare equal.
auto ComparablePath(const std::filesystem::path& path) -> std::filesystem::path {
  auto normal = path.lexically_normal();
  if (normal.has_filename() || normal.empty()) {
    return normal;
  }
  return normal.parent_path();
}

auto NormalizeRuntimeOptions(AiSidecarRuntimeOptions options) -> AiSidecarRuntimeOptions {
  if (options.runtime_binary.empty()) {
    options.runtime_binary = DefaultRuntimeBinary();
  }
  if (options.model_root.empty()) {
    options.model_root = DefaultRuntimeModelRoot();
  }
  if (options.provider_config_dir.empty()) {
    options.provider_config_dir = DefaultProviderConfigDir();
  }
  return options;
}

auto DefaultRuntimeBinary() -> std::filesystem::path {
  const QByteArray env_binary = qgetenv(kAiSidecarRuntimeBinaryEnv);
  if (!env_binary.isEmpty()) {
    return std::filesystem::path(env_binary.constData());
  }

  const auto app_dir = QCoreApplication::applicationDirPath();
#ifdef _WIN32
  const auto app_path = std::filesystem::path(app_dir.toStdWString());
#else
  const auto app_path = std::filesystem::path(app_dir.toStdString());
#endif

  const auto append_ancestor_runtime_binaries = [](const std::filesystem::path&        start,
                                                   std::vector<std::filesystem::path>* candidates) {
    if (start.empty()) {
      return;
    }

    std::error_code ec;
    auto            current = std::filesystem::absolute(start, ec);
    if (ec) {
      current = start;
    }
    while (!current.empty()) {
      candidates->push_back(current / "rust" / "puerh_mind" / "target" / "release" /
                            kAiSidecarRuntimeBinaryName);
      candidates->push_back(current / "rust" / "puerh_mind" / "target" / "debug" /
                            kAiSidecarRuntimeBinaryName);
      const auto parent = current.parent_path();
      if (parent == current) {
        break;
      }
      current = parent;
    }
  };

  std::vector<std::filesystem::path> candidates;
  candidates.push_back(app_path / kAiSidecarRuntimeBinaryName);
  append_ancestor_runtime_binaries(app_path, &candidates);
  std::error_code ec;
  append_ancestor_runtime_binaries(std::filesystem::current_path(ec), &candidates);

  for (const auto& candidate : candidates) {
    if (std::filesystem::exists(candidate, ec) && !ec &&
        std::filesystem::is_regular_file(candidate, ec) && !ec) {
      return candidate;
    }
  }
  return app_path / kAiSidecarRuntimeBinaryName;
}

auto DefaultRuntimeModelRoot() -> std::filesystem::path {
  const QByteArray env_root = qgetenv(kSemanticModelRootEnv);
  if (!env_root.isEmpty()) {
    return std::filesystem::path(env_root.constData());
  }

  const auto app_dir = QCoreApplication::applicationDirPath();
#ifdef _WIN32
  const auto app_path = std::filesystem::path(app_dir.toStdWString());
#else
  const auto app_path = std::filesystem::path(app_dir.toStdString());
#endif

  return app_path / kDefaultModelDirectory;
}

#ifdef _WIN32
auto ProxyValueForScheme(const QString& proxy_server, const QString& scheme) -> QString {
  const QString trimmed = proxy_server.trimmed();
  if (trimmed.isEmpty()) {
    return {};
  }
  const QString prefix = scheme + QStringLiteral("=");
  for (const QString& part : trimmed.split(';', Qt::SkipEmptyParts)) {
    const QString entry = part.trimmed();
    if (entry.startsWith(prefix, Qt::CaseInsensitive)) {
      return entry.mid(prefix.size()).trimmed();
    }
  }
  if (!trimmed.contains('=')) {
    return trimmed;
  }
  return {};
}

auto NormalizeProxyUrl(QString value) -> QString {
  value = value.trimmed();
  if (value.isEmpty()) {
    return {};
  }
  if (!value.contains(QStringLiteral("://"))) {
    value.prepend(QStringLiteral("http://"));
  }
  return value;
}

auto AddWindowsUserProxyEnvironment(QProcessEnvironment env) -> QProcessEnvironment {
  if (env.contains(QStringLiteral("HTTPS_PROXY")) || env.contains(QStringLiteral("HTTP_PROXY")) ||
      env.contains(QStringLiteral("https_proxy")) || env.contains(QStringLiteral("http_proxy"))) {
    return env;
  }

  QSettings internet(
      QStringLiteral("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Internet "
                     "Settings"),
      QSettings::NativeFormat);
  if (internet.value(QStringLiteral("ProxyEnable")).toInt() == 0) {
    return env;
  }

  const QString proxy_server = internet.value(QStringLiteral("ProxyServer")).toString();
  const QString http_proxy   = NormalizeProxyUrl(ProxyValueForScheme(proxy_server, "http"));
  QString       https_proxy  = NormalizeProxyUrl(ProxyValueForScheme(proxy_server, "https"));
  if (https_proxy.isEmpty()) {
    https_proxy = http_proxy;
  }

  if (!http_proxy.isEmpty()) {
    env.insert(QStringLiteral("HTTP_PROXY"), http_proxy);
  }
  if (!https_proxy.isEmpty()) {
    env.insert(QStringLiteral("HTTPS_PROXY"), https_proxy);
  }
  if (!env.contains(QStringLiteral("NO_PROXY")) && !env.contains(QStringLiteral("no_proxy"))) {
    env.insert(QStringLiteral("NO_PROXY"), QStringLiteral("localhost,127.0.0.1,::1"));
  }
  return env;
}
#endif

/// Result of one readiness poll: Ping, then GetModelInfo when Ping succeeds and the start
/// requires the semantic model information.
struct ReadinessProbe {
  bool                      ping_ok_       = false;
  bool                      model_info_ok_ = false;
  std::string               ping_error_;
  AiSidecarRuntimeModelInfo model_info_;
  std::string               model_info_error_;
};

/// GetModelInfo makes the sidecar prepare the CLIP model profile (about 2 s), so a start that
/// does not need the semantic model (AI Mask, image analysis) does not ask for it.
auto RunReadinessProbe(const std::shared_ptr<sidecar_client::Client>& client,
                       std::chrono::milliseconds ping_timeout, bool require_model_info)
    -> ReadinessProbe {
  ReadinessProbe probe;
  probe.ping_ok_ = client->runtime().Ping(ping_timeout, &probe.ping_error_);
  if (probe.ping_ok_ && require_model_info) {
    probe.model_info_ok_ = client->semantic().GetModelInfo(
        std::chrono::milliseconds(500), &probe.model_info_, &probe.model_info_error_);
  }
  return probe;
}

/// Runs the event loop of the current thread for @p duration.
void PumpEventsFor(std::chrono::milliseconds duration) {
  QEventLoop loop;
  QTimer::singleShot(duration, &loop, &QEventLoop::quit);
  loop.exec();
}

/// Runs the readiness probe on a helper thread while the event loop of the current thread runs,
/// so the blocking gRPC calls (up to ping_timeout + 500 ms while the process boots or loads the
/// model) do not stop the UI. The helper holds its own reference to @p client.
auto RunProbeWhilePumpingEvents(std::shared_ptr<sidecar_client::Client> client,
                                std::chrono::milliseconds ping_timeout, bool require_model_info)
    -> ReadinessProbe {
  ReadinessProbe probe;
  QEventLoop     loop;
  std::thread    helper(
      [&probe, &loop, client = std::move(client), ping_timeout, require_model_info]() {
        probe = RunReadinessProbe(client, ping_timeout, require_model_info);
        // Queued: the quit runs inside loop.exec(), even when the probe ends before exec starts.
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
      });
  loop.exec();
  helper.join();
  return probe;
}

}  // namespace

auto ToString(AiSidecarRuntimeState state) -> const char* {
  switch (state) {
    case AiSidecarRuntimeState::kStopped:
      return "stopped";
    case AiSidecarRuntimeState::kStarting:
      return "starting";
    case AiSidecarRuntimeState::kReady:
      return "ready";
    case AiSidecarRuntimeState::kStopping:
      return "stopping";
    case AiSidecarRuntimeState::kFailed:
      return "failed";
  }
  return "unknown";
}

auto ToString(AiSidecarRuntimeIssue issue) -> const char* {
  switch (issue) {
    case AiSidecarRuntimeIssue::kNone:
      return "none";
    case AiSidecarRuntimeIssue::kBinaryMissing:
      return "binary_missing";
    case AiSidecarRuntimeIssue::kStartFailed:
      return "start_failed";
    case AiSidecarRuntimeIssue::kReadinessTimeout:
      return "readiness_timeout";
    case AiSidecarRuntimeIssue::kRuntimeExited:
      return "runtime_exited";
    case AiSidecarRuntimeIssue::kRuntimeCrashed:
      return "runtime_crashed";
    case AiSidecarRuntimeIssue::kStopTimedOut:
      return "stop_timed_out";
    case AiSidecarRuntimeIssue::kClientUnavailable:
      return "client_unavailable";
    case AiSidecarRuntimeIssue::kClientError:
      return "client_error";
  }
  return "unknown";
}

AiSidecarRuntimeService::AiSidecarRuntimeService(AiSidecarClientFactory client_factory,
                                                 QObject*               parent)
    : QObject(parent), client_factory_(std::move(client_factory)) {
  if (!client_factory_) {
    client_factory_ = [](const std::string& endpoint) {
      return sidecar_client::MakeGrpcClient(endpoint);
    };
  }

  status_.state   = AiSidecarRuntimeState::kStopped;
  status_.issue   = AiSidecarRuntimeIssue::kNone;
  status_.message = "Semantic runtime is stopped";

  connect(&process_, &QProcess::readyReadStandardOutput, this,
          [this]() { AppendStdout(process_.readAllStandardOutput()); });
  connect(&process_, &QProcess::readyReadStandardError, this,
          [this]() { AppendStderr(process_.readAllStandardError()); });
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      SetStatus(AiSidecarRuntimeState::kFailed, AiSidecarRuntimeIssue::kStartFailed,
                process_.errorString().toStdString());
    }
  });
  connect(&process_, &QProcess::finished, this, [this]() {
    if (idle_stop_pending_) {
      FinishIdleStop();
    }
  });

  idle_stop_timer_.setSingleShot(true);
  connect(&idle_stop_timer_, &QTimer::timeout, this, [this]() {
    if (active_leases_ == 0 && IsRunning()) {
      BeginIdleStop();
    }
  });
  idle_stop_kill_timer_.setSingleShot(true);
  connect(&idle_stop_kill_timer_, &QTimer::timeout, this, [this]() { OnIdleStopKillTimeout(); });
}

AiSidecarRuntimeService::~AiSidecarRuntimeService() { StopForProjectClose(); }

auto AiSidecarRuntimeService::StartAndWait(const AiSidecarRuntimeOptions& options) -> bool {
  if (QThread::currentThread() != thread()) {
    bool result = false;
    QMetaObject::invokeMethod(
        this, [this, options, &result]() { result = StartAndWait(options); },
        Qt::BlockingQueuedConnection);
    return result;
  }
  return StartAndWaitBody(options, /*interactive=*/false);
}

auto AiSidecarRuntimeService::StartAndWaitInteractive(const AiSidecarRuntimeOptions& options)
    -> bool {
  // The nested event processing below is only valid on the service's own
  // thread; callers from other threads must use the synchronous StartAndWait.
  if (QThread::currentThread() != thread()) {
    bool result = false;
    QMetaObject::invokeMethod(
        this, [this, options, &result]() { result = StartAndWaitInteractive(options); },
        Qt::BlockingQueuedConnection);
    return result;
  }
  cancel_start_requested_.store(false);
  interactive_starting_.store(true);
  const bool result = StartAndWaitBody(options, /*interactive=*/true);
  interactive_starting_.store(false);
  return result;
}

void AiSidecarRuntimeService::RequestCancelStart() { cancel_start_requested_.store(true); }

auto AiSidecarRuntimeService::StartAndWaitBody(const AiSidecarRuntimeOptions& options,
                                               bool interactive) -> bool {
  // An idle stop has sent the terminate request; the process will not serve this start.
  if (idle_stop_pending_ && !CompleteIdleStopBeforeStart()) {
    return false;
  }
  if (IsRunning()) {
    const auto requested_options = NormalizeRuntimeOptions(options);
    // The process stays running between operations, so it can hold an older model or older
    // provider configs. Restart it when no other operation holds a lease; with other leases
    // the running process is reused and the caller reports a model mismatch.
    const bool model_changed     = ModelIdentityDiffers(requested_options);
    if (active_leases_ <= 1 && (model_changed || provider_configs_changed_)) {
      qCInfo(diag::semanticLog).noquote()
          << QStringLiteral("semantic.runtime.restart pid=%1 reason=%2 model_id=%3")
                 .arg(status_.process_id)
                 .arg(model_changed ? QStringLiteral("model") : QStringLiteral("provider_configs"))
                 .arg(QString::fromStdString(requested_options.model_id));
      Stop();
      if (IsRunning()) {
        return false;
      }
    }
  }
  if (IsRunning()) {
    const auto requested_options = NormalizeRuntimeOptions(options);
    if (requested_options.require_model_info && !status_.model_info.has_value()) {
      options_.require_model_info   = true;
      options_.startup_timeout      = requested_options.startup_timeout;
      options_.health_poll_interval = requested_options.health_poll_interval;
      return WaitForReadiness(false, interactive);
    }
    if (status_.state != AiSidecarRuntimeState::kReady) {
      options_.startup_timeout      = requested_options.startup_timeout;
      options_.health_poll_interval = requested_options.health_poll_interval;
      return WaitForReadiness(false, interactive);
    }
    return true;
  }

  options_ = NormalizeRuntimeOptions(options);
  // The new process reads the current provider configs.
  provider_configs_changed_ = false;
  if (options_.port == 0) {
    options_.port = ChoosePort();
  }
  endpoint_ = BuildEndpoint(options_.host, options_.port);
  client_.reset();
  status_.stdout_tail.clear();
  status_.stderr_tail.clear();
  status_.model_info.reset();
  status_.remote_status.reset();

  std::error_code ec;
  if (!std::filesystem::exists(options_.runtime_binary, ec) || ec) {
    SetStatus(AiSidecarRuntimeState::kFailed, AiSidecarRuntimeIssue::kBinaryMissing,
              "Semantic runtime binary was not found: " + options_.runtime_binary.string());
    return false;
  }
  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral(
             "semantic.runtime.start binary=%1 endpoint=%2 model_id=%3 revision=%4 "
             "model_root=%5 device=%6 interactive=%7")
             .arg(QString::fromStdString(options_.runtime_binary.string()),
                  QString::fromStdString(endpoint_), QString::fromStdString(options_.model_id),
                  QString::fromStdString(options_.revision),
                  QString::fromStdString(options_.model_root.string()),
                  QString::fromStdString(options_.device),
                  interactive ? QStringLiteral("true") : QStringLiteral("false"));
  SetStatus(AiSidecarRuntimeState::kStarting, AiSidecarRuntimeIssue::kNone,
            "Starting semantic runtime");
  process_.setProgram(QString::fromStdString(options_.runtime_binary.string()));
  process_.setArguments(BuildArguments());
  process_.setProcessChannelMode(QProcess::SeparateChannels);
#ifdef _WIN32
  process_.setProcessEnvironment(
      AddWindowsUserProxyEnvironment(QProcessEnvironment::systemEnvironment()));
#endif
  process_.start();

  if (!process_.waitForStarted(static_cast<int>(options_.startup_timeout.count()))) {
    SetStatus(AiSidecarRuntimeState::kFailed, AiSidecarRuntimeIssue::kStartFailed,
              process_.errorString().toStdString());
    return false;
  }
  status_.process_id = static_cast<int64_t>(process_.processId());
  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral("semantic.runtime.started pid=%1 endpoint=%2")
             .arg(status_.process_id)
             .arg(QString::fromStdString(endpoint_));
  AttachChildTreeCleanup();

  client_ = client_factory_ ? client_factory_(endpoint_) : nullptr;
  if (!client_) {
    if (process_.state() != QProcess::NotRunning) {
      process_.terminate();
      if (!process_.waitForFinished(static_cast<int>(options_.graceful_stop_timeout.count()))) {
        process_.kill();
        process_.waitForFinished(static_cast<int>(options_.kill_timeout.count()));
      }
    }
    ReleaseChildTreeCleanup();
    status_.process_id = 0;
    SetStatus(AiSidecarRuntimeState::kFailed, AiSidecarRuntimeIssue::kClientUnavailable,
              "AI sidecar client session is not available");
    return false;
  }

  return WaitForReadiness(true, interactive);
}

auto AiSidecarRuntimeService::AcquireLease() -> std::shared_ptr<void> {
  if (QThread::currentThread() != thread()) {
    std::shared_ptr<void> lease;
    QMetaObject::invokeMethod(
        this, [this, &lease]() { lease = AcquireLease(); }, Qt::BlockingQueuedConnection);
    return lease;
  }

  ++active_leases_;
  idle_stop_timer_.stop();
  QPointer<AiSidecarRuntimeService> self(this);
  return std::shared_ptr<void>(new int(0), [self](int* token) {
    delete token;
    if (self) {
      self->ReleaseLease();
    }
  });
}

void AiSidecarRuntimeService::Stop() {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this]() { Stop(); }, Qt::BlockingQueuedConnection);
    return;
  }

  // The blocking stop below replaces an idle stop that has not finished.
  idle_stop_timer_.stop();
  idle_stop_kill_timer_.stop();
  idle_stop_pending_        = false;
  idle_stop_kill_sent_      = false;
  provider_configs_changed_ = false;

  if (!IsRunning()) {
    client_.reset();
    SetStatus(AiSidecarRuntimeState::kStopped, AiSidecarRuntimeIssue::kNone,
              "Semantic runtime is stopped");
    return;
  }

  SetStatus(AiSidecarRuntimeState::kStopping, AiSidecarRuntimeIssue::kNone,
            "Stopping semantic runtime");
  qCInfo(diag::semanticLog).noquote() << QStringLiteral("semantic.runtime.stop pid=%1 endpoint=%2")
                                             .arg(status_.process_id)
                                             .arg(QString::fromStdString(endpoint_));
  process_.terminate();
  if (!process_.waitForFinished(static_cast<int>(options_.graceful_stop_timeout.count()))) {
    process_.kill();
    if (!process_.waitForFinished(static_cast<int>(options_.kill_timeout.count()))) {
      SetStatus(AiSidecarRuntimeState::kFailed, AiSidecarRuntimeIssue::kStopTimedOut,
                "Semantic runtime did not exit after kill request");
      return;
    }
  }
  ReleaseChildTreeCleanup();
  client_.reset();
  status_.process_id = 0;
  SetStatus(AiSidecarRuntimeState::kStopped, AiSidecarRuntimeIssue::kNone,
            "Semantic runtime is stopped");
}

void AiSidecarRuntimeService::StopForProjectClose() {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(
        this, [this]() { StopForProjectClose(); }, Qt::BlockingQueuedConnection);
    return;
  }
  active_leases_ = 0;
  Stop();
}

void AiSidecarRuntimeService::ReleaseLease() {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(this, [this]() { ReleaseLease(); }, Qt::BlockingQueuedConnection);
    return;
  }

  if (active_leases_ > 0) {
    --active_leases_;
  }
  if (active_leases_ != 0 || idle_stop_pending_ || !IsRunning()) {
    return;
  }
  if (idle_stop_delay_.count() == 0) {
    Stop();
  } else if (idle_stop_delay_.count() > 0) {
    idle_stop_timer_.start(idle_stop_delay_);
  }
}

void AiSidecarRuntimeService::SetIdleStopDelay(std::chrono::milliseconds delay) {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(
        this, [this, delay]() { SetIdleStopDelay(delay); }, Qt::BlockingQueuedConnection);
    return;
  }
  idle_stop_delay_ = delay;
  qCInfo(diag::semanticLog).noquote() << QStringLiteral("semantic.runtime.idle_stop_delay ms=%1")
                                             .arg(static_cast<qlonglong>(delay.count()));
  if (active_leases_ != 0 || idle_stop_pending_ || !IsRunning()) {
    return;
  }
  if (delay.count() == 0) {
    idle_stop_timer_.stop();
    BeginIdleStop();
  } else if (delay.count() > 0) {
    idle_stop_timer_.start(delay);
  } else {
    idle_stop_timer_.stop();
  }
}

void AiSidecarRuntimeService::MarkProviderConfigsChanged() {
  if (QThread::currentThread() != thread()) {
    QMetaObject::invokeMethod(
        this, [this]() { MarkProviderConfigsChanged(); }, Qt::BlockingQueuedConnection);
    return;
  }
  provider_configs_changed_ = true;
}

void AiSidecarRuntimeService::BeginIdleStop() {
  if (idle_stop_pending_ || !IsRunning()) {
    return;
  }
  SetStatus(AiSidecarRuntimeState::kStopping, AiSidecarRuntimeIssue::kNone,
            "Stopping idle semantic runtime");
  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral("semantic.runtime.idle_stop pid=%1 endpoint=%2")
             .arg(status_.process_id)
             .arg(QString::fromStdString(endpoint_));
  idle_stop_pending_   = true;
  idle_stop_kill_sent_ = false;
  process_.terminate();
  idle_stop_kill_timer_.start(options_.graceful_stop_timeout);
}

void AiSidecarRuntimeService::OnIdleStopKillTimeout() {
  if (!idle_stop_pending_) {
    return;
  }
  if (process_.state() == QProcess::NotRunning) {
    FinishIdleStop();
    return;
  }
  if (!idle_stop_kill_sent_) {
    idle_stop_kill_sent_ = true;
    process_.kill();
    idle_stop_kill_timer_.start(options_.kill_timeout);
    return;
  }
  idle_stop_pending_   = false;
  idle_stop_kill_sent_ = false;
  SetStatus(AiSidecarRuntimeState::kFailed, AiSidecarRuntimeIssue::kStopTimedOut,
            "Semantic runtime did not exit after kill request");
}

void AiSidecarRuntimeService::FinishIdleStop() {
  idle_stop_kill_timer_.stop();
  idle_stop_pending_        = false;
  idle_stop_kill_sent_      = false;
  provider_configs_changed_ = false;
  AppendStdout(process_.readAllStandardOutput());
  AppendStderr(process_.readAllStandardError());
  ReleaseChildTreeCleanup();
  client_.reset();
  status_.process_id = 0;
  SetStatus(AiSidecarRuntimeState::kStopped, AiSidecarRuntimeIssue::kNone,
            "Semantic runtime is stopped");
}

auto AiSidecarRuntimeService::CompleteIdleStopBeforeStart() -> bool {
  idle_stop_kill_timer_.stop();
  if (process_.state() != QProcess::NotRunning) {
    process_.kill();
    process_.waitForFinished(static_cast<int>(options_.kill_timeout.count()));
  }
  if (process_.state() != QProcess::NotRunning) {
    idle_stop_pending_   = false;
    idle_stop_kill_sent_ = false;
    SetStatus(AiSidecarRuntimeState::kFailed, AiSidecarRuntimeIssue::kStopTimedOut,
              "Semantic runtime did not exit after kill request");
    return false;
  }
  // waitForFinished emits finished(), whose handler may already have finished the stop.
  if (idle_stop_pending_) {
    FinishIdleStop();
  }
  return true;
}

auto AiSidecarRuntimeService::ModelIdentityDiffers(const AiSidecarRuntimeOptions& requested) const
    -> bool {
  return requested.model_id != options_.model_id || requested.revision != options_.revision ||
         ComparablePath(requested.model_root) != ComparablePath(options_.model_root);
}

auto AiSidecarRuntimeService::Status() -> AiSidecarRuntimeStatusSnapshot {
  if (QThread::currentThread() != thread()) {
    AiSidecarRuntimeStatusSnapshot snapshot;
    QMetaObject::invokeMethod(
        this, [this, &snapshot]() { snapshot = Status(); }, Qt::BlockingQueuedConnection);
    return snapshot;
  }

  RefreshProcessExit();
  if (status_.state == AiSidecarRuntimeState::kReady && client_) {
    AiSidecarRuntimeRemoteStatus remote;
    std::string                  error;
    if (client_->runtime().GetRuntimeStatus(std::chrono::milliseconds(250), &remote, &error)) {
      status_.remote_status = remote;
    }
  }
  return status_;
}

auto AiSidecarRuntimeService::IsRunning() -> bool {
  if (QThread::currentThread() != thread()) {
    bool result = false;
    QMetaObject::invokeMethod(
        this, [this, &result]() { result = IsRunning(); }, Qt::BlockingQueuedConnection);
    return result;
  }

  RefreshProcessExit();
  return process_.state() != QProcess::NotRunning;
}

auto AiSidecarRuntimeService::StateName() const -> QString {
  return QString::fromLatin1(ToString(status_.state));
}

auto AiSidecarRuntimeService::IssueName() const -> QString {
  return QString::fromLatin1(ToString(status_.issue));
}

void AiSidecarRuntimeService::SetStatus(AiSidecarRuntimeState state, AiSidecarRuntimeIssue issue,
                                        std::string message) {
  status_.state    = state;
  status_.issue    = issue;
  status_.message  = std::move(message);
  status_.endpoint = endpoint_;
  qCInfo(diag::semanticLog).noquote()
      << QStringLiteral("semantic.runtime.status state=%1 issue=%2 endpoint=%3 message=%4")
             .arg(QString::fromLatin1(ToString(state)), QString::fromLatin1(ToString(issue)),
                  QString::fromStdString(endpoint_), QString::fromStdString(status_.message));
  emit statusChanged();
}

void AiSidecarRuntimeService::AppendStdout(const QByteArray& bytes) {
  TailAppend(&status_.stdout_tail, bytes);
  const QString text = QString::fromUtf8(bytes).trimmed();
  if (!text.isEmpty()) {
    qCInfo(diag::semanticLog).noquote()
        << QStringLiteral("semantic.runtime.stdout %1").arg(text.left(1000));
  }
}

void AiSidecarRuntimeService::AppendStderr(const QByteArray& bytes) {
  TailAppend(&status_.stderr_tail, bytes);
  const QString text = QString::fromUtf8(bytes).trimmed();
  if (!text.isEmpty()) {
    qCWarning(diag::semanticLog).noquote()
        << QStringLiteral("semantic.runtime.stderr %1").arg(text.left(1000));
  }
}

void AiSidecarRuntimeService::RefreshProcessExit() {
  if (process_.state() != QProcess::NotRunning) {
    process_.waitForFinished(0);
  }
  if (process_.state() != QProcess::NotRunning) {
    return;
  }
  if (idle_stop_pending_) {
    FinishIdleStop();
    return;
  }
  if (status_.state != AiSidecarRuntimeState::kReady &&
      status_.state != AiSidecarRuntimeState::kStarting &&
      status_.state != AiSidecarRuntimeState::kStopping) {
    return;
  }

  ReleaseChildTreeCleanup();
  client_.reset();
  AppendStdout(process_.readAllStandardOutput());
  AppendStderr(process_.readAllStandardError());
  status_.process_id = 0;
  const bool crashed = process_.exitStatus() == QProcess::CrashExit || process_.exitCode() != 0;
  std::ostringstream message;
  message << "Semantic runtime exited with code " << process_.exitCode();
  SetStatus(
      AiSidecarRuntimeState::kFailed,
      crashed ? AiSidecarRuntimeIssue::kRuntimeCrashed : AiSidecarRuntimeIssue::kRuntimeExited,
      message.str());
}

auto AiSidecarRuntimeService::BuildArguments() const -> QStringList {
  QStringList args;
  args << "--host" << QString::fromStdString(options_.host);
  args << "--port" << QString::number(options_.port);
  if (!options_.model_root.empty()) {
    args << "--model-root" << QString::fromStdString(options_.model_root.string());
  }
  args << "--model-id" << QString::fromStdString(options_.model_id);
  if (!options_.revision.empty()) {
    args << "--revision" << QString::fromStdString(options_.revision);
  }
  if (!options_.hf_endpoint.empty()) {
    args << "--hf-endpoint" << QString::fromStdString(options_.hf_endpoint);
  }
  args << "--device" << QString::fromStdString(options_.device);
  args << "--text-device" << QString::fromStdString(options_.text_device);
  if (!options_.vision_device.empty()) {
    args << "--vision-device" << QString::fromStdString(options_.vision_device);
  }
  // The idle stop delay is in milliseconds; a negative delay keeps the text encoder loaded.
  const auto text_idle_unload = options_.text_idle_unload.value_or(
      idle_stop_delay_.count() < 0
          ? std::chrono::seconds(-1)
          : std::chrono::duration_cast<std::chrono::seconds>(idle_stop_delay_));
  args << "--text-idle-unload-seconds" << QString::number(text_idle_unload.count());
  args << "--vision-idle-unload-seconds" << QString::number(options_.vision_idle_unload.count());
  args << (options_.allow_download ? "--allow-download" : "--no-download");
  args << "--batch-cap" << QString::number(options_.batch_cap);
  args << "--batch-wait-ms" << QString::number(options_.batch_wait_ms);
  args << "--max-message-bytes" << QString::number(options_.max_message_bytes);
  if (!options_.provider_config_dir.empty()) {
    args << "--provider-config-dir"
         << QString::fromStdString(options_.provider_config_dir.string());
  }
  for (const auto& arg : options_.extra_arguments) {
    args << QString::fromStdString(arg);
  }
  return args;
}

auto AiSidecarRuntimeService::ChoosePort() const -> uint16_t {
  QTcpServer server;
  if (server.listen(QHostAddress::LocalHost, 0)) {
    const auto port = static_cast<uint16_t>(server.serverPort());
    server.close();
    return port;
  }
  return 50051;
}

auto AiSidecarRuntimeService::WaitForReadiness(bool terminate_on_timeout, bool interactive)
    -> bool {
  const auto  deadline = std::chrono::steady_clock::now() + options_.startup_timeout;
  std::string last_error;
  while (std::chrono::steady_clock::now() < deadline) {
    if (interactive) {
      // The event loop runs during the poll interval, so the process pipes drain through
      // their notifiers and the UI keeps painting.
      PumpEventsFor(options_.health_poll_interval);
    } else {
      process_.waitForReadyRead(static_cast<int>(options_.health_poll_interval.count()));
    }
    RefreshProcessExit();
    if (status_.state == AiSidecarRuntimeState::kFailed) {
      return false;
    }
    if (interactive && process_.state() == QProcess::NotRunning) {
      // A Stop() ran from the event loop above.
      return false;
    }
    ReadinessProbe probe;
    if (client_) {
      probe = interactive ? RunProbeWhilePumpingEvents(client_, options_.health_poll_interval,
                                                       options_.require_model_info)
                          : RunReadinessProbe(client_, options_.health_poll_interval,
                                              options_.require_model_info);
      if (interactive && process_.state() == QProcess::NotRunning) {
        return false;
      }
      if (!probe.ping_ok_) {
        last_error = probe.ping_error_;
      }
    }
    if (probe.ping_ok_) {
      const auto& info       = probe.model_info_;
      const auto& info_error = probe.model_info_error_;
      if (probe.model_info_ok_) {
        status_.model_info = info;
        SetStatus(AiSidecarRuntimeState::kReady, AiSidecarRuntimeIssue::kNone,
                  "Semantic runtime is ready");
        return true;
      }
      if (!options_.require_model_info) {
        SetStatus(AiSidecarRuntimeState::kReady, AiSidecarRuntimeIssue::kNone,
                  "Semantic model manager is ready");
        return true;
      }
      if (!info_error.empty()) {
        last_error      = info_error;
        status_.message = info_error;
      } else {
        last_error = "Semantic runtime responded but semantic model is not ready";
      }
    }
    if (interactive) {
      // A cancel request checkpointed between polls: terminate the process and
      // return to kStopped so the caller can treat it as a user cancellation
      // rather than a start failure.
      if (cancel_start_requested_.load()) {
        SetStatus(AiSidecarRuntimeState::kStopping, AiSidecarRuntimeIssue::kNone,
                  "Cancelling semantic runtime start");
        if (process_.state() != QProcess::NotRunning) {
          process_.terminate();
          if (!process_.waitForFinished(
                  static_cast<int>(options_.graceful_stop_timeout.count()))) {
            process_.kill();
            process_.waitForFinished(static_cast<int>(options_.kill_timeout.count()));
          }
        }
        ReleaseChildTreeCleanup();
        client_.reset();
        status_.process_id = 0;
        SetStatus(AiSidecarRuntimeState::kStopped, AiSidecarRuntimeIssue::kNone,
                  "Semantic runtime start was cancelled");
        return false;
      }
    } else {
      std::this_thread::sleep_for(options_.health_poll_interval);
    }
  }

  const std::string message =
      last_error.empty() ? "Timed out waiting for semantic runtime readiness" : last_error;
  if (!terminate_on_timeout && IsRunning()) {
    status_.message = message;
    emit statusChanged();
    return false;
  }

  SetStatus(AiSidecarRuntimeState::kFailed, AiSidecarRuntimeIssue::kReadinessTimeout, message);
  if (process_.state() != QProcess::NotRunning) {
    process_.terminate();
    if (!process_.waitForFinished(static_cast<int>(options_.graceful_stop_timeout.count()))) {
      process_.kill();
      process_.waitForFinished(static_cast<int>(options_.kill_timeout.count()));
    }
  }
  ReleaseChildTreeCleanup();
  client_.reset();
  status_.process_id = 0;
  return false;
}

void AiSidecarRuntimeService::AttachChildTreeCleanup() {
#ifdef _WIN32
  ReleaseChildTreeCleanup();
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) {
    return;
  }
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
  info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
    CloseHandle(job);
    return;
  }
  HANDLE process_handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE, FALSE,
                                      static_cast<DWORD>(process_.processId()));
  if (process_handle == nullptr) {
    CloseHandle(job);
    return;
  }
  if (!AssignProcessToJobObject(job, process_handle)) {
    CloseHandle(process_handle);
    CloseHandle(job);
    return;
  }
  CloseHandle(process_handle);
  job_object_ = job;
#endif
}

void AiSidecarRuntimeService::ReleaseChildTreeCleanup() {
#ifdef _WIN32
  if (job_object_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(job_object_));
    job_object_ = nullptr;
  }
#endif
}

}  // namespace alcedo
