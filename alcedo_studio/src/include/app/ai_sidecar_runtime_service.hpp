//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sidecar_client/client.hpp"
#include "sidecar_client/dto/runtime.hpp"

namespace alcedo {

enum class AiSidecarRuntimeState : uint8_t {
  kStopped = 0,
  kStarting,
  kReady,
  kStopping,
  kFailed,
};

enum class AiSidecarRuntimeIssue : uint8_t {
  kNone = 0,
  kBinaryMissing,
  kStartFailed,
  kReadinessTimeout,
  kRuntimeExited,
  kRuntimeCrashed,
  kStopTimedOut,
  kClientUnavailable,
  kClientError,
};

struct AiSidecarRuntimeStatusSnapshot {
  AiSidecarRuntimeState                       state = AiSidecarRuntimeState::kStopped;
  AiSidecarRuntimeIssue                       issue = AiSidecarRuntimeIssue::kNone;
  std::string                                 message;
  std::string                                 endpoint;
  int64_t                                     process_id = 0;
  std::string                                 stdout_tail;
  std::string                                 stderr_tail;
  std::optional<AiSidecarRuntimeModelInfo>    model_info;
  std::optional<AiSidecarRuntimeRemoteStatus> remote_status;
};

struct AiSidecarRuntimeOptions {
  std::filesystem::path     runtime_binary;
  std::filesystem::path     model_root;
  std::filesystem::path     provider_config_dir;
  std::string               host     = "127.0.0.1";
  uint16_t                  port     = 0;
  /// The CLIP model of the open project's active model. Empty starts the sidecar without
  /// `--model-id`: semantic requests then report that no model is active, and segmentation and
  /// image analysis run (AI Models plan, P10).
  std::string                         model_id;
  std::string               revision;
  std::string               hf_endpoint        = "https://hf-mirror.com";
  std::string               device             = "auto";
  /// Device of the text encoder. The text encoder runs on the CPU (residency plan, 1.2 item 6).
  std::string                         text_device        = "cpu";
  /// Device of the image encoder; empty uses `device`.
  std::string                         vision_device;
  /// Idle unload time of the text encoder (-1 never, 0 after each request). Empty uses the
  /// idle stop delay of the service (SetIdleStopDelay), so "Keep AI model loaded" applies to
  /// both the process and the text encoder.
  std::optional<std::chrono::seconds> text_idle_unload;
  /// Idle unload time of the image encoder, with the same rule.
  std::chrono::seconds                vision_idle_unload{60};
  uint32_t                  batch_cap          = 512;
  uint32_t                  batch_wait_ms      = 25;
  uint32_t                  max_message_bytes  = 16 * 1024 * 1024;
  bool                      allow_download     = false;
  bool                      require_model_info = true;
  std::chrono::milliseconds startup_timeout{5000};
  std::chrono::milliseconds health_poll_interval{50};
  std::chrono::milliseconds graceful_stop_timeout{1000};
  std::chrono::milliseconds kill_timeout{1000};
  std::vector<std::string>  extra_arguments;
};

using AiSidecarClientFactory =
    std::function<std::shared_ptr<sidecar_client::Client>(const std::string& endpoint)>;

class AiSidecarRuntimeService final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString state READ StateName NOTIFY statusChanged)
  Q_PROPERTY(QString issue READ IssueName NOTIFY statusChanged)
  Q_PROPERTY(QString statusMessage READ StatusMessage NOTIFY statusChanged)
  Q_PROPERTY(QString endpoint READ EndpointQString NOTIFY statusChanged)

 public:
  explicit AiSidecarRuntimeService(AiSidecarClientFactory client_factory = {},
                                   QObject*               parent         = nullptr);
  ~AiSidecarRuntimeService() override;

  auto               StartAndWait(const AiSidecarRuntimeOptions& options) -> bool;
  // Starts the sidecar on the service's own (UI) thread and keeps its Qt event
  // loop running for the whole readiness wait: the poll interval runs a local
  // event loop, and the blocking Ping/GetModelInfo probe runs on a helper thread
  // while the event loop runs, so animations do not stutter during a cold boot.
  // Calls from other threads are queued to the service's thread. Checkpoints
  // RequestCancelStart() each poll and aborts the boot (terminating the
  // process, returning to kStopped) if cancellation is requested. Emits
  // statusChanged() for each state transition, exactly like StartAndWait.
  auto               StartAndWaitInteractive(const AiSidecarRuntimeOptions& options) -> bool;
  // Requests that an in-progress StartAndWaitInteractive abort at the next poll
  // checkpoint. Safe to call from any thread; a no-op when no interactive boot
  // is running.
  void               RequestCancelStart();
  // True while an interactive boot is in progress on the service's thread.
  bool               IsStartingInteractive() const { return interactive_starting_.load(); }
  [[nodiscard]] auto AcquireLease() -> std::shared_ptr<void>;
  void               Stop();
  void               StopForProjectClose();

  /**
   * @brief Sets how long the process keeps running after the last lease is released.
   *
   * 0 stops the process at once at the last release (blocking stop). A positive delay starts a
   * single-shot timer at the last release; when it expires with no lease, the process stops
   * without blocking the owner thread (terminate, then kill after `graceful_stop_timeout`). A
   * negative delay starts no timer: the process runs until StopForProjectClose(). A new lease
   * stops the timer. A change applies at once: with no lease and a running process, 0 starts
   * the stop, a positive delay restarts the timer with the new value, and a negative delay
   * stops the timer. Runs on the owner thread (other threads block on a queued call).
   */
  void               SetIdleStopDelay(std::chrono::milliseconds delay);
  auto               IdleStopDelay() const -> std::chrono::milliseconds { return idle_stop_delay_; }
  /**
   * @brief Records that the provider configs changed after the running process read them.
   *
   * The next start request made while no lease other than the caller's own is held restarts
   * the process, so the new process reads the new configs. Runs on the owner thread.
   */
  void               MarkProviderConfigsChanged();

  auto               Status() -> AiSidecarRuntimeStatusSnapshot;
  auto               Options() const -> const AiSidecarRuntimeOptions& { return options_; }
  auto               IsRunning() -> bool;
  auto               Endpoint() const -> std::string { return endpoint_; }
  auto ClientSession() const -> std::shared_ptr<sidecar_client::Client> { return client_; }

  auto StateName() const -> QString;
  auto IssueName() const -> QString;
  auto StatusMessage() const -> QString { return QString::fromStdString(status_.message); }
  auto EndpointQString() const -> QString { return QString::fromStdString(endpoint_); }

 signals:
  void statusChanged();

 private:
  void SetStatus(AiSidecarRuntimeState state, AiSidecarRuntimeIssue issue, std::string message);
  void AppendStdout(const QByteArray& bytes);
  void AppendStderr(const QByteArray& bytes);
  void RefreshProcessExit();
  auto BuildArguments() const -> QStringList;
  auto ChoosePort() const -> uint16_t;
  // Shared boot body for StartAndWait (non-interactive) and
  // StartAndWaitInteractive (interactive). When `interactive`, the readiness
  // poll pumps the Qt event loop between iterations and checkpoints
  // cancel_start_requested_ so the UI stays responsive and the boot is
  // cancellable. Must run on the service's own thread.
  auto StartAndWaitBody(const AiSidecarRuntimeOptions& options, bool interactive) -> bool;
  auto WaitForReadiness(bool terminate_on_timeout, bool interactive) -> bool;
  void AttachChildTreeCleanup();
  void ReleaseChildTreeCleanup();
  void ReleaseLease();
  // Idle stop without blocking the owner thread: terminate, then idle_stop_kill_timer_ escalates to
  // kill and finally to kStopTimedOut. The QProcess::finished handler or RefreshProcessExit calls
  // FinishIdleStop when the process exits.
  void BeginIdleStop();
  void OnIdleStopKillTimeout();
  void FinishIdleStop();
  // A start during an idle stop kills the exiting process and waits up to kill_timeout for it.
  auto CompleteIdleStopBeforeStart() -> bool;
  // Model identity (model_root, model_id, revision) of the running process differs from
  // @p requested.
  auto ModelIdentityDiffers(const AiSidecarRuntimeOptions& requested) const -> bool;

  AiSidecarClientFactory                  client_factory_;
  std::shared_ptr<sidecar_client::Client> client_;
  QProcess                                process_;
  AiSidecarRuntimeOptions                 options_;
  AiSidecarRuntimeStatusSnapshot          status_;
  std::string                             endpoint_;
  int                                     active_leases_ = 0;
  // Residency of the process after the last lease (SetIdleStopDelay). Owner thread only.
  std::chrono::milliseconds               idle_stop_delay_{0};
  QTimer                                  idle_stop_timer_;
  QTimer                                  idle_stop_kill_timer_;
  bool                                    idle_stop_pending_        = false;
  bool                                    idle_stop_kill_sent_      = false;
  bool                                    provider_configs_changed_ = false;
  // Interactive-boot state. `interactive_starting_` is set for the duration of
  // a StartAndWaitInteractive boot so IsStartingInteractive() can report it;
  // `cancel_start_requested_` is the cancel flag checkpointed each poll.
  std::atomic<bool>                       interactive_starting_{false};
  std::atomic<bool>                       cancel_start_requested_{false};
#ifdef _WIN32
  void* job_object_ = nullptr;
#endif
};

auto ToString(AiSidecarRuntimeState state) -> const char*;
auto ToString(AiSidecarRuntimeIssue issue) -> const char*;

}  // namespace alcedo
