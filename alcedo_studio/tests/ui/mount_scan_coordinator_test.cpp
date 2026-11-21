//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Mounted Folder Plan, MF7 (#989): the scan triggers of the mounted folders. The trigger rules
// run with a clock that the test advances, a watcher that the test drives, and a scan port that
// counts the scans. FileAddedToTheDirectoryAppearsInTheLibrary runs the platform watcher on a
// real directory and a real project database. MountDirectoryWatcherTest holds the watcher rules.

#include "ui/alcedo_main/album_backend/mount_scan_coordinator.hpp"

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QVariantMap>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exiv2/exiv2.hpp>
#include <filesystem>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "app/import_service.hpp"
#include "app/pipeline_service.hpp"
#include "app/project_service.hpp"
#include "support/test_temp_directory.hpp"
#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/alcedo_main/album_backend/mount_directory_watcher.hpp"
#include "utils/clock/time_provider.hpp"

namespace alcedo::ui {
namespace {

namespace stdfs = std::filesystem;
using std::chrono::milliseconds;
using std::chrono::seconds;

/// Process events until @p done is true or 20 s pass. Returns @p done().
auto WaitUntil(const std::function<bool()>& done) -> bool {
  QElapsedTimer timer;
  timer.start();
  while (!done()) {
    if (timer.elapsed() > 20000) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QThread::msleep(2);
  }
  return true;
}

/// Process the events that are queued now (posted calls of a finished scan).
void ProcessQueuedEvents() {
  for (int i = 0; i < 5; ++i) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
  }
}

class ManualClock final : public MountScanClock {
 public:
  [[nodiscard]] auto Now() const -> TimePoint override { return now_; }

  void               StartTimer(mount_id_t mount_id, milliseconds delay,
                                std::function<void()> callback) override {
    timers_[mount_id] = Timer{now_ + delay, std::move(callback)};
  }
  void StopTimer(mount_id_t mount_id) override { timers_.erase(mount_id); }
  void StopAllTimers() override { timers_.clear(); }

  /// Move the time forward and run each timer that is due, in the order of its time.
  void Advance(milliseconds step) {
    now_ += step;
    while (true) {
      auto due = timers_.end();
      for (auto it = timers_.begin(); it != timers_.end(); ++it) {
        if (it->second.deadline <= now_ &&
            (due == timers_.end() || it->second.deadline < due->second.deadline)) {
          due = it;
        }
      }
      if (due == timers_.end()) {
        return;
      }
      auto callback = std::move(due->second.callback);
      timers_.erase(due);
      callback();
    }
  }

  [[nodiscard]] auto HasTimer(mount_id_t mount_id) const -> bool {
    return timers_.contains(mount_id);
  }

 private:
  struct Timer {
    TimePoint             deadline;
    std::function<void()> callback;
  };
  TimePoint                   now_ = TimePoint{} + std::chrono::hours(1);
  std::map<mount_id_t, Timer> timers_;
};

class ManualWatcher final : public MountDirectoryWatcher {
 public:
  void SetChangeCallback(ChangeCallback callback) override { on_change_ = std::move(callback); }
  auto Watch(mount_id_t mount_id, const stdfs::path& root, std::span<const std::wstring>)
      -> bool override {
    if (fail_registration_) {
      watched_.erase(mount_id);
      return false;
    }
    watched_.insert(mount_id);
    watched_roots_[mount_id] = root;
    return true;
  }
  void Unwatch(mount_id_t mount_id) override { watched_.erase(mount_id); }
  void UnwatchAll() override { watched_.clear(); }

  void Notify(mount_id_t mount_id) {
    if (watched_.contains(mount_id) && on_change_) {
      on_change_(mount_id);
    }
  }

  bool                              fail_registration_ = false;
  std::set<mount_id_t>              watched_;
  std::map<mount_id_t, stdfs::path> watched_roots_;

 private:
  ChangeCallback on_change_;
};

/// A scan port that counts the scans. A scan can wait until the test lets it continue: a wait
/// that the stop request ends (a scan between two batches), or a wait that only the test ends
/// (a scan in a batch that started its import).
class CountingScanPort final : public MountScanPort {
 public:
  explicit CountingScanPort(std::vector<MountSummary> mounts) : mounts_(std::move(mounts)) {}

  auto ListMounts() -> std::vector<MountSummary> override { return mounts_; }

  auto ScanMount(mount_id_t mount_id, const std::atomic<bool>& stop_requested,
                 const std::function<void(const MountScanProgress&)>& on_progress,
                 bool find_moved_volume) -> MountScanResult override {
    std::unique_lock lock(mutex_);
    ++scan_count_;
    scanned_.push_back(mount_id);
    find_moved_volume_.push_back(find_moved_volume);
    const bool hold_scan = hold_scans_;
    if (hold_scan) {
      scan_is_held_ = true;
      changed_.notify_all();
      // The coordinator stops a scan through the stop request; the scan reports kCanceled.
      // Nothing notifies a change of the stop request, so the wait polls it.
      while (hold_scans_ && (in_batch_ || !stop_requested.load())) {
        changed_.wait_for(lock, milliseconds(5));
      }
      scan_is_held_ = false;
    }
    if (stop_requested.load()) {
      saw_stop_request_ = true;
      MountScanResult result;
      result.status = MountScanStatus::kCanceled;
      return result;
    }
    auto result = next_result_;
    lock.unlock();
    if (result.imported_count > 0 && on_progress) {
      on_progress(MountScanProgress{result.imported_count, 0});
      on_progress(MountScanProgress{result.imported_count, result.imported_count});
    }
    return result;
  }

  void HoldScans() {
    std::lock_guard lock(mutex_);
    hold_scans_ = true;
  }
  /// The next scan waits in a batch: the stop request does not end the wait, ReleaseScans does.
  void HoldInBatch() {
    std::lock_guard lock(mutex_);
    hold_scans_ = true;
    in_batch_   = true;
  }
  void ReleaseScans() {
    {
      std::lock_guard lock(mutex_);
      hold_scans_ = false;
      in_batch_   = false;
    }
    changed_.notify_all();
  }
  auto WaitUntilScanIsHeld() -> bool {
    return WaitUntil([this] {
      std::lock_guard lock(mutex_);
      return scan_is_held_;
    });
  }
  void SetNextResult(MountScanResult result) {
    std::lock_guard lock(mutex_);
    next_result_ = std::move(result);
  }
  auto ScanCount() -> int {
    std::lock_guard lock(mutex_);
    return scan_count_;
  }
  auto SawStopRequest() -> bool {
    std::lock_guard lock(mutex_);
    return saw_stop_request_;
  }
  /// MountScanRequest::find_moved_volume of each scan, in scan order.
  auto FindMovedVolumeRequests() -> std::vector<bool> {
    std::lock_guard lock(mutex_);
    return find_moved_volume_;
  }

 private:
  std::vector<MountSummary> mounts_;
  std::mutex                mutex_;
  std::condition_variable   changed_;
  bool                      hold_scans_       = false;
  bool                      in_batch_         = false;
  bool                      scan_is_held_     = false;
  bool                      saw_stop_request_ = false;
  int                       scan_count_       = 0;
  std::vector<mount_id_t>   scanned_;
  std::vector<bool>         find_moved_volume_;
  MountScanResult           next_result_;
};

auto MountSummaryOf(mount_id_t mount_id, const stdfs::path& root) -> MountSummary {
  MountSummary summary;
  summary.record.id             = mount_id;
  summary.record.root_folder_id = 100 + mount_id;
  summary.record.root_path      = root;
  return summary;
}

/// The mount scan tasks of the task list, oldest first.
auto MountScanTasks(BackgroundTaskController& tasks) -> std::vector<QVariantMap> {
  std::vector<QVariantMap> result;
  for (const auto& value : tasks.Tasks()) {
    const auto task = value.toMap();
    if (task.value(QStringLiteral("kind")).toString() == QStringLiteral("mountScan")) {
      result.push_back(task);
    }
  }
  return result;
}

class MountScanCoordinatorTest : public ::testing::Test {
 protected:
  static constexpr mount_id_t kMount = 7;

  void                        SetUp() override {
    port_ = std::make_shared<CountingScanPort>(
        std::vector<MountSummary>{MountSummaryOf(kMount, temp_dir_ / "Japan")});
    MakeCoordinator(std::make_unique<ManualWatcher>());
  }

  void MakeCoordinator(std::unique_ptr<MountDirectoryWatcher> watcher) {
    coordinator_.reset();
    auto clock = std::make_unique<ManualClock>();
    clock_     = clock.get();
    watcher_   = dynamic_cast<ManualWatcher*>(watcher.get());
    MountScanCoordinatorDependencies dependencies;
    dependencies.tasks         = &tasks_;
    dependencies.clock         = std::move(clock);
    dependencies.watcher       = std::move(watcher);
    dependencies.on_scan_ended = [this](mount_id_t, const MountScanResult&) { ++ended_count_; };
    coordinator_               = std::make_unique<MountScanCoordinator>(std::move(dependencies));
  }

  /// Start the coordinator and wait for the scan of the project open.
  void StartAndWaitForTheOpenScan() {
    coordinator_->Start(port_);
    ASSERT_TRUE(WaitUntil([this] { return !coordinator_->HasPendingScans(); }));
    ASSERT_EQ(port_->ScanCount(), 1);
  }

  auto WaitForScans(int count) -> bool {
    const bool reached =
        WaitUntil([&] { return port_->ScanCount() >= count && !coordinator_->HasPendingScans(); });
    ProcessQueuedEvents();
    return reached;
  }

  test::TestTempDirectory               temp_dir_;
  BackgroundTaskController              tasks_;
  std::shared_ptr<CountingScanPort>     port_;
  ManualClock*                          clock_       = nullptr;
  ManualWatcher*                        watcher_     = nullptr;
  int                                   ended_count_ = 0;
  std::unique_ptr<MountScanCoordinator> coordinator_;
};

TEST_F(MountScanCoordinatorTest, ProjectOpenScansEachMountAndWatchesIt) {
  StartAndWaitForTheOpenScan();
  EXPECT_TRUE(coordinator_->IsWatched(kMount));
  // A scan that changed nothing shows no task.
  EXPECT_TRUE(MountScanTasks(tasks_).empty());
}

TEST_F(MountScanCoordinatorTest, OnlyTheProjectOpenScanSearchesForAMovedVolume) {
  StartAndWaitForTheOpenScan();
  coordinator_->RequestScan(kMount, MountScanTrigger::kSynchronize);
  ASSERT_TRUE(WaitForScans(2));
  EXPECT_EQ(port_->FindMovedVolumeRequests(), (std::vector<bool>{true, false}));
}

TEST_F(MountScanCoordinatorTest, RelocatedRootIsWatchedAtTheNewRoot) {
  MountScanResult relocated;
  relocated.relocated_root = temp_dir_ / "Card" / "Japan";
  port_->SetNextResult(relocated);
  StartAndWaitForTheOpenScan();
  ASSERT_TRUE(coordinator_->IsWatched(kMount));
  EXPECT_EQ(watcher_->watched_roots_.at(kMount), temp_dir_ / "Card" / "Japan");
}

TEST_F(MountScanCoordinatorTest, TenNotificationsInTwoSecondsStartOneScan) {
  StartAndWaitForTheOpenScan();

  for (int i = 0; i < 10; ++i) {
    watcher_->Notify(kMount);
    clock_->Advance(milliseconds(200));
  }
  // 5 s after the last notification, not after the first one.
  clock_->Advance(milliseconds(4799));
  ProcessQueuedEvents();
  EXPECT_EQ(port_->ScanCount(), 1);

  clock_->Advance(milliseconds(1));
  ASSERT_TRUE(WaitForScans(2));
  clock_->Advance(seconds(30));
  ProcessQueuedEvents();
  EXPECT_EQ(port_->ScanCount(), 2);
}

TEST_F(MountScanCoordinatorTest, RequestDuringAScanStartsExactlyOneMoreScan) {
  StartAndWaitForTheOpenScan();
  port_->HoldScans();
  watcher_->Notify(kMount);
  clock_->Advance(seconds(5));
  ASSERT_TRUE(port_->WaitUntilScanIsHeld());
  ASSERT_TRUE(coordinator_->IsScanRunning(kMount));

  // Requests of every trigger during the scan set one flag.
  coordinator_->RequestScan(kMount, MountScanTrigger::kDirectoryChange);
  coordinator_->RequestScan(kMount, MountScanTrigger::kActivation);
  watcher_->Notify(kMount);
  clock_->Advance(seconds(5));
  coordinator_->RequestScan(kMount, MountScanTrigger::kDirectoryChange);

  port_->ReleaseScans();
  ASSERT_TRUE(WaitForScans(3));
  EXPECT_EQ(port_->ScanCount(), 3);
  EXPECT_EQ(ended_count_, 3);
}

TEST_F(MountScanCoordinatorTest, WatcherFailureKeepsSynchronizeWorking) {
  auto watcher                = std::make_unique<ManualWatcher>();
  watcher->fail_registration_ = true;
  MakeCoordinator(std::move(watcher));
  StartAndWaitForTheOpenScan();
  EXPECT_FALSE(coordinator_->IsWatched(kMount));

  coordinator_->RequestScan(kMount, MountScanTrigger::kSynchronize);
  ASSERT_TRUE(WaitForScans(2));
  const auto tasks = MountScanTasks(tasks_);
  ASSERT_EQ(tasks.size(), 1U);
  EXPECT_EQ(tasks[0].value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
  EXPECT_EQ(tasks[0].value(QStringLiteral("title")).toString(), QStringLiteral("Japan"));
  EXPECT_EQ(tasks[0].value(QStringLiteral("detail")).toString(), QStringLiteral("No changes"));

  // The activation trigger also still scans the mount.
  coordinator_->HandleApplicationActiveChanged(false);
  clock_->Advance(seconds(60));
  coordinator_->HandleApplicationActiveChanged(true);
  ASSERT_TRUE(WaitForScans(3));
}

TEST_F(MountScanCoordinatorTest, ActivationAfterSixtySecondsInTheBackgroundStartsAScan) {
  StartAndWaitForTheOpenScan();

  coordinator_->HandleApplicationActiveChanged(false);
  clock_->Advance(seconds(59));
  coordinator_->HandleApplicationActiveChanged(true);
  ProcessQueuedEvents();
  EXPECT_EQ(port_->ScanCount(), 1);

  coordinator_->HandleApplicationActiveChanged(false);
  clock_->Advance(seconds(30));
  // Another inactive state (hidden) keeps the time of the first one.
  coordinator_->HandleApplicationActiveChanged(false);
  clock_->Advance(seconds(30));
  coordinator_->HandleApplicationActiveChanged(true);
  ASSERT_TRUE(WaitForScans(2));
}

TEST_F(MountScanCoordinatorTest, ProjectCloseEndsTheRunningScan) {
  StartAndWaitForTheOpenScan();
  port_->HoldScans();
  coordinator_->RequestScan(kMount, MountScanTrigger::kSynchronize);
  ASSERT_TRUE(port_->WaitUntilScanIsHeld());

  coordinator_->Stop();
  EXPECT_TRUE(port_->SawStopRequest());
  EXPECT_FALSE(coordinator_->IsStarted());
  EXPECT_FALSE(coordinator_->HasPendingScans());
  EXPECT_TRUE(watcher_->watched_.empty());
  const auto tasks = MountScanTasks(tasks_);
  ASSERT_EQ(tasks.size(), 1U);
  EXPECT_EQ(tasks[0].value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));

  // A notification or a request after the stop starts nothing.
  coordinator_->RequestScan(kMount, MountScanTrigger::kSynchronize);
  ProcessQueuedEvents();
  EXPECT_EQ(port_->ScanCount(), 2);
}

TEST_F(MountScanCoordinatorTest, BeginStopReturnsWhileTheWorkerCompletesItsBatch) {
  // #1014: the stop of the scans joined the worker on the UI thread. The scan here is in a
  // batch that the stop request does not end.
  StartAndWaitForTheOpenScan();
  port_->HoldInBatch();
  coordinator_->RequestScan(kMount, MountScanTrigger::kSynchronize);
  ASSERT_TRUE(port_->WaitUntilScanIsHeld());
  int ended_signals = 0;
  QObject::connect(coordinator_.get(), &MountScanCoordinator::StoppedWorkerEnded,
                   coordinator_.get(), [&ended_signals] { ++ended_signals; });

  const auto worker_ended = coordinator_->BeginStop();

  EXPECT_FALSE(coordinator_->IsStarted());
  EXPECT_TRUE(coordinator_->HasStoppedWorker());
  EXPECT_TRUE(coordinator_->HasPendingScans());
  ASSERT_TRUE(worker_ended.valid());
  EXPECT_EQ(worker_ended.wait_for(milliseconds(0)), std::future_status::timeout);
  const auto tasks = MountScanTasks(tasks_);
  ASSERT_EQ(tasks.size(), 1U);
  EXPECT_EQ(tasks[0].value(QStringLiteral("state")).toString(), QStringLiteral("canceled"));
  ProcessQueuedEvents();
  EXPECT_EQ(ended_signals, 0);
  // A second request gives the same worker.
  EXPECT_TRUE(coordinator_->BeginStop().valid());

  port_->ReleaseScans();
  ASSERT_TRUE(WaitUntil([this] { return !coordinator_->HasStoppedWorker(); }));
  EXPECT_EQ(ended_signals, 1);
  EXPECT_EQ(worker_ended.wait_for(milliseconds(0)), std::future_status::ready);
  EXPECT_TRUE(port_->SawStopRequest());
  EXPECT_FALSE(coordinator_->HasPendingScans());
  EXPECT_FALSE(coordinator_->BeginStop().valid());
  EXPECT_EQ(ended_count_, 1) << "a stopped scan reports no end to the scan observer";
}

TEST_F(MountScanCoordinatorTest, StartAfterBeginStopWaitsForTheStoppedWorker) {
  // One worker at most writes the project: the scans of a new start wait for the worker of the
  // stopped scan.
  StartAndWaitForTheOpenScan();
  port_->HoldInBatch();
  coordinator_->RequestScan(kMount, MountScanTrigger::kSynchronize);
  ASSERT_TRUE(port_->WaitUntilScanIsHeld());
  (void)coordinator_->BeginStop();

  coordinator_->Start(port_);
  ProcessQueuedEvents();
  EXPECT_TRUE(coordinator_->IsStarted());
  EXPECT_TRUE(coordinator_->HasStoppedWorker());
  EXPECT_EQ(port_->ScanCount(), 2);

  port_->ReleaseScans();
  ASSERT_TRUE(WaitForScans(3));
  EXPECT_FALSE(coordinator_->HasStoppedWorker());
  EXPECT_EQ(port_->ScanCount(), 3);
  EXPECT_TRUE(coordinator_->IsWatched(kMount));
}

TEST_F(MountScanCoordinatorTest, BeginStopLetsTheRunningOperationComplete) {
  StartAndWaitForTheOpenScan();
  std::promise<void> release;
  std::atomic<bool>  operation_started{false};
  std::atomic<bool>  operation_completed{false};
  int                done_count = 0;
  bool               done_error = false;
  ASSERT_TRUE(coordinator_->RunMountOperation(
      [&, released = release.get_future().share()] {
        operation_started.store(true);
        released.wait();
        operation_completed.store(true);
      },
      [&](std::exception_ptr error) {
        ++done_count;
        done_error = error != nullptr;
      }));
  ASSERT_TRUE(WaitUntil([&] { return operation_started.load(); }));

  const auto worker_ended = coordinator_->BeginStop();

  // The callback gets the stop; the storage change of the operation still completes.
  EXPECT_EQ(done_count, 1);
  EXPECT_TRUE(done_error);
  EXPECT_TRUE(coordinator_->HasStoppedWorker());
  ASSERT_TRUE(worker_ended.valid());
  EXPECT_FALSE(operation_completed.load());

  release.set_value();
  ASSERT_TRUE(WaitUntil([this] { return !coordinator_->HasStoppedWorker(); }));
  EXPECT_TRUE(operation_completed.load());
  EXPECT_EQ(done_count, 1);
}

TEST_F(MountScanCoordinatorTest, AutomaticScanShowsATaskWhenItImportsNewFiles) {
  StartAndWaitForTheOpenScan();
  MountScanResult result;
  result.imported_count = 3;
  result.moved_count    = 1;
  port_->SetNextResult(result);

  watcher_->Notify(kMount);
  clock_->Advance(seconds(5));
  ASSERT_TRUE(WaitForScans(2));

  const auto tasks = MountScanTasks(tasks_);
  ASSERT_EQ(tasks.size(), 1U);
  EXPECT_EQ(tasks[0].value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
  EXPECT_EQ(tasks[0].value(QStringLiteral("detail")).toString(), QStringLiteral("3 new, 1 moved"));
}

TEST_F(MountScanCoordinatorTest, OfflineShowsOnceUntilTheMountIsOnlineAgain) {
  MountScanResult offline;
  offline.status = MountScanStatus::kOffline;
  port_->SetNextResult(offline);
  StartAndWaitForTheOpenScan();
  EXPECT_FALSE(coordinator_->IsWatched(kMount));
  ASSERT_EQ(MountScanTasks(tasks_).size(), 1U);
  EXPECT_EQ(MountScanTasks(tasks_)[0].value(QStringLiteral("detail")).toString(),
            QStringLiteral("Offline"));

  coordinator_->RequestScan(kMount, MountScanTrigger::kActivation);
  ASSERT_TRUE(WaitForScans(2));
  EXPECT_EQ(MountScanTasks(tasks_).size(), 1U);

  port_->SetNextResult({});
  coordinator_->RequestScan(kMount, MountScanTrigger::kActivation);
  ASSERT_TRUE(WaitForScans(3));
  EXPECT_TRUE(coordinator_->IsWatched(kMount));
  port_->SetNextResult(offline);
  coordinator_->RequestScan(kMount, MountScanTrigger::kActivation);
  ASSERT_TRUE(WaitForScans(4));
  EXPECT_EQ(MountScanTasks(tasks_).size(), 2U);
}

TEST_F(MountScanCoordinatorTest, UserCancelEndsTheScanWithoutAnotherScan) {
  StartAndWaitForTheOpenScan();
  port_->HoldScans();
  coordinator_->RequestScan(kMount, MountScanTrigger::kSynchronize);
  ASSERT_TRUE(port_->WaitUntilScanIsHeld());
  coordinator_->RequestScan(kMount, MountScanTrigger::kDirectoryChange);

  const auto task_id = MountScanTasks(tasks_).at(0).value(QStringLiteral("id")).toString();
  ASSERT_TRUE(tasks_.CancelTask(task_id));
  ASSERT_TRUE(WaitForScans(2));
  EXPECT_EQ(port_->ScanCount(), 2);
  EXPECT_EQ(MountScanTasks(tasks_).at(0).value(QStringLiteral("state")).toString(),
            QStringLiteral("canceled"));
}

// ── Real directory, real project ────────────────────────────────────────────

/// Encode a 24x16 JPEG whose pixels depend on @p seed.
void WriteJpeg(const stdfs::path& path, int seed) {
  stdfs::create_directories(path.parent_path());
  constexpr int             kWidth = 24, kHeight = 16, kChannels = 3;
  std::vector<std::uint8_t> pixels(kWidth * kHeight * kChannels);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    pixels[i] = static_cast<std::uint8_t>(i * 7 + static_cast<std::size_t>(seed) * 31);
  }
  // Encode next to the directory, so the directory sees one new file and no partial write.
  const auto encode_path =
      path.parent_path().parent_path() / (path.stem().string() + "_encode.jpg");
  auto output = OIIO::ImageOutput::create(encode_path.string());
  ASSERT_TRUE(output);
  const OIIO::ImageSpec spec(kWidth, kHeight, kChannels, OIIO::TypeDesc::UINT8);
  ASSERT_TRUE(output->open(encode_path.string(), spec));
  ASSERT_TRUE(output->write_image(OIIO::TypeDesc::UINT8, pixels.data()));
  ASSERT_TRUE(output->close());
  output.reset();
  stdfs::rename(encode_path, path);
}

class MountScanCoordinatorProjectTest : public ::testing::Test {
 protected:
  void SetUp() override {
    TimeProvider::Refresh();
    Exiv2::LogMsg::setLevel(Exiv2::LogMsg::Level::mute);
    stdfs::create_directories(temp_dir_ / "disk" / "Japan" / "Day 1");
    disk_ = stdfs::canonical(temp_dir_ / "disk");
    project_ =
        std::make_unique<ProjectService>(temp_dir_ / "project.db", temp_dir_ / "project.json");
    pipeline_ = std::make_shared<PipelineMgmtService>(project_->GetStorage());
    import_   = std::make_unique<ImportServiceImpl>(project_->GetSleeveService(),
                                                    project_->GetImagePoolService(), pipeline_);
  }

  void TearDown() override {
    coordinator_.reset();
    import_.reset();
    project_->GetSleeveService()->Sync();
    project_->GetImagePoolService()->SyncWithStorage();
    pipeline_.reset();
    project_.reset();
  }

  auto SummaryOf(mount_id_t mount_id) -> MountSummary {
    for (const auto& summary : project_->GetMountService()->ListMounts()) {
      if (summary.record.id == mount_id) {
        return summary;
      }
    }
    ADD_FAILURE() << "no mount with the id";
    return {};
  }

  test::TestTempDirectory               temp_dir_;
  stdfs::path                           disk_;
  std::unique_ptr<ProjectService>       project_;
  std::shared_ptr<PipelineMgmtService>  pipeline_;
  std::unique_ptr<ImportServiceImpl>    import_;
  BackgroundTaskController              tasks_;
  std::unique_ptr<MountScanCoordinator> coordinator_;
};

TEST_F(MountScanCoordinatorProjectTest, FileAddedToTheDirectoryAppearsInTheLibrary) {
  WriteJpeg(disk_ / "Japan" / "Day 1" / "a.jpg", 1);
  const auto mount_id  = project_->GetMountService()->CreateMount(0, disk_ / "Japan");

  auto       clock     = std::make_unique<ManualClock>();
  auto*      clock_ptr = clock.get();
  MountScanCoordinatorDependencies dependencies;
  dependencies.tasks   = &tasks_;
  dependencies.clock   = std::move(clock);
  dependencies.watcher = CreatePlatformMountDirectoryWatcher();
  coordinator_         = std::make_unique<MountScanCoordinator>(std::move(dependencies));
  coordinator_->Start(
      std::make_shared<ProjectMountScanPort>(project_->GetMountService(), import_.get(), nullptr));
  ASSERT_TRUE(WaitUntil([&] { return !coordinator_->HasPendingScans(); }));
  ProcessQueuedEvents();
  ASSERT_EQ(SummaryOf(mount_id).present_count, 1U);
  ASSERT_TRUE(coordinator_->IsWatched(mount_id));
  // The scan reads the files and writes nothing to the directory, so it starts no scan.
  ProcessQueuedEvents();
  EXPECT_FALSE(clock_ptr->HasTimer(mount_id));

  // A new file in a new directory below the root, with no user action in the application.
  WriteJpeg(disk_ / "Japan" / "Day 2" / "b.jpg", 2);
  ASSERT_TRUE(WaitUntil([&] { return clock_ptr->HasTimer(mount_id); }))
      << "the watcher reported no change";
  clock_ptr->Advance(kMountScanWatcherDelay);
  ASSERT_TRUE(WaitUntil([&] { return !coordinator_->HasPendingScans(); }));
  ProcessQueuedEvents();

  EXPECT_EQ(SummaryOf(mount_id).present_count, 2U);
  // One task for the import of the open scan, one for the scan that the watcher started.
  const auto tasks = MountScanTasks(tasks_);
  ASSERT_EQ(tasks.size(), 2U);
  EXPECT_EQ(tasks.back().value(QStringLiteral("state")).toString(), QStringLiteral("succeeded"));
  EXPECT_EQ(tasks.back().value(QStringLiteral("detail")).toString(), QStringLiteral("1 new"));
}

}  // namespace
}  // namespace alcedo::ui
