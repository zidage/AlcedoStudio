//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/mount_scan_coordinator.hpp"

#include <QMetaObject>
#include <QStringList>
#include <QTimer>
#include <algorithm>
#include <exception>
#include <stdexcept>
#include <system_error>
#include <unordered_set>
#include <utility>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "utils/file/mount_directory_listing.hpp"
#include "utils/file/volume_identity.hpp"

namespace alcedo::ui {
namespace {

/// The trigger of a merged request: a request of the user shows its task from the start.
auto MergeTrigger(const std::optional<MountScanTrigger>& current, MountScanTrigger next)
    -> MountScanTrigger {
  if (current && *current == MountScanTrigger::kSynchronize) {
    return *current;
  }
  return next == MountScanTrigger::kSynchronize || !current ? next : *current;
}

class SteadyMountScanClock final : public MountScanClock {
 public:
  [[nodiscard]] auto Now() const -> TimePoint override { return std::chrono::steady_clock::now(); }

  void               StartTimer(mount_id_t mount_id, std::chrono::milliseconds delay,
                                std::function<void()> callback) override {
    auto& timer = timers_[mount_id];
    if (!timer) {
      timer = std::make_unique<QTimer>();
      timer->setSingleShot(true);
    }
    QObject::disconnect(timer.get(), &QTimer::timeout, nullptr, nullptr);
    QObject::connect(timer.get(), &QTimer::timeout, timer.get(), std::move(callback));
    timer->start(delay);
  }

  void StopTimer(mount_id_t mount_id) override {
    const auto it = timers_.find(mount_id);
    if (it != timers_.end()) {
      it->second->stop();
    }
  }

  void StopAllTimers() override {
    for (auto& [mount_id, timer] : timers_) {
      (void)mount_id;
      timer->stop();
    }
  }

 private:
  std::unordered_map<mount_id_t, std::unique_ptr<QTimer>> timers_;
};

auto CanceledScanResult() -> MountScanResult {
  MountScanResult result;
  result.status = MountScanStatus::kCanceled;
  return result;
}

void CallScanEnded(std::vector<MountScanEndedCallback>& callbacks, const MountScanResult& result) {
  auto pending = std::move(callbacks);
  callbacks.clear();
  for (const auto& callback : pending) {
    if (callback) {
      callback(result);
    }
  }
}

auto ProgressPercent(const MountScanProgress& progress) -> int {
  if (progress.new_files_total == 0) {
    return -1;
  }
  return static_cast<int>(
      std::min<std::size_t>(100, progress.new_files_done * 100 / progress.new_files_total));
}

auto ProgressDetail(const MountScanProgress& progress) -> QString {
  if (progress.new_files_total == 0) {
    return Tr("Reading the folder");
  }
  return Tr("Importing %1 of %2 new photos")
      .arg(static_cast<qulonglong>(progress.new_files_done))
      .arg(static_cast<qulonglong>(progress.new_files_total));
}

}  // namespace

// ── ProjectMountScanPort ────────────────────────────────────────────────────

ProjectMountScanPort::ProjectMountScanPort(
    std::shared_ptr<MountService> mounts, MountFileImporter* importer,
    std::function<void(sl_element_id_t)> invalidate_thumbnail,
    std::function<bool(QString*)>        save_project)
    : mounts_(std::move(mounts)),
      importer_(importer),
      invalidate_thumbnail_(std::move(invalidate_thumbnail)),
      save_project_(std::move(save_project)) {
  if (!mounts_ || importer_ == nullptr || !save_project_) {
    throw std::invalid_argument(
        "ProjectMountScanPort: the mount service, the importer, and the save are required");
  }
}

auto ProjectMountScanPort::SaveProject() -> bool {
  QString error;
  return save_project_(&error);
}

auto ProjectMountScanPort::ListMounts() -> std::vector<MountSummary> {
  return mounts_->ListMounts();
}

auto ProjectMountScanPort::ScanMount(
    mount_id_t mount_id, const std::atomic<bool>& stop_requested,
    const std::function<void(const MountScanProgress&)>& on_progress, bool find_moved_volume)
    -> MountScanResult {
  MountScanRequest request;
  request.mount_id             = mount_id;
  request.find_moved_volume    = find_moved_volume;
  request.importer             = importer_;
  request.stop_requested       = &stop_requested;
  request.invalidate_thumbnail = invalidate_thumbnail_;
  request.on_progress          = on_progress;
  return mounts_->ScanMount(request);
}

auto CreateSteadyMountScanClock() -> std::unique_ptr<MountScanClock> {
  return std::make_unique<SteadyMountScanClock>();
}

// ── Result text ─────────────────────────────────────────────────────────────

auto MountScanChangedPhotos(const MountScanResult& result) -> bool {
  return result.imported_count + result.adopted_count + result.moved_count +
             result.changed_in_place_count + result.missing_count + result.returned_count +
             result.import_failed_count >
         0;
}

auto MountScanStoredChanges(const MountScanResult& result) -> bool {
  return result.imported_count + result.adopted_count + result.moved_count +
                 result.changed_in_place_count + result.missing_count + result.returned_count +
                 result.folders_added_count + result.folders_removed_count >
             0 ||
         result.relocated_root.has_value() || result.volume_name_updated ||
         result.failed_batch > 0;
}

auto MountScanResultDetail(const MountScanResult& result) -> QString {
  QStringList parts;
  const auto  add = [&parts](std::size_t count, const char* text) {
    if (count > 0) {
      parts.push_back(Tr(text).arg(static_cast<qulonglong>(count)));
    }
  };
  // An adopted photo is new in the mounted folder.
  add(result.imported_count + result.adopted_count, "%1 new");
  add(result.moved_count, "%1 moved");
  add(result.changed_in_place_count, "%1 changed");
  add(result.missing_count, "%1 missing");
  add(result.returned_count, "%1 returned");
  add(result.import_failed_count, "%1 not imported");
  if (parts.isEmpty()) {
    return Tr("No changes");
  }
  return parts.join(Tr(", "));
}

auto IsPathOfAMountScan(const std::filesystem::path&              path,
                        const std::vector<std::filesystem::path>& mount_roots) -> bool {
  for (const auto& root : mount_roots) {
    if (!root.empty() && IsPathAtOrBelow(path, root) &&
        ReadMountDirectoryFile(root, path).has_value()) {
      return true;
    }
  }
  return false;
}

// ── MountScanHold ───────────────────────────────────────────────────────────

MountScanHold::MountScanHold(MountScanCoordinator* coordinator, quint64 id)
    : coordinator_(coordinator), id_(id) {}

MountScanHold::MountScanHold(MountScanHold&& other) noexcept
    : coordinator_(std::move(other.coordinator_)), id_(std::exchange(other.id_, 0)) {
  other.coordinator_.clear();
}

auto MountScanHold::operator=(MountScanHold&& other) noexcept -> MountScanHold& {
  if (this != &other) {
    Release();
    coordinator_ = std::move(other.coordinator_);
    id_          = std::exchange(other.id_, 0);
    other.coordinator_.clear();
  }
  return *this;
}

MountScanHold::~MountScanHold() { Release(); }

void MountScanHold::WhenReady(std::function<void()> start) {
  if (!start) {
    return;
  }
  if (!IsHeld()) {
    start();
    return;
  }
  coordinator_->WhenHoldReady(id_, std::move(start));
}

void MountScanHold::Release() {
  const auto id          = std::exchange(id_, 0);
  auto*      coordinator = coordinator_.data();
  coordinator_.clear();
  if (id != 0 && coordinator != nullptr) {
    coordinator->ReleaseHold(id);
  }
}

// ── MountScanCoordinator ────────────────────────────────────────────────────

MountScanCoordinator::MountScanCoordinator(MountScanCoordinatorDependencies dependencies,
                                           QObject*                         parent)
    : QObject(parent),
      tasks_(dependencies.tasks),
      clock_(std::move(dependencies.clock)),
      watcher_(std::move(dependencies.watcher)),
      watcher_delay_(dependencies.watcher_delay),
      activation_interval_(dependencies.activation_interval),
      volume_arrival_delay_(dependencies.volume_arrival_delay),
      on_batch_stored_(std::move(dependencies.on_batch_stored)),
      on_scan_ended_(std::move(dependencies.on_scan_ended)),
      read_volume_identity_(dependencies.read_volume_identity
                                ? std::move(dependencies.read_volume_identity)
                                : std::function<std::string(const std::filesystem::path&)>(
                                      &ReadVolumeIdentity)) {
  if (tasks_ == nullptr || !clock_ || !watcher_) {
    throw std::invalid_argument(
        "MountScanCoordinator: the task list, the clock, and the watcher are required");
  }
  watcher_->SetChangeCallback([this](mount_id_t mount_id) { HandleDirectoryChanged(mount_id); });
  watcher_->SetVolumeCallbacks(
      [this](mount_id_t mount_id) { HandleVolumeRemoved(mount_id); },
      [this] { HandleVolumeArrived(); },
      [this](const ArrivedVolume& volume) { HandleMountPointArrived(volume); },
      [this](const std::filesystem::path& mount_point) { HandleMountPointRemoved(mount_point); });
}

MountScanCoordinator::~MountScanCoordinator() { Stop(); }

void MountScanCoordinator::Start(std::shared_ptr<MountScanPort> port) {
  (void)BeginStop();
  if (!port) {
    return;
  }
  port_ = std::move(port);
  ++generation_;
  ReloadMounts();
  std::vector<mount_id_t> ids;
  ids.reserve(mounts_.size());
  for (const auto& [mount_id, state] : mounts_) {
    (void)state;
    ids.push_back(mount_id);
  }
  std::sort(ids.begin(), ids.end());
  for (const auto mount_id : ids) {
    RequestScan(mount_id, MountScanTrigger::kProjectOpen);
  }
}

void MountScanCoordinator::Stop() {
  (void)BeginStop();
  if (!stopped_worker_) {
    return;
  }
  if (stopped_worker_->worker.joinable()) {
    stopped_worker_->worker.join();
  }
  // The queued end of this worker finds no stopped worker and does nothing.
  stopped_worker_.reset();
  emit StoppedWorkerEnded();
  // The destructor calls Stop: a queued call does not run the waiting starts there.
  QMetaObject::invokeMethod(this, [this] { RunReadyHolds(); }, Qt::QueuedConnection);
}

void MountScanCoordinator::HandleStoppedWorkerEnded(quint64 generation) {
  if (!stopped_worker_ || stopped_worker_->generation != generation) {
    return;
  }
  // The worker thread posted this call as its last step before its end; the join is short.
  if (stopped_worker_->worker.joinable()) {
    stopped_worker_->worker.join();
  }
  stopped_worker_.reset();
  emit StoppedWorkerEnded();
  RunReadyHolds();
  StartNextScan();
}

auto MountScanCoordinator::BeginStop() -> std::shared_future<void> {
  if (!port_) {
    return stopped_worker_ ? stopped_worker_->ended : std::shared_future<void>{};
  }
  ++generation_;
  clock_->StopAllTimers();
  watcher_->UnwatchAll();
  queue_.clear();
  std::vector<MountScanEndedCallback> ended_callbacks;
  if (running_) {
    running_->stop_requested->store(true);
    // The worker completes a batch that started its import and ends; the stored batches stay.
    stopped_worker_ =
        StoppedWorker{std::move(running_->worker), running_->ended, running_->generation};
    if (!running_->task_id.isEmpty()) {
      tasks_->FinishTask(running_->task_id, BackgroundTaskState::Canceled);
    }
    ended_callbacks = std::move(running_->on_ended);
    running_.reset();
  }
  std::vector<MountOperationDoneCallback> dropped_operations;
  if (running_operation_) {
    // An operation is one storage change; its worker completes it.
    stopped_worker_ = StoppedWorker{std::move(running_operation_->worker),
                                    running_operation_->ended, running_operation_->generation};
    dropped_operations.push_back(std::move(running_operation_->done));
    running_operation_.reset();
  }
  for (auto& pending : operations_) {
    dropped_operations.push_back(std::move(pending.done));
  }
  operations_.clear();
  for (auto& [mount_id, state] : mounts_) {
    (void)mount_id;
    CancelPausedTask(state);
    for (auto& callback : state.on_next_scan_ended) {
      ended_callbacks.push_back(std::move(callback));
    }
  }
  mounts_.clear();
  port_.reset();
  inactive_since_.reset();
  // The callbacks run after the stop, so a callback that requests a scan finds no port.
  CallScanEnded(ended_callbacks, CanceledScanResult());
  const auto stopped =
      std::make_exception_ptr(std::runtime_error("The mount operation ended with the project."));
  for (const auto& done : dropped_operations) {
    if (done) {
      done(stopped);
    }
  }
  return stopped_worker_ ? stopped_worker_->ended : std::shared_future<void>{};
}

void MountScanCoordinator::ReloadMounts() {
  if (!port_) {
    return;
  }
  std::vector<MountSummary> summaries;
  try {
    summaries = port_->ListMounts();
  } catch (const std::exception&) {
    // A storage read failed: keep the known mounts; the next request reads them again.
    return;
  }
  std::unordered_set<mount_id_t> listed;
  for (const auto& summary : summaries) {
    listed.insert(summary.record.id);
    auto [it, inserted] = mounts_.try_emplace(summary.record.id);
    auto& state           = it->second;
    state.root            = summary.record.root_path;
    state.volume_identity = summary.record.volume_identity;
    state.title           = i18n::PathToQString(summary.record.root_path.filename());
    if (inserted) {
      state.reachability = summary.reachability;
    }
  }
  for (auto it = mounts_.begin(); it != mounts_.end();) {
    if (listed.contains(it->first)) {
      ++it;
      continue;
    }
    clock_->StopTimer(it->first);
    watcher_->Unwatch(it->first);
    std::erase(queue_, it->first);
    CancelPausedTask(it->second);
    CallScanEnded(it->second.on_next_scan_ended, CanceledScanResult());
    it = mounts_.erase(it);
  }
}

auto MountScanCoordinator::RequestScan(mount_id_t mount_id, MountScanTrigger trigger,
                                       MountScanEndedCallback on_ended) -> bool {
  if (!port_) {
    return false;
  }
  auto it = mounts_.find(mount_id);
  if (it == mounts_.end()) {
    ReloadMounts();
    it = mounts_.find(mount_id);
    if (it == mounts_.end()) {
      return false;
    }
  }
  auto& state = it->second;
  if (on_ended) {
    state.on_next_scan_ended.push_back(std::move(on_ended));
  }
  if (running_ && running_->mount_id == mount_id) {
    state.scan_again = MergeTrigger(state.scan_again, trigger);
    if (trigger == MountScanTrigger::kSynchronize && running_->task_id.isEmpty()) {
      running_->task_id = RegisterTask(mount_id, state, Tr("Reading the folder"), -1);
    }
    return true;
  }
  if (state.queued) {
    state.queued = MergeTrigger(state.queued, trigger);
    return true;
  }
  state.queued = trigger;
  queue_.push_back(mount_id);
  StartNextScan();
  return true;
}

auto MountScanCoordinator::RunMountOperation(std::function<void()>      operation,
                                             MountOperationDoneCallback done) -> bool {
  if (!port_) {
    return false;
  }
  operations_.push_back(PendingOperation{std::move(operation), std::move(done)});
  StartNextScan();
  return true;
}

auto MountScanCoordinator::Hold() -> MountScanHold {
  const quint64 id = ++next_hold_id_;
  holds_.push_back(HoldState{id, {}});
  if (running_ && !running_->stopped_by_hold) {
    // The scan completes a batch that started its import and ends; it runs again after the
    // last hold. A running operation is one storage change, so it completes.
    running_->stopped_by_hold = true;
    running_->stop_requested->store(true);
  }
  return MountScanHold(this, id);
}

auto MountScanCoordinator::MountRoots() const -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> roots;
  roots.reserve(mounts_.size());
  for (const auto& [mount_id, state] : mounts_) {
    (void)mount_id;
    roots.push_back(state.root);
  }
  return roots;
}

void MountScanCoordinator::WhenHoldReady(quint64 hold_id, std::function<void()> on_ready) {
  const auto it = std::find_if(holds_.begin(), holds_.end(),
                               [hold_id](const HoldState& hold) { return hold.id == hold_id; });
  if (it == holds_.end()) {
    return;
  }
  if (!HasWorker()) {
    on_ready();
    return;
  }
  it->on_ready = std::move(on_ready);
}

void MountScanCoordinator::ReleaseHold(quint64 hold_id) {
  std::erase_if(holds_, [hold_id](const HoldState& hold) { return hold.id == hold_id; });
  if (holds_.empty()) {
    StartNextScan();
  }
}

void MountScanCoordinator::RunReadyHolds() {
  if (HasWorker()) {
    return;
  }
  std::vector<quint64> waiting;
  for (const auto& hold : holds_) {
    if (hold.on_ready) {
      waiting.push_back(hold.id);
    }
  }
  // A start can release any hold, so each start is looked up again before it runs.
  for (const auto hold_id : waiting) {
    const auto it = std::find_if(holds_.begin(), holds_.end(),
                                 [hold_id](const HoldState& hold) { return hold.id == hold_id; });
    if (it == holds_.end() || !it->on_ready) {
      continue;
    }
    auto start = std::exchange(it->on_ready, nullptr);
    start();
  }
}

void MountScanCoordinator::StartOperation(PendingOperation pending) {
  running_operation_.emplace();
  running_operation_->done       = std::move(pending.done);
  running_operation_->generation = generation_;
  std::promise<void> ended;
  running_operation_->ended = ended.get_future().share();
  // The coordinator joins the worker before it is destroyed (Stop), so `this` outlives it.
  running_operation_->worker =
      std::thread([this, generation = generation_, operation = std::move(pending.operation),
                   ended = std::move(ended)]() mutable {
        std::exception_ptr error;
        try {
          operation();
        } catch (...) {
          error = std::current_exception();
        }
        QMetaObject::invokeMethod(
            this, [this, generation, error] { HandleOperationEnded(generation, error); },
            Qt::QueuedConnection);
        ended.set_value();
      });
}

void MountScanCoordinator::HandleOperationEnded(quint64 generation, std::exception_ptr error) {
  if (generation != generation_) {
    HandleStoppedWorkerEnded(generation);
    return;
  }
  if (!running_operation_) {
    return;
  }
  RunningOperation run = std::move(*running_operation_);
  running_operation_.reset();
  if (run.worker.joinable()) {
    run.worker.join();
  }
  if (run.done) {
    run.done(error);
  }
  RunReadyHolds();
  StartNextScan();
}

void MountScanCoordinator::HandleApplicationActiveChanged(bool active) {
  if (!active) {
    if (!inactive_since_) {
      inactive_since_ = clock_->Now();
    }
    return;
  }
  if (!inactive_since_) {
    return;
  }
  const auto background_time = clock_->Now() - *inactive_since_;
  inactive_since_.reset();
  if (!port_ || background_time < activation_interval_) {
    return;
  }
  std::vector<mount_id_t> ids;
  for (const auto& [mount_id, state] : mounts_) {
    (void)state;
    ids.push_back(mount_id);
  }
  std::sort(ids.begin(), ids.end());
  for (const auto mount_id : ids) {
    RequestScan(mount_id, MountScanTrigger::kActivation);
  }
}

auto MountScanCoordinator::IsWatched(mount_id_t mount_id) const -> bool {
  const auto it = mounts_.find(mount_id);
  return it != mounts_.end() && it->second.watched;
}

auto MountScanCoordinator::IsScanRunning(mount_id_t mount_id) const -> bool {
  return running_ && running_->mount_id == mount_id;
}

auto MountScanCoordinator::HasPendingScans() const -> bool {
  return running_.has_value() || !queue_.empty() || running_operation_.has_value() ||
         !operations_.empty() || stopped_worker_.has_value();
}

void MountScanCoordinator::HandleDirectoryChanged(mount_id_t mount_id) {
  if (!port_ || !mounts_.contains(mount_id)) {
    return;
  }
  // Each notification restarts the delay; the scan starts after the last one.
  clock_->StartTimer(mount_id, watcher_delay_, [this, mount_id] {
    RequestScan(mount_id, MountScanTrigger::kDirectoryChange);
  });
}

void MountScanCoordinator::HandleVolumeRemoved(mount_id_t mount_id) {
  const auto it = mounts_.find(mount_id);
  if (!port_ || it == mounts_.end()) {
    return;
  }
  // The watcher ended the watch. The scan finds the mount offline, or watches it again when the
  // removal was canceled.
  it->second.watched = false;
  RequestScan(mount_id, MountScanTrigger::kVolumeChange);
}

void MountScanCoordinator::HandleVolumeArrived() {
  if (!port_) {
    return;
  }
  for (const auto& [mount_id, state] : mounts_) {
    if (state.reachability != MountReachability::kOffline) {
      continue;
    }
    // A second arrival in the delay restarts it; one scan follows.
    clock_->StartTimer(mount_id, volume_arrival_delay_, [this, mount_id] {
      RequestScan(mount_id, MountScanTrigger::kVolumeChange);
    });
  }
}

void MountScanCoordinator::HandleMountPointArrived(const ArrivedVolume& volume) {
  if (!port_) {
    return;
  }
  // The mounts of an unreachable volume are offline, so the watcher holds no watch for them; the
  // stored identity and root of each mount decide. Two known identities that differ name two
  // volumes, so a root at or below the mount point belongs to the other volume (#1087).
  for (const auto& [mount_id, state] : mounts_) {
    const bool both_known = !volume.identity.empty() && !state.volume_identity.empty();
    if (both_known ? state.volume_identity == volume.identity
                   : IsPathAtOrBelow(state.root, volume.mount_point)) {
      return;
    }
  }
  emit VolumeWithoutMountArrived(
      QString::fromStdString(volume.name), i18n::PathToQString(volume.mount_point),
      volume.has_dcim_directory, QString::fromStdString(volume.identity));
}

void MountScanCoordinator::HandleMountPointRemoved(const std::filesystem::path& mount_point) {
  if (!port_) {
    return;
  }
  emit MountPointRemoved(i18n::PathToQString(mount_point));
}

auto MountScanCoordinator::IsArrivedVolumeAt(const std::filesystem::path& mount_point,
                                             const std::string& identity) const -> bool {
  std::error_code ec;
  if (!std::filesystem::is_directory(mount_point, ec) || ec) {
    return false;
  }
  return read_volume_identity_(mount_point) == identity;
}

void MountScanCoordinator::StartNextScan() {
  // A stopped worker can still write its batch; HandleStoppedWorkerEnded calls this again. The
  // release of the last hold also calls this again.
  if (!port_ || HasWorker() || !holds_.empty()) {
    return;
  }
  if (!operations_.empty()) {
    auto pending = std::move(operations_.front());
    operations_.pop_front();
    StartOperation(std::move(pending));
    return;
  }
  while (port_ && !running_ && !queue_.empty()) {
    const mount_id_t mount_id = queue_.front();
    queue_.pop_front();
    const auto it = mounts_.find(mount_id);
    if (it == mounts_.end() || !it->second.queued) {
      continue;
    }
    auto& state = it->second;
    running_.emplace();
    running_->mount_id       = mount_id;
    running_->trigger        = *state.queued;
    running_->stop_requested = std::make_shared<std::atomic<bool>>(false);
    running_->on_ended       = std::move(state.on_next_scan_ended);
    running_->generation     = generation_;
    std::promise<void> ended;
    running_->ended = ended.get_future().share();
    state.on_next_scan_ended.clear();
    state.queued.reset();
    if (!state.paused_task_id.isEmpty()) {
      // The scan that a hold stopped runs again in its task.
      running_->task_id = std::exchange(state.paused_task_id, QString{});
      tasks_->UpdateTask(running_->task_id, state.title, Tr("Reading the folder"), -1);
    } else if (running_->trigger == MountScanTrigger::kSynchronize) {
      running_->task_id = RegisterTask(mount_id, state, Tr("Reading the folder"), -1);
    }

    // The coordinator joins the worker before it is destroyed (Stop), so `this` outlives the
    // worker. Qt drops a queued call whose receiver is gone.
    running_->worker = std::thread(
        [this, generation = generation_, mount_id, port = port_, stop = running_->stop_requested,
         find_moved_volume = running_->trigger == MountScanTrigger::kProjectOpen ||
                             running_->trigger == MountScanTrigger::kVolumeChange,
         ended = std::move(ended)]() mutable {
          MountScanResult result;
          try {
            result = port->ScanMount(
                mount_id, *stop,
                [this, generation, mount_id](const MountScanProgress& progress) {
                  QMetaObject::invokeMethod(
                      this,
                      [this, generation, mount_id, progress] {
                        HandleScanProgress(generation, mount_id, progress);
                      },
                      Qt::QueuedConnection);
                },
                find_moved_volume);
          } catch (const std::exception& e) {
            result        = {};
            result.status = MountScanStatus::kStorageFailure;
            result.error  = e.what();
          } catch (...) {
            result        = {};
            result.status = MountScanStatus::kStorageFailure;
            result.error  = "unknown error";
          }
          // #1015: the stored changes reach the project file before the worker ends, so the
          // close, the load, and the exit, which wait for the worker, never save next to it.
          bool save_failed = false;
          if (MountScanStoredChanges(result)) {
            try {
              save_failed = !port->SaveProject();
            } catch (...) {
              save_failed = true;
            }
          }
          QMetaObject::invokeMethod(
              this,
              [this, generation, mount_id, result = std::move(result), save_failed] {
                HandleScanEnded(generation, mount_id, result, save_failed);
              },
              Qt::QueuedConnection);
          ended.set_value();
        });
  }
}

void MountScanCoordinator::HandleScanProgress(quint64 generation, mount_id_t mount_id,
                                              const MountScanProgress& progress) {
  if (generation != generation_ || !running_ || running_->mount_id != mount_id) {
    return;
  }
  const auto it = mounts_.find(mount_id);
  if (it != mounts_.end() && !running_->canceled_by_user) {
    const QString detail  = ProgressDetail(progress);
    const int     percent = ProgressPercent(progress);
    if (running_->task_id.isEmpty()) {
      // An automatic scan shows when it starts to import new files.
      if (progress.new_files_done < progress.new_files_total) {
        running_->task_id = RegisterTask(mount_id, it->second, detail, percent);
      }
    } else {
      tasks_->UpdateTask(running_->task_id, it->second.title, detail, percent);
    }
  }
  if (on_batch_stored_) {
    on_batch_stored_(mount_id);
  }
}

void MountScanCoordinator::HandleScanEnded(quint64 generation, mount_id_t mount_id,
                                           const MountScanResult& result, bool save_failed) {
  if (generation != generation_) {
    HandleStoppedWorkerEnded(generation);
    return;
  }
  if (!running_ || running_->mount_id != mount_id) {
    return;
  }
  RunningScan run = std::move(*running_);
  running_.reset();
  if (run.worker.joinable()) {
    run.worker.join();
  }

  // A scan that a hold stopped before its end runs again after the last hold (#1016).
  const bool resume_after_hold =
      run.stopped_by_hold && !run.canceled_by_user && result.status == MountScanStatus::kCanceled;
  std::optional<MountScanTrigger> again;
  if (const auto it = mounts_.find(mount_id); it != mounts_.end()) {
    auto&      state    = it->second;
    const auto previous = state.reachability;
    if (result.relocated_root.has_value()) {
      state.root = *result.relocated_root;
    }
    state.reachability  = result.status == MountScanStatus::kOffline ? MountReachability::kOffline
                                                                     : MountReachability::kOnline;
    if (resume_after_hold && !save_failed) {
      if (!run.task_id.isEmpty()) {
        state.paused_task_id = run.task_id;
        tasks_->UpdateTask(run.task_id, state.title, Tr("Paused"), -1);
      }
    } else {
      ReportScanEnd(run, state, result, previous, save_failed);
    }
    UpdateWatch(mount_id, state, result);
    if (resume_after_hold) {
      state.scan_again = MergeTrigger(state.scan_again, run.trigger);
      // The requests that waited for this scan get the result of the scan that runs again.
      state.on_next_scan_ended.insert(state.on_next_scan_ended.begin(),
                                      std::make_move_iterator(run.on_ended.begin()),
                                      std::make_move_iterator(run.on_ended.end()));
      run.on_ended.clear();
    }
    if (!run.canceled_by_user) {
      again = state.scan_again;
    }
    state.scan_again.reset();
    if (!again) {
      // A user cancel dropped the scan that these requests waited for.
      CallScanEnded(state.on_next_scan_ended, CanceledScanResult());
    }
  }

  if (on_scan_ended_) {
    on_scan_ended_(mount_id, result);
  }
  CallScanEnded(run.on_ended, result);
  if (again) {
    RequestScan(mount_id, *again);
  }
  RunReadyHolds();
  StartNextScan();
}

void MountScanCoordinator::ReportScanEnd(const RunningScan& run, const MountState& state,
                                         const MountScanResult& result,
                                         MountReachability      previous_reachability,
                                         bool                   save_failed) {
  QString task_id = run.task_id;
  if (save_failed && result.status != MountScanStatus::kStorageFailure) {
    // The changes are in the database; the project file holds the state of the last save.
    if (task_id.isEmpty()) {
      task_id = RegisterTask(run.mount_id, state, QString(), -1);
    }
    tasks_->FinishTask(task_id, BackgroundTaskState::Failed, Tr("The project could not be saved."));
    return;
  }
  switch (result.status) {
    case MountScanStatus::kComplete:
      if (task_id.isEmpty()) {
        if (!MountScanChangedPhotos(result)) {
          return;
        }
        task_id = RegisterTask(run.mount_id, state, QString(), 100);
      }
      tasks_->FinishTask(task_id, BackgroundTaskState::Succeeded, MountScanResultDetail(result));
      return;
    case MountScanStatus::kOffline:
      if (task_id.isEmpty()) {
        if (previous_reachability == MountReachability::kOffline) {
          return;
        }
        task_id = RegisterTask(run.mount_id, state, QString(), -1);
      }
      tasks_->FinishTask(task_id, BackgroundTaskState::Failed, Tr("Offline"));
      return;
    case MountScanStatus::kCanceled:
      if (!task_id.isEmpty()) {
        tasks_->FinishTask(task_id, BackgroundTaskState::Canceled, MountScanResultDetail(result));
      }
      return;
    case MountScanStatus::kStorageFailure:
      if (task_id.isEmpty()) {
        task_id = RegisterTask(run.mount_id, state, QString(), -1);
      }
      tasks_->FinishTask(task_id, BackgroundTaskState::Failed,
                         Tr("The changes could not be stored."));
      return;
  }
}

void MountScanCoordinator::UpdateWatch(mount_id_t mount_id, MountState& state,
                                       const MountScanResult& result) {
  switch (result.status) {
    case MountScanStatus::kOffline:
      watcher_->Unwatch(mount_id);
      state.watched = false;
      return;
    case MountScanStatus::kCanceled:
      // A stopped listing can be partial; keep a watch that exists.
      if (state.watched) {
        return;
      }
      break;
    case MountScanStatus::kComplete:
    case MountScanStatus::kStorageFailure:
      break;
  }
  state.watched = watcher_->Watch(mount_id, state.root, result.listed_directories);
}

auto MountScanCoordinator::RegisterTask(mount_id_t mount_id, const MountState& state,
                                        const QString& detail, int progress_percent) -> QString {
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::MountScan;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = state.title;
  snapshot.detail_           = detail;
  snapshot.progress_percent_ = progress_percent;
  snapshot.cancelable_       = true;
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::CancelAndWait;
  // Stop finishes the task, which clears the cancel callback before the coordinator ends.
  return tasks_->RegisterTask(
      snapshot, [this, generation = generation_, mount_id] { CancelByUser(generation, mount_id); });
}

void MountScanCoordinator::CancelPausedTask(MountState& state) {
  if (!state.paused_task_id.isEmpty()) {
    tasks_->FinishTask(std::exchange(state.paused_task_id, QString{}),
                       BackgroundTaskState::Canceled);
  }
}

void MountScanCoordinator::CancelByUser(quint64 generation, mount_id_t mount_id) {
  if (generation != generation_) {
    return;
  }
  if (!running_ || running_->mount_id != mount_id) {
    // The task of a scan that a hold stopped: the scan does not run again. The end of the task
    // can remove task records, so it runs after the cancel loop of the task list. A scan that
    // took the task over in the meantime gets the cancel of a running scan.
    QMetaObject::invokeMethod(
        this,
        [this, generation, mount_id] {
          if (generation != generation_) {
            return;
          }
          if (running_ && running_->mount_id == mount_id) {
            CancelRunningScanByUser();
            return;
          }
          const auto it = mounts_.find(mount_id);
          if (it == mounts_.end() || it->second.paused_task_id.isEmpty()) {
            return;
          }
          auto& state = it->second;
          CancelPausedTask(state);
          state.queued.reset();
          state.scan_again.reset();
          std::erase(queue_, mount_id);
          CallScanEnded(state.on_next_scan_ended, CanceledScanResult());
        },
        Qt::QueuedConnection);
    return;
  }
  CancelRunningScanByUser();
}

void MountScanCoordinator::CancelRunningScanByUser() {
  running_->canceled_by_user = true;
  running_->stop_requested->store(true);
  if (const auto it = mounts_.find(running_->mount_id); it != mounts_.end()) {
    it->second.scan_again.reset();
  }
  if (!running_->task_id.isEmpty()) {
    tasks_->UpdateTaskState(running_->task_id, BackgroundTaskState::Canceling);
  }
}

}  // namespace alcedo::ui
