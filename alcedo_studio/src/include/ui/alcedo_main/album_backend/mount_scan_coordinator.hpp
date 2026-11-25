//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QPointer>
#include <QString>
#include <atomic>
#include <chrono>
#include <deque>
#include <exception>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "app/mount_service.hpp"
#include "type/type.hpp"
#include "ui/alcedo_main/album_backend/mount_directory_watcher.hpp"

namespace alcedo::ui {

class BackgroundTaskController;
class MountScanHold;

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
  /// A volume event (#1020): the volume of the watched mount was removed or its removal was
  /// canceled, or a volume arrived while the mount was offline.
  kVolumeChange,
};

/// Time after the last change notification of a mount before its scan starts (section 14).
inline constexpr std::chrono::milliseconds kMountScanWatcherDelay{5000};
/// Time after the arrival of a volume before the scans of the offline mounts start. Windows
/// reports the arrival of the volume device before the drive letter is assigned.
inline constexpr std::chrono::milliseconds kMountScanVolumeArrivalDelay{2000};
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
   * @param find_moved_volume MountScanRequest::find_moved_volume; true for the scan of the
   *        project open and of a volume event.
   */
  virtual auto ScanMount(mount_id_t mount_id, const std::atomic<bool>& stop_requested,
                         const std::function<void(const MountScanProgress&)>& on_progress,
                         bool find_moved_volume) -> MountScanResult = 0;
  /**
   * @brief Save the project file after a scan that stored changes (MountScanStoredChanges;
   *        #1015). Called on the worker thread right after ScanMount, before the worker ends.
   * @return false when the save failed. An exception counts as a failure.
   */
  virtual auto SaveProject() -> bool                                = 0;
};

/// The production port: MountService::ScanMount with the import service of the project.
class ProjectMountScanPort final : public MountScanPort {
 public:
  /// @param invalidate_thumbnail Called on the worker thread for each file that changed in
  ///        place, moved, or returned.
  /// @param save_project ProjectHandler::MakeSaveAndPackageJob of the project: the metadata
  ///        save and the package write, the step that ends a user import. Required.
  ProjectMountScanPort(std::shared_ptr<MountService> mounts, MountFileImporter* importer,
                       std::function<void(sl_element_id_t)> invalidate_thumbnail,
                       std::function<bool(QString*)>        save_project);

  auto ListMounts() -> std::vector<MountSummary> override;
  auto ScanMount(mount_id_t mount_id, const std::atomic<bool>& stop_requested,
                 const std::function<void(const MountScanProgress&)>& on_progress,
                 bool find_moved_volume) -> MountScanResult override;
  auto SaveProject() -> bool override;

 private:
  std::shared_ptr<MountService>        mounts_;
  MountFileImporter*                   importer_ = nullptr;
  std::function<void(sl_element_id_t)> invalidate_thumbnail_;
  std::function<bool(QString*)>        save_project_;
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

/// Called on the UI thread with the result of the scan that a RequestScan asked for. A request
/// that the stop of the coordinator, a user cancel, or the removal of the mount ended gets a
/// MountScanStatus::kCanceled result.
using MountScanEndedCallback     = std::function<void(const MountScanResult&)>;

/// Called on the UI thread when a mount operation ended: empty on success, else the exception of
/// the operation (MountOperationError for a refusal). An operation that the stop of the
/// coordinator dropped gets a std::runtime_error.
using MountOperationDoneCallback = std::function<void(std::exception_ptr)>;

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
  std::chrono::milliseconds              watcher_delay        = kMountScanWatcherDelay;
  std::chrono::milliseconds              activation_interval  = kMountScanActivationInterval;
  std::chrono::milliseconds              volume_arrival_delay = kMountScanVolumeArrivalDelay;
  /// Called on the UI thread after each stored batch of a scan, so the Library shows the photos
  /// while the scan imports them.
  std::function<void(mount_id_t)>        on_batch_stored;
  /// Called on the UI thread when a scan ends.
  std::function<void(mount_id_t, const MountScanResult&)> on_scan_ended;
  /// The volume identity of a mount point for IsArrivedVolumeAt; empty takes ReadVolumeIdentity.
  std::function<std::string(const std::filesystem::path&)> read_volume_identity;
};

/**
 * @brief Starts the scans of the mounts of the open project (Mounted Folder Plan, MF7).
 *
 * The scan of the project open and the scan of a volume event search for a volume that moved to
 * another mount point (MountScanRequest::find_moved_volume, MF8). A scan that relocated the root
 * is watched at the new root.
 *
 * Triggers: Start (project open), an activation of the application after the activation
 * interval or more in the background, the watcher delay after the last change notification of
 * a mount, the volume events of the watcher, and RequestScan.
 *
 * Volume events (#1020): the removal of the volume of a watched mount, or the cancel of that
 * removal, requests a scan of the mount at once; the scan finds the mount offline or watches it
 * again. The arrival of a volume requests a scan of each offline mount after the volume arrival
 * delay; a connected drive brings its mount online and its missing photos return. A new mount
 * point whose volume holds no mount of the project emits VolumeWithoutMountArrived (#1076): no
 * mount has the identity of the volume, and no mount root is at or below the mount point. A
 * mount root at or below the mount point counts only when the mount or the volume has no known
 * identity: a mount of another volume that used the same mount point does not hide the card of
 * the new volume (#1087). A mount point that is gone emits MountPointRemoved, so the card of its
 * volume closes.
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
 * The mount operations of the Library and of automation (create, relocate, locate, remove,
 * unmount; Mounted Folder Plan, MF6 and MF8) run on the same worker through RunMountOperation,
 * one at a time, before the next queued scan. The UI thread never waits for the operation lock of
 * MountService, which a scan holds for its whole length.
 *
 * Save (#1015): after a scan that stored changes (MountScanStoredChanges), the worker saves the
 * project file through MountScanPort::SaveProject before it ends, also for a scan that a user
 * cancel or BeginStop ended. One save per scan, not per batch. The project close, load, and exit
 * already wait for the worker, so this save never runs next to their own save. An unchanged scan
 * does not save. A failed save ends the task Failed with "The project could not be saved."; the
 * stored changes stay in the database.
 *
 * Hold (#1016): a user import and a delete write the library through the same image pool and
 * sleeve tree as the scan batches, and a batch syncs the whole pool and tree. Each of them holds
 * the scans through Hold for its whole length, the finalize step of an import included. A hold
 * requests the stop of the running scan, and no scan and no mount operation starts while a hold
 * exists. MountScanHold::WhenReady runs the start of the import or the delete when no worker
 * writes the project: at once, or after the running scan completed its batch, or after the
 * running operation. A scan that the hold stopped runs again after the last hold, with its
 * trigger, its waiting callbacks, and its task, which shows "Paused" in the meantime. A cancel of
 * that task drops the scan.
 *
 * After each scan that read the listing, the coordinator registers the directories of the
 * listing with the watcher. A mount whose registration fails is "not watched"; the other
 * triggers still scan it.
 *
 * Stop: BeginStop requests the stop of the running scan and returns at once. The worker reads the
 * request before each fingerprint read and before each new file batch, completes a batch that
 * started its import, and ends. Until then HasStoppedWorker is true and no other scan or
 * operation starts, so one worker at most writes the project. A project close, a project load,
 * and the application exit wait for that worker before they save or replace the project: the
 * close on StoppedWorkerEnded, the load on the result of BeginStop in its loader thread, and the
 * exit in its event wait. The UI thread joins the worker only in Stop, which the destructor and
 * the last step of the exit call.
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
  /// coordinator stops first (BeginStop); the first scan starts when its worker has ended.
  void               Start(std::shared_ptr<MountScanPort> port);

  /**
   * @brief Stop without a wait: stop the timers and the watcher, drop the queued scans and
   *        operations, and request the stop of the running scan. The task of the scan ends
   *        Canceled and the waiting callbacks get their canceled result in this call.
   *
   * The worker of the running scan or operation continues to the end of its batch or its
   * storage change; the stored batches stay (MountScanStatus::kCanceled).
   *
   * @return A future that is ready when that worker has ended; any thread can wait for it. An
   *         invalid future when no worker runs.
   */
  auto               BeginStop() -> std::shared_future<void>;

  /// BeginStop, then a join of the worker on the calling thread. The wait is one fingerprint
  /// read plus the batch in work at most. No wait when no worker runs.
  void               Stop();

  /**
   * @brief Request a scan of @p mount_id.
   * @param on_ended Optional; called with the result of the scan that runs for this request: the
   *        next scan of the mount that starts after the call.
   * @return false, with no call of @p on_ended, when the coordinator is not started or the mount
   *         does not exist.
   */
  auto               RequestScan(mount_id_t mount_id, MountScanTrigger trigger,
                                 MountScanEndedCallback on_ended = {}) -> bool;

  /**
   * @brief Run @p operation on the worker thread after the running scan or operation, before the
   *        next queued scan, and call @p done on the UI thread with its outcome.
   * @return false, with no call of @p done, when the coordinator is not started.
   */
  auto RunMountOperation(std::function<void()> operation, MountOperationDoneCallback done) -> bool;

  /**
   * @brief Hold the scans and the mount operations (#1016; see the class comment).
   *
   * Works also when the coordinator is not started: a stopped worker still counts as a worker.
   * The hold ends with MountScanHold::Release or the destruction of the hold.
   */
  [[nodiscard]] auto Hold() -> MountScanHold;
  /// True while a hold exists.
  [[nodiscard]] auto IsHeld() const -> bool { return !holds_.empty(); }
  /// True while a scan, an operation, or a stopped worker runs: a worker can write the project.
  [[nodiscard]] auto HasWorker() const -> bool {
    return running_.has_value() || running_operation_.has_value() || stopped_worker_.has_value();
  }

  /// The root directories of the mounts of the started coordinator.
  [[nodiscard]] auto MountRoots() const -> std::vector<std::filesystem::path>;

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
  /// True while a scan or a mount operation runs or waits to run, or the worker of a stopped
  /// scan or operation has not ended.
  [[nodiscard]] auto HasPendingScans() const -> bool;
  /// True while the worker that BeginStop stopped has not ended. It can still write its batch.
  [[nodiscard]] auto HasStoppedWorker() const -> bool { return stopped_worker_.has_value(); }

  /**
   * @brief True when the volume that VolumeWithoutMountArrived reported is still at its mount
   *        point (#1087): the mount point exists and its volume has the identity @p identity.
   *
   * The card of the volume calls it before Import and Mount, so a volume that replaced the
   * reported one at the same mount point is not used in its place. An empty @p identity matches
   * a volume that has no identity. It reads the volume on the calling thread, as the watcher does
   * at the arrival.
   */
  [[nodiscard]] auto IsArrivedVolumeAt(const std::filesystem::path& mount_point,
                                       const std::string& identity) const -> bool;

 signals:
  /// The worker that BeginStop stopped has ended; HasStoppedWorker is false.
  void StoppedWorkerEnded();
  /// A volume that holds no mount of the open project arrived (#1076). Emitted only while the
  /// coordinator is started.
  /// @param volumeName The display name of the volume; empty when it has none.
  /// @param mountPoint The mount point (`E:\` on Windows, the mount path on macOS and Linux).
  /// @param hasDcimDirectory True when the root of the volume holds a `DCIM` directory.
  /// @param volumeIdentity The identity of the volume (ReadVolumeIdentity) for
  ///        IsArrivedVolumeAt; empty when it has none.
  void VolumeWithoutMountArrived(const QString& volumeName, const QString& mountPoint,
                                 bool hasDcimDirectory, const QString& volumeIdentity);
  /// A mount point is gone from the mount table (#1087): its volume was ejected or pulled out.
  /// Emitted only while the coordinator is started.
  void MountPointRemoved(const QString& mountPoint);

 private:
  friend class MountScanHold;

  struct MountState {
    std::filesystem::path           root;
    /// MountRootRecord::volume_identity; empty when the volume has none.
    std::string                     volume_identity;
    QString                         title;
    std::optional<MountScanTrigger> queued;
    std::optional<MountScanTrigger> scan_again;
    MountReachability               reachability = MountReachability::kNotScanned;
    bool                            watched      = false;
    /// Callbacks of the requests for the next scan of the mount that starts.
    std::vector<MountScanEndedCallback> on_next_scan_ended;
    /// The task of a scan that a hold stopped; the next scan of the mount takes it over.
    QString                             paused_task_id;
  };

  struct HoldState {
    quint64               id = 0;
    /// The start that waits for the end of the worker (MountScanHold::WhenReady).
    std::function<void()> on_ready;
  };

  struct PendingOperation {
    std::function<void()>      operation;
    MountOperationDoneCallback done;
  };

  struct RunningOperation {
    MountOperationDoneCallback done;
    std::thread                worker;
    /// Ready when the worker thread has run the operation and posted its end.
    std::shared_future<void>   ended;
    /// `generation_` at the start of the operation.
    quint64                    generation = 0;
  };

  /// The worker of the scan or operation that ran at BeginStop. It holds the port of its run.
  struct StoppedWorker {
    std::thread              worker;
    std::shared_future<void> ended;
    /// `generation_` at the start of the worker; the queued end of the worker carries it.
    quint64                  generation = 0;
  };

  struct RunningScan {
    mount_id_t                         mount_id = 0;
    MountScanTrigger                   trigger  = MountScanTrigger::kProjectOpen;
    std::shared_ptr<std::atomic<bool>> stop_requested;
    std::thread                        worker;
    /// Ready when the worker thread has run the scan and posted its end.
    std::shared_future<void>            ended;
    /// `generation_` at the start of the scan.
    quint64                             generation = 0;
    QString                            task_id;
    bool                               canceled_by_user = false;
    /// A hold requested the stop of the scan.
    bool                                stopped_by_hold  = false;
    std::vector<MountScanEndedCallback> on_ended;
  };

  void HandleDirectoryChanged(mount_id_t mount_id);
  void HandleVolumeRemoved(mount_id_t mount_id);
  void HandleVolumeArrived();
  void HandleMountPointArrived(const ArrivedVolume& volume);
  void HandleMountPointRemoved(const std::filesystem::path& mount_point);
  /// Start the next queued operation, else the next queued scan, when the worker is free.
  void StartNextScan();
  void StartOperation(PendingOperation pending);
  void HandleOperationEnded(quint64 generation, std::exception_ptr error);
  /// Join the stopped worker whose queued end carries @p generation, then start the next scan.
  void HandleStoppedWorkerEnded(quint64 generation);
  void HandleScanProgress(quint64 generation, mount_id_t mount_id,
                          const MountScanProgress& progress);
  /// @param save_failed True when the scan stored changes and MountScanPort::SaveProject failed.
  void HandleScanEnded(quint64 generation, mount_id_t mount_id, const MountScanResult& result,
                       bool save_failed);
  void ReportScanEnd(const RunningScan& run, const MountState& state, const MountScanResult& result,
                     MountReachability previous_reachability, bool save_failed);
  void UpdateWatch(mount_id_t mount_id, MountState& state, const MountScanResult& result);
  auto RegisterTask(mount_id_t mount_id, const MountState& state, const QString& detail,
                    int progress_percent) -> QString;
  void CancelByUser(quint64 generation, mount_id_t mount_id);
  /// The cancel of the running scan: requires `running_`.
  void CancelRunningScanByUser();
  /// End the paused task of @p state as Canceled.
  void CancelPausedTask(MountState& state);
  void WhenHoldReady(quint64 hold_id, std::function<void()> on_ready);
  void ReleaseHold(quint64 hold_id);
  /// Run the waiting starts of the holds when no worker runs.
  void RunReadyHolds();

  BackgroundTaskController*                               tasks_ = nullptr;
  std::unique_ptr<MountScanClock>                         clock_;
  std::unique_ptr<MountDirectoryWatcher>                  watcher_;
  std::chrono::milliseconds                               watcher_delay_;
  std::chrono::milliseconds                               activation_interval_;
  std::chrono::milliseconds                               volume_arrival_delay_;
  std::function<void(mount_id_t)>                         on_batch_stored_;
  std::function<void(mount_id_t, const MountScanResult&)> on_scan_ended_;
  std::function<std::string(const std::filesystem::path&)> read_volume_identity_;

  std::shared_ptr<MountScanPort>                          port_;
  std::unordered_map<mount_id_t, MountState>              mounts_;
  std::deque<mount_id_t>                                  queue_;
  std::optional<RunningScan>                              running_;
  std::deque<PendingOperation>                            operations_;
  std::optional<RunningOperation>                         running_operation_;
  /// Set by BeginStop when a worker ran. No scan and no operation starts while it is set, so
  /// `running_`, `running_operation_`, and this member hold one worker thread at most.
  std::optional<StoppedWorker>                            stopped_worker_;
  /// Changes at each Start and BeginStop, so a queued call of an earlier run does nothing.
  quint64                                                 generation_ = 0;
  std::optional<MountScanClock::TimePoint>                inactive_since_;
  /// The holds, oldest first. They stay through Start and BeginStop.
  std::vector<HoldState>                                  holds_;
  quint64                                                 next_hold_id_ = 0;
};

/**
 * @brief A hold of the scans of a MountScanCoordinator (#1016). Move-only; the destruction
 *        releases it. An empty hold (no coordinator) holds nothing.
 *
 * Thread safety: UI thread only, like the coordinator.
 */
class MountScanHold final {
 public:
  MountScanHold() = default;
  MountScanHold(MountScanHold&& other) noexcept;
  auto operator=(MountScanHold&& other) noexcept -> MountScanHold&;
  MountScanHold(const MountScanHold&)                    = delete;
  auto operator=(const MountScanHold&) -> MountScanHold& = delete;
  ~MountScanHold();

  /**
   * @brief Run @p start on the UI thread when no worker of the coordinator writes the project.
   *
   * In this call when no worker runs or the hold is empty, else after the end of the worker.
   * A released hold never runs it. A second call replaces a start that still waits.
   */
  void               WhenReady(std::function<void()> start);
  /// End the hold; a start that still waits does not run. A released hold is empty.
  void               Release();
  /// True while the hold holds a coordinator.
  [[nodiscard]] auto IsHeld() const -> bool { return id_ != 0 && coordinator_ != nullptr; }

 private:
  friend class MountScanCoordinator;
  MountScanHold(MountScanCoordinator* coordinator, quint64 id);

  QPointer<MountScanCoordinator> coordinator_;
  quint64                        id_ = 0;
};

/**
 * @brief True when a user import leaves @p path to the scan of a mount (#1016, decision D10:
 *        one file has one element).
 *
 * The listing of one of @p mount_roots lists @p path (ReadMountDirectoryFile), so a scan of that
 * mount imports the file, or has imported it. A file that the listing skips (below a directory
 * whose name starts with a period, behind a link, or of no import category) stays with the user
 * import. Reads the file system only for a path that is below a root.
 */
[[nodiscard]] auto IsPathOfAMountScan(const std::filesystem::path&              path,
                                      const std::vector<std::filesystem::path>& mount_roots)
    -> bool;

/// The task detail of a scan result: the counts of new, moved, changed, missing, and returned
/// photos, or "No changes".
auto MountScanResultDetail(const MountScanResult& result) -> QString;

/// True when the scan changed a photo of the library or could not import one.
auto MountScanChangedPhotos(const MountScanResult& result) -> bool;

/**
 * @brief True when the scan wrote a library change that the project file must hold (#1015).
 *
 * A new, adopted, moved, changed, returned, or missing photo, a folder added or removed, a
 * relocated root, a new volume name (#1088), or a failed batch (batches before it can be
 * stored). A file that the scan could not import writes no row, and the next scan tries it
 * again, so it alone does not count.
 */
auto MountScanStoredChanges(const MountScanResult& result) -> bool;

}  // namespace alcedo::ui
