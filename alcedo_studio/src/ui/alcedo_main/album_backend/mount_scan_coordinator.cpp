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
#include <unordered_set>
#include <utility>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/i18n.hpp"

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
    std::function<void(sl_element_id_t)> invalidate_thumbnail)
    : mounts_(std::move(mounts)),
      importer_(importer),
      invalidate_thumbnail_(std::move(invalidate_thumbnail)) {
  if (!mounts_ || importer_ == nullptr) {
    throw std::invalid_argument(
        "ProjectMountScanPort: the mount service and the importer are required");
  }
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

// ── MountScanCoordinator ────────────────────────────────────────────────────

MountScanCoordinator::MountScanCoordinator(MountScanCoordinatorDependencies dependencies,
                                           QObject*                         parent)
    : QObject(parent),
      tasks_(dependencies.tasks),
      clock_(std::move(dependencies.clock)),
      watcher_(std::move(dependencies.watcher)),
      watcher_delay_(dependencies.watcher_delay),
      activation_interval_(dependencies.activation_interval),
      on_batch_stored_(std::move(dependencies.on_batch_stored)),
      on_scan_ended_(std::move(dependencies.on_scan_ended)) {
  if (tasks_ == nullptr || !clock_ || !watcher_) {
    throw std::invalid_argument(
        "MountScanCoordinator: the task list, the clock, and the watcher are required");
  }
  watcher_->SetChangeCallback([this](mount_id_t mount_id) { HandleDirectoryChanged(mount_id); });
}

MountScanCoordinator::~MountScanCoordinator() { Stop(); }

void MountScanCoordinator::Start(std::shared_ptr<MountScanPort> port) {
  Stop();
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
  if (!port_) {
    return;
  }
  ++generation_;
  clock_->StopAllTimers();
  watcher_->UnwatchAll();
  queue_.clear();
  if (running_) {
    running_->stop_requested->store(true);
    // The scan completes its current batch; the stored batches stay.
    if (running_->worker.joinable()) {
      running_->worker.join();
    }
    if (!running_->task_id.isEmpty()) {
      tasks_->FinishTask(running_->task_id, BackgroundTaskState::Canceled);
    }
    running_.reset();
  }
  mounts_.clear();
  port_.reset();
  inactive_since_.reset();
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
    auto& state         = it->second;
    state.root          = summary.record.root_path;
    state.title         = i18n::PathToQString(summary.record.root_path.filename());
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
    it = mounts_.erase(it);
  }
}

void MountScanCoordinator::RequestScan(mount_id_t mount_id, MountScanTrigger trigger) {
  if (!port_) {
    return;
  }
  auto it = mounts_.find(mount_id);
  if (it == mounts_.end()) {
    ReloadMounts();
    it = mounts_.find(mount_id);
    if (it == mounts_.end()) {
      return;
    }
  }
  auto& state = it->second;
  if (running_ && running_->mount_id == mount_id) {
    state.scan_again = MergeTrigger(state.scan_again, trigger);
    if (trigger == MountScanTrigger::kSynchronize && running_->task_id.isEmpty()) {
      running_->task_id = RegisterTask(state, Tr("Reading the folder"), -1);
    }
    return;
  }
  if (state.queued) {
    state.queued = MergeTrigger(state.queued, trigger);
    return;
  }
  state.queued = trigger;
  queue_.push_back(mount_id);
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
  return running_.has_value() || !queue_.empty();
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

void MountScanCoordinator::StartNextScan() {
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
    state.queued.reset();
    if (running_->trigger == MountScanTrigger::kSynchronize) {
      running_->task_id = RegisterTask(state, Tr("Reading the folder"), -1);
    }

    // The coordinator joins the worker before it is destroyed (Stop), so `this` outlives the
    // worker. Qt drops a queued call whose receiver is gone.
    running_->worker = std::thread(
        [this, generation = generation_, mount_id, port = port_, stop = running_->stop_requested,
         find_moved_volume = running_->trigger == MountScanTrigger::kProjectOpen] {
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
          QMetaObject::invokeMethod(
              this,
              [this, generation, mount_id, result = std::move(result)] {
                HandleScanEnded(generation, mount_id, result);
              },
              Qt::QueuedConnection);
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
        running_->task_id = RegisterTask(it->second, detail, percent);
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
                                           const MountScanResult& result) {
  if (generation != generation_ || !running_ || running_->mount_id != mount_id) {
    return;
  }
  RunningScan run = std::move(*running_);
  running_.reset();
  if (run.worker.joinable()) {
    run.worker.join();
  }

  std::optional<MountScanTrigger> again;
  if (const auto it = mounts_.find(mount_id); it != mounts_.end()) {
    auto&      state    = it->second;
    const auto previous = state.reachability;
    if (result.relocated_root.has_value()) {
      state.root = *result.relocated_root;
    }
    state.reachability  = result.status == MountScanStatus::kOffline ? MountReachability::kOffline
                                                                     : MountReachability::kOnline;
    ReportScanEnd(run, state, result, previous);
    UpdateWatch(mount_id, state, result);
    if (!run.canceled_by_user) {
      again = state.scan_again;
    }
    state.scan_again.reset();
  }

  if (on_scan_ended_) {
    on_scan_ended_(mount_id, result);
  }
  if (again) {
    RequestScan(mount_id, *again);
  }
  StartNextScan();
}

void MountScanCoordinator::ReportScanEnd(const RunningScan& run, const MountState& state,
                                         const MountScanResult& result,
                                         MountReachability      previous_reachability) {
  QString task_id = run.task_id;
  switch (result.status) {
    case MountScanStatus::kComplete:
      if (task_id.isEmpty()) {
        if (!MountScanChangedPhotos(result)) {
          return;
        }
        task_id = RegisterTask(state, QString(), 100);
      }
      tasks_->FinishTask(task_id, BackgroundTaskState::Succeeded, MountScanResultDetail(result));
      return;
    case MountScanStatus::kOffline:
      if (task_id.isEmpty()) {
        if (previous_reachability == MountReachability::kOffline) {
          return;
        }
        task_id = RegisterTask(state, QString(), -1);
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
        task_id = RegisterTask(state, QString(), -1);
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

auto MountScanCoordinator::RegisterTask(const MountState& state, const QString& detail,
                                        int progress_percent) -> QString {
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_             = BackgroundTaskKind::MountScan;
  snapshot.state_            = BackgroundTaskState::Running;
  snapshot.title_            = state.title;
  snapshot.detail_           = detail;
  snapshot.progress_percent_ = progress_percent;
  snapshot.cancelable_       = true;
  snapshot.shutdown_policy_  = BackgroundTaskShutdownPolicy::CancelAndWait;
  // Stop finishes the task, which clears the cancel callback before the coordinator ends.
  return tasks_->RegisterTask(snapshot,
                              [this, generation = generation_] { CancelByUser(generation); });
}

void MountScanCoordinator::CancelByUser(quint64 generation) {
  if (generation != generation_ || !running_) {
    return;
  }
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
