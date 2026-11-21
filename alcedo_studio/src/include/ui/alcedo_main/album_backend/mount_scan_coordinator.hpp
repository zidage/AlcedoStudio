//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/mount_service.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/mount_directory_watcher.hpp"

namespace alcedo::ui {

class BackgroundTaskController;

/// What asked for a scan (Mounted Folder Plan, MF7).
enum class MountScanTrigger : std::uint8_t {
  kProjectOpen,
  /// The application became active after kMountScanActivationInterval or more in the
  /// background.
  kActivation,
  /// The watcher delay passed after the last change notification of the mount.
  kDirectoryChange,
  /// The user asked for the scan (Synchronize). The task shows from the start of the scan.
  kSynchronize,
};

/// Time after the last change notification of a mount before its scan starts (section 14).
inline constexpr std::chrono::milliseconds kMountScanWatcherDelay{5000};
/// Time in the background before an activation of the application starts the scans.
inline constexpr std::chrono::milliseconds kMountScanActivationInterval{60000};

/// The mounts of the open project for the coordinator. A test can replace the mount service.
class MountScanPort {
 public:
  virtual ~MountScanPort()                               = default;
  /// Every mount of the project. Called on the UI thread.
  virtual auto ListMounts() -> std::vector<MountSummary> = 0;
  /**
   * @brief Scan one mount (MountService::ScanMount). Called on the worker thread of the
   *        coordinator; blocks for the length of the scan.
   * @param on_progress Called on the worker thread after each stored batch.
   */
  virtual auto ScanMount(mount_id_t mount_id, const std::atomic<bool>& stop_requested,
                         const std::function<void(const MountScanProgress&)>& on_progress)
      -> MountScanResult = 0;
};

/// The production port: MountService::ScanMount with the import service of the project.
class ProjectMountScanPort final : public MountScanPort {
 public:
  /// @param invalidate_thumbnail Called on the worker thread for each file that changed in
  ///        place, moved, or returned.
  ProjectMountScanPort(std::shared_ptr<MountService> mounts, MountFileImporter* importer,
                       std::function<void(sl_element_id_t)> invalidate_thumbnail);

  auto ListMounts() -> std::vector<MountSummary> override;
  auto ScanMount(mount_id_t mount_id, const std::atomic<bool>& stop_requested,
                 const std::function<void(const MountScanProgress&)>& on_progress)
      -> MountScanResult override;

 private:
  std::shared_ptr<MountService>        mounts_;
  MountFileImporter*                   importer_ = nullptr;
  std::function<void(sl_element_id_t)> invalidate_thumbnail_;
};

/// Steady time and single-shot timers of the coordinator. A test advances the time by hand.
class MountScanClock {
 public:
  using TimePoint                                     = std::chrono::steady_clock::time_point;

  virtual ~MountScanClock()                           = default;
  [[nodiscard]] virtual auto Now() const -> TimePoint = 0;
  /// Run @p callback once after @p delay on the UI thread. A timer of the same mount restarts.
  virtual void               StartTimer(mount_id_t mount_id, std::chrono::milliseconds delay,
                                        std::function<void()> callback) = 0;
  virtual void               StopTimer(mount_id_t mount_id)             = 0;
  virtual void               StopAllTimers()                            = 0;
};

/// QTimer and std::chrono::steady_clock.
auto CreateSteadyMountScanClock() -> std::unique_ptr<MountScanClock>;

/// The fixed parts of a coordinator.
struct MountScanCoordinatorDependencies {
  /// Task list of the application; required.
  BackgroundTaskController*              tasks = nullptr;
  /// Required.
  std::unique_ptr<MountScanClock>        clock;
  /// Required.
  std::unique_ptr<MountDirectoryWatcher> watcher;
  std::chrono::milliseconds              watcher_delay       = kMountScanWatcherDelay;
  std::chrono::milliseconds              activation_interval = kMountScanActivationInterval;
  /// Called on the UI thread after each stored batch of a scan, so the Library shows the photos
  /// while the scan imports them.
  std::function<void(mount_id_t)>        on_batch_stored;
  /// Called on the UI thread when a scan ends.
  std::function<void(mount_id_t, const MountScanResult&)> on_scan_ended;
};

/**
 * @brief Starts the scans of the mounts of the open project (Mounted Folder Plan, MF7).
 *
 * Triggers: Start (project open), an activation of the application after the activation
 * interval or more in the background, the watcher delay after the last change notification of
 * a mount, and RequestScan.
 *
 * One scan runs at a time. A request for the mount of the running scan sets its "scan again"
 * flag: when the scan ends, one more scan of that mount starts. Production interleaving: the
 * user copies 500 files into the directory; the watcher starts a scan after the first 200; the
 * other 300 arrive after the listing of that scan, and the second scan finds them. A request
 * for a mount that waits for its scan changes nothing.
 *
 * The background task of a scan (BackgroundTaskKind::MountScan) shows only when the scan changes
 * the library. A scan that RequestScan started with kSynchronize shows from its start. An
 * automatic scan shows when it starts to import new files, or at its end when it moved, changed,
 * returned, or lost photos. An automatic scan that finds the mount offline shows only when the
 * mount was not offline before.
 *
 * After each scan that read the listing, the coordinator registers the directories of the
 * listing with the watcher. A mount whose registration fails is "not watched"; the other
 * triggers still scan it.
 *
 * Thread safety: UI thread only. The scans run on one worker thread.
 */
class MountScanCoordinator final : public QObject {
  Q_OBJECT

 public:
  explicit MountScanCoordinator(MountScanCoordinatorDependencies dependencies,
                                QObject*                         parent = nullptr);
  /// Stop().
  ~MountScanCoordinator() override;

  MountScanCoordinator(const MountScanCoordinator&)                                  = delete;
  auto               operator=(const MountScanCoordinator&) -> MountScanCoordinator& = delete;

  /// Start for an entered project and request a kProjectOpen scan of each mount. A running
  /// coordinator stops first.
  void               Start(std::shared_ptr<MountScanPort> port);

  /**
   * @brief Stop before the project closes: stop the timers and the watcher, request the stop of
   *        the running scan, and wait for it. The running scan keeps its stored batches
   *        (MountScanStatus::kCanceled). No effect when the coordinator is not started.
   */
  void               Stop();

  /// Request a scan of @p mount_id. No effect when the coordinator is not started or the mount
  /// does not exist.
  void               RequestScan(mount_id_t mount_id, MountScanTrigger trigger);

  /// Read the mounts again (after a mount was created or removed). A removed mount loses its
  /// watch and its queued scan.
  void               ReloadMounts();

  /// Pass each change of Qt::ApplicationState: @p active is true for Qt::ApplicationActive.
  void               HandleApplicationActiveChanged(bool active);

  [[nodiscard]] auto IsStarted() const -> bool { return port_ != nullptr; }
  /// True when the watcher holds the mount.
  [[nodiscard]] auto IsWatched(mount_id_t mount_id) const -> bool;
  /// True while a scan of @p mount_id runs.
  [[nodiscard]] auto IsScanRunning(mount_id_t mount_id) const -> bool;
  /// True while a scan runs or waits to run.
  [[nodiscard]] auto HasPendingScans() const -> bool;

 private:
  struct MountState {
    std::filesystem::path           root;
    QString                         title;
    std::optional<MountScanTrigger> queued;
    std::optional<MountScanTrigger> scan_again;
    MountReachability               reachability = MountReachability::kNotScanned;
    bool                            watched      = false;
  };

  struct RunningScan {
    mount_id_t                         mount_id = 0;
    MountScanTrigger                   trigger  = MountScanTrigger::kProjectOpen;
    std::shared_ptr<std::atomic<bool>> stop_requested;
    std::thread                        worker;
    QString                            task_id;
    bool                               canceled_by_user = false;
  };

  void HandleDirectoryChanged(mount_id_t mount_id);
  void StartNextScan();
  void HandleScanProgress(quint64 generation, mount_id_t mount_id,
                          const MountScanProgress& progress);
  void HandleScanEnded(quint64 generation, mount_id_t mount_id, const MountScanResult& result);
  void ReportScanEnd(const RunningScan& run, const MountState& state, const MountScanResult& result,
                     MountReachability previous_reachability);
  void UpdateWatch(mount_id_t mount_id, MountState& state, const MountScanResult& result);
  auto RegisterTask(const MountState& state, const QString& detail, int progress_percent)
      -> QString;
  void                                                    CancelByUser(quint64 generation);

  BackgroundTaskController*                               tasks_ = nullptr;
  std::unique_ptr<MountScanClock>                         clock_;
  std::unique_ptr<MountDirectoryWatcher>                  watcher_;
  std::chrono::milliseconds                               watcher_delay_;
  std::chrono::milliseconds                               activation_interval_;
  std::function<void(mount_id_t)>                         on_batch_stored_;
  std::function<void(mount_id_t, const MountScanResult&)> on_scan_ended_;

  std::shared_ptr<MountScanPort>                          port_;
  std::unordered_map<mount_id_t, MountState>              mounts_;
  std::deque<mount_id_t>                                  queue_;
  std::optional<RunningScan>                              running_;
  /// Changes at each Start and Stop, so a queued call of an earlier run does nothing.
  quint64                                                 generation_ = 0;
  std::optional<MountScanClock::TimePoint>                inactive_since_;
};

/// The task detail of a scan result: the counts of new, moved, changed, missing, and returned
/// photos, or "No changes".
auto MountScanResultDetail(const MountScanResult& result) -> QString;

/// True when the scan changed a photo of the library or could not import one.
auto MountScanChangedPhotos(const MountScanResult& result) -> bool;

}  // namespace alcedo::ui
