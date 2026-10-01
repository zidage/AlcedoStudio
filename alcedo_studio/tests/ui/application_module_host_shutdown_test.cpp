//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file application_module_host_shutdown_test.cpp
/// @brief Verifies host shutdown waits for every registered task, drains
/// analysis writes released by an export barrier, repacks the project
/// package only when the user entered the project, and removes the runtime
/// workspace after the host closes the project database.

#include "ui/album_backend_test_fixture.hpp"

#include <QMutex>
#include <QMutexLocker>
#include <QStringList>
#include <QTimer>
#include <QtLogging>

#include <array>
#include <filesystem>
#include <memory>

#include "ui/alcedo_main/album_backend/background_task_controller.hpp"
#include "ui/welcome_project_test_support.hpp"

namespace alcedo::ui::test {
namespace {

using ApplicationModuleHostShutdownTests = ApplicationModuleHostTestFixture;

// Collects the qWarning messages of all threads while it is alive.
class ScopedWarningCapture {
 public:
  ScopedWarningCapture() {
    {
      QMutexLocker lock(&Mutex());
      Messages().clear();
    }
    previous_ = qInstallMessageHandler(&ScopedWarningCapture::Capture);
  }
  ~ScopedWarningCapture() { qInstallMessageHandler(previous_); }
  ScopedWarningCapture(const ScopedWarningCapture&)                    = delete;
  auto operator=(const ScopedWarningCapture&) -> ScopedWarningCapture& = delete;

  [[nodiscard]] auto CountContaining(const QString& text) const -> int {
    QMutexLocker lock(&Mutex());
    int          count = 0;
    for (const auto& message : Messages()) {
      if (message.contains(text)) {
        ++count;
      }
    }
    return count;
  }

 private:
  static auto Mutex() -> QMutex& {
    static QMutex mutex;
    return mutex;
  }
  static auto Messages() -> QStringList& {
    static QStringList messages;
    return messages;
  }
  static void Capture(QtMsgType type, const QMessageLogContext&, const QString& message) {
    if (type == QtWarningMsg) {
      QMutexLocker lock(&Mutex());
      Messages().push_back(message);
    }
  }

  QtMessageHandler previous_ = nullptr;
};

constexpr auto kWorkspaceRemovalWarning = "Runtime workspace removal failed";

auto RunningTask(BackgroundTaskKind kind, bool cancelable,
                 BackgroundTaskShutdownPolicy policy) -> BackgroundTaskSnapshot {
  BackgroundTaskSnapshot snapshot;
  snapshot.kind_            = kind;
  snapshot.state_           = BackgroundTaskState::Running;
  snapshot.title_           = QStringLiteral("shutdown test task");
  snapshot.cancelable_      = cancelable;
  snapshot.shutdown_policy_ = policy;
  return snapshot;
}

TEST_F(ApplicationModuleHostShutdownTests,
       ShutdownCancelsAndWaitsForImageAnalysisSemanticAndModelTasks) {
  ApplicationModuleHost host;
  auto*                 tasks = host.background_tasks();
  ASSERT_NE(tasks, nullptr);

  const std::array kinds = {BackgroundTaskKind::ImageAnalysis,
                            BackgroundTaskKind::SemanticGeneration,
                            BackgroundTaskKind::ModelDownload};
  for (const auto kind : kinds) {
    const auto id = std::make_shared<QString>();
    *id = tasks->RegisterTask(
        RunningTask(kind, true, BackgroundTaskShutdownPolicy::CancelAndWait),
        [tasks, id] { tasks->FinishTask(*id, BackgroundTaskState::Canceled); });
    ASSERT_FALSE(id->isEmpty());
  }

  EXPECT_EQ(tasks->RunningCount(), 3);
  host.Shutdown();
  EXPECT_EQ(tasks->RunningCount(), 0);
  host.Shutdown();
  EXPECT_EQ(tasks->RunningCount(), 0);
}

TEST_F(ApplicationModuleHostShutdownTests,
       ShutdownWaitsForExportBarrierAndDrainsAnalysisCompletion) {
  ApplicationModuleHost host;
  auto*                 tasks = host.background_tasks();
  auto*                 sink  = host.image_analysis_sink();
  ASSERT_NE(tasks, nullptr);
  ASSERT_NE(sink, nullptr);

  host.db_write_barrier().Acquire();
  ImageAnalysisItemResult result;
  result.item.element_id = 1;
  ASSERT_TRUE(sink->PersistUnderstanding(result));
  ASSERT_TRUE(sink->HasPendingWrites());

  const QString export_id = tasks->RegisterTask(
      RunningTask(BackgroundTaskKind::Export, false,
                  BackgroundTaskShutdownPolicy::WaitForFinish));
  ASSERT_FALSE(export_id.isEmpty());

  QTimer::singleShot(0, [&host, tasks, export_id] {
    host.db_write_barrier().Release();
    tasks->FinishTask(export_id, BackgroundTaskState::Succeeded);
  });

  host.Shutdown();
  EXPECT_EQ(tasks->RunningCount(), 0);
  EXPECT_FALSE(host.db_write_barrier().IsHeld());
  EXPECT_FALSE(sink->HasPendingWrites());
}

TEST_F(ApplicationModuleHostShutdownTests, ShutdownWithUnenteredProjectSkipsRepack) {
  ScopedRecentProjectSettings settings(temp_dir_);
  const auto                  package = BuildPackedProject(temp_dir_, "unentered_project");

  ApplicationModuleHost host;
  ASSERT_TRUE(host.project()->PreviewProject(PathToQString(package)));
  ASSERT_TRUE(WaitForProjectLoadIdle(host));
  ASSERT_TRUE(host.project()->ServiceReady());
  ASSERT_FALSE(host.project()->ProjectEntered());
  const auto package_before = CapturePackageFileState(package);

  host.Shutdown();

  // The workspace removal runs in the host destructor; see
  // ShutdownRemovesUnenteredProjectWorkspace.
  EXPECT_EQ(ReadFileBytes(package), package_before.bytes_);
  EXPECT_EQ(std::filesystem::last_write_time(package), package_before.write_time_);
}

TEST_F(ApplicationModuleHostShutdownTests, ShutdownWithEnteredProjectRepacks) {
  ScopedRecentProjectSettings settings(temp_dir_);
  const auto                  package = BuildPackedProject(temp_dir_, "entered_project");

  ApplicationModuleHost host;
  ASSERT_TRUE(host.project()->PreviewProject(PathToQString(package)));
  ASSERT_TRUE(WaitForProjectLoadIdle(host));
  ASSERT_TRUE(host.project()->EnterLoadedProject());
  const auto package_before = CapturePackageFileState(package);

  host.Shutdown();

  EXPECT_NE(std::filesystem::last_write_time(package), package_before.write_time_);
}

TEST_F(ApplicationModuleHostShutdownTests, ShutdownRemovesUnenteredProjectWorkspace) {
  ScopedRecentProjectSettings settings(temp_dir_);
  const auto                  package = BuildPackedProject(temp_dir_, "unentered_workspace");
  ScopedWarningCapture        warnings;
  std::filesystem::path       workspace;
  PackageFileState            package_before;
  {
    ApplicationModuleHost host;
    ASSERT_TRUE(host.project()->PreviewProject(PathToQString(package)));
    ASSERT_TRUE(WaitForProjectLoadIdle(host));
    ASSERT_FALSE(host.project()->ProjectEntered());
    workspace = host.project()->handler().workspace_dir();
    ASSERT_FALSE(workspace.empty());
    ASSERT_TRUE(std::filesystem::exists(workspace));
    package_before = CapturePackageFileState(package);
    host.Shutdown();
  }

  EXPECT_FALSE(std::filesystem::exists(workspace)) << workspace.string();
  EXPECT_EQ(warnings.CountContaining(kWorkspaceRemovalWarning), 0);
  EXPECT_EQ(ReadFileBytes(package), package_before.bytes_);
  EXPECT_EQ(std::filesystem::last_write_time(package), package_before.write_time_);
}

TEST_F(ApplicationModuleHostShutdownTests, ShutdownRemovesEnteredProjectWorkspace) {
  ScopedRecentProjectSettings settings(temp_dir_);
  const auto                  package = BuildPackedProject(temp_dir_, "entered_workspace");
  ScopedWarningCapture        warnings;
  std::filesystem::path       workspace;
  PackageFileState            package_before;
  {
    ApplicationModuleHost host;
    ASSERT_TRUE(host.project()->PreviewProject(PathToQString(package)));
    ASSERT_TRUE(WaitForProjectLoadIdle(host));
    ASSERT_TRUE(host.project()->EnterLoadedProject());
    workspace = host.project()->handler().workspace_dir();
    ASSERT_FALSE(workspace.empty());
    ASSERT_TRUE(std::filesystem::exists(workspace));
    package_before = CapturePackageFileState(package);
    host.Shutdown();
  }

  EXPECT_FALSE(std::filesystem::exists(workspace)) << workspace.string();
  EXPECT_EQ(warnings.CountContaining(kWorkspaceRemovalWarning), 0);
  EXPECT_NE(std::filesystem::last_write_time(package), package_before.write_time_);
}

TEST_F(ApplicationModuleHostShutdownTests, ShutdownWithoutProjectRemovesNothing) {
  ScopedWarningCapture warnings;
  {
    ApplicationModuleHost host;
    ASSERT_FALSE(host.project()->ServiceReady());
    EXPECT_TRUE(host.project()->handler().workspace_dir().empty());
    EXPECT_NO_THROW(host.Shutdown());
  }

  EXPECT_EQ(warnings.CountContaining(kWorkspaceRemovalWarning), 0);
  EXPECT_TRUE(std::filesystem::exists(temp_dir_));
}

TEST_F(ApplicationModuleHostShutdownTests, RepeatedShutdownRemovesWorkspaceOnce) {
  ScopedRecentProjectSettings settings(temp_dir_);
  const auto                  package = BuildPackedProject(temp_dir_, "repeated_shutdown");
  ScopedWarningCapture        warnings;
  std::filesystem::path       workspace;
  PackageFileState            package_before;
  {
    ApplicationModuleHost host;
    ASSERT_TRUE(host.project()->PreviewProject(PathToQString(package)));
    ASSERT_TRUE(WaitForProjectLoadIdle(host));
    workspace = host.project()->handler().workspace_dir();
    ASSERT_FALSE(workspace.empty());
    package_before = CapturePackageFileState(package);
    host.Shutdown();
    host.Shutdown();
    // The removal waits for the destructor: the project database is still open here.
    EXPECT_TRUE(std::filesystem::exists(workspace)) << workspace.string();
  }

  EXPECT_FALSE(std::filesystem::exists(workspace)) << workspace.string();
  EXPECT_EQ(warnings.CountContaining(kWorkspaceRemovalWarning), 0);
  EXPECT_EQ(ReadFileBytes(package), package_before.bytes_);
}

}  // namespace
}  // namespace alcedo::ui::test
