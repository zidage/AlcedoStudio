//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// ProjectLaunchCoordinator tests over the real ProjectModule of ApplicationModuleHost.

#include "ui/alcedo_main/album_backend/project_launch_coordinator.hpp"

#include <gtest/gtest.h>

#include <QSignalSpy>

#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::ui::test {
namespace {

class ProjectLaunchCoordinatorTest : public ApplicationModuleHostTestFixture {};

TEST_F(ProjectLaunchCoordinatorTest, FailedLaunchRestoresPreviousState) {
  ApplicationModuleHost host;
  ASSERT_TRUE(CreateTestProject(host));
  ProjectLaunchCoordinator* launch       = host.project_launch();
  const auto                package_path = host.project()->handler().package_path();

  QSignalSpy                finished(launch, &ProjectLaunchCoordinator::LaunchRequestFinished);
  ASSERT_TRUE(launch->BeginOpen(PathToQString(temp_dir_ / "missing.alcd")));
  EXPECT_TRUE(launch->launch_pending());
  EXPECT_TRUE(launch->loading_overlay_visible());
  EXPECT_TRUE(launch->launch_busy());
  // A second request while one is queued is refused.
  EXPECT_FALSE(launch->BeginOpen(PathToQString(temp_dir_ / "other.alcd")));

  ASSERT_TRUE(WaitForSignal(finished));
  EXPECT_FALSE(finished.front().front().toBool());
  EXPECT_FALSE(launch->launch_pending());
  EXPECT_FALSE(launch->loading_overlay_visible());
  EXPECT_FALSE(launch->launch_busy());
  // The previous project stays entered.
  EXPECT_TRUE(host.project()->ServiceReady());
  EXPECT_TRUE(host.project()->ProjectEntered());
  EXPECT_EQ(host.project()->handler().package_path(), package_path);
}

TEST_F(ProjectLaunchCoordinatorTest, FailedLaunchFromWelcomeShowsWelcomeAgain) {
  ApplicationModuleHost     host;
  ProjectLaunchCoordinator* launch = host.project_launch();

  QSignalSpy                finished(launch, &ProjectLaunchCoordinator::LaunchRequestFinished);
  ASSERT_TRUE(launch->BeginOpen(PathToQString(temp_dir_ / "missing.alcd")));
  EXPECT_TRUE(launch->welcome_dismissed_for_launch());

  ASSERT_TRUE(WaitForSignal(finished));
  EXPECT_FALSE(finished.front().front().toBool());
  EXPECT_FALSE(launch->launch_pending());
  EXPECT_FALSE(launch->welcome_dismissed_for_launch());
  EXPECT_FALSE(host.project()->ServiceReady());
}

TEST_F(ProjectLaunchCoordinatorTest, CreateLaunchEntersProjectAndClearsLaunchState) {
  ApplicationModuleHost     host;
  ProjectLaunchCoordinator* launch = host.project_launch();

  QSignalSpy                finished(launch, &ProjectLaunchCoordinator::LaunchRequestFinished);
  QSignalSpy                changed(host.project(), &ProjectModule::ProjectChanged);
  ASSERT_TRUE(launch->BeginCreate(PathToQString(temp_dir_), QStringLiteral("launch_project")));
  ASSERT_TRUE(WaitForSignal(finished));
  EXPECT_TRUE(finished.front().front().toBool());
  // The enter-mode load keeps the overlay and the dismissed welcome surface until it ends.
  EXPECT_FALSE(launch->launch_pending());
  EXPECT_TRUE(launch->loading_overlay_visible());
  EXPECT_TRUE(launch->welcome_dismissed_for_launch());

  ASSERT_TRUE(WaitForSignal(changed, 15000));
  ProcessEvents(200);
  EXPECT_TRUE(host.project()->ServiceReady());
  EXPECT_TRUE(host.project()->ProjectEntered());
  EXPECT_FALSE(launch->loading_overlay_visible());
  EXPECT_FALSE(launch->welcome_dismissed_for_launch());
  EXPECT_FALSE(launch->launch_busy());
}

}  // namespace
}  // namespace alcedo::ui::test
