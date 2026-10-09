//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// project.* command tests in an in-process headless session.

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QJsonObject>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "automation/automation_protocol.hpp"
#include "automation_in_process_session.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::automation {
namespace {

using test::InProcessAutomationSession;

class AutomationProjectCommandsTest : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  auto Folder() const -> QString { return ui::test::PathToQString(temp_dir_); }
};

auto CollectCiRawFiles(size_t max_count) -> std::vector<std::filesystem::path> {
  const std::filesystem::path        root{std::string(TEST_IMG_PATH) + "/ci_rawfiles"};
  std::vector<std::filesystem::path> paths;
  if (std::filesystem::exists(root)) {
    for (const auto& entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_regular_file() && is_supported_file(entry.path())) {
        paths.push_back(entry.path());
      }
    }
  }
  std::sort(paths.begin(), paths.end());
  if (paths.size() > max_count) {
    paths.resize(max_count);
  }
  return paths;
}

template <class Predicate>
auto WaitUntil(Predicate&& predicate, std::chrono::milliseconds timeout) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!predicate() && std::chrono::steady_clock::now() < deadline) {
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

TEST_F(AutomationProjectCommandsTest, OpenSecondProjectClosesFirstWithPersist) {
  const auto raw_files = CollectCiRawFiles(2);
  ASSERT_FALSE(raw_files.empty()) << "the CI RAW files are missing under " << TEST_IMG_PATH;
  InProcessAutomationSession session;
  ui::ApplicationModuleHost& host = session.host();

  const QJsonObject          first =
      session.CallResult(QStringLiteral("project.create"),
                         QJsonObject{{"folder", Folder()}, {"name", "first_project"}});
  const QString first_path = first.value("path").toString();
  ASSERT_TRUE(first_path.endsWith(QStringLiteral(".alcd"))) << first_path.toStdString();
  EXPECT_EQ(first.value("entered"), QJsonValue(true));
  EXPECT_EQ(first.value("photo_count").toInt(), 0);

  // The last change of the first project: an import that the switch must persist.
  host.import_export()->StartImport(ui::test::PathsToQStringList(raw_files));
  ASSERT_TRUE(WaitUntil([&] { return host.IsIdle(); }, std::chrono::minutes(2)));
  ASSERT_EQ(host.import_export()->ImportFailed(), 0);

  const QJsonObject second =
      session.CallResult(QStringLiteral("project.create"),
                         QJsonObject{{"folder", Folder()}, {"name", "second_project"}});
  const QString second_path = second.value("path").toString();
  ASSERT_FALSE(second_path.isEmpty());
  EXPECT_NE(second_path, first_path);
  // The switch closed the first project with persist, which wrote its package.
  EXPECT_TRUE(QFileInfo(first_path).isFile()) << first_path.toStdString();

  const QJsonObject state = session.CallResult(QStringLiteral("state.get"));
  EXPECT_EQ(state.value("project").toObject().value("path").toString(), second_path);
  EXPECT_EQ(state.value("project").toObject().value("entered"), QJsonValue(true));

  const QJsonObject reopened =
      session.CallResult(QStringLiteral("project.open"), QJsonObject{{"path", first_path}});
  EXPECT_EQ(reopened.value("path").toString(), first_path);
  EXPECT_EQ(reopened.value("photo_count").toInt(), static_cast<int>(raw_files.size()));
  EXPECT_EQ(session.CallResult(QStringLiteral("state.get"))
                .value("project")
                .toObject()
                .value("path")
                .toString(),
            first_path);
}

TEST_F(AutomationProjectCommandsTest, OpenMissingFileIsRejectedAndKeepsProjectOpen) {
  InProcessAutomationSession session;
  const QString              path = session
                           .CallResult(QStringLiteral("project.create"),
                                       QJsonObject{{"folder", Folder()}, {"name", "kept_project"}})
                           .value("path")
                           .toString();
  ASSERT_FALSE(path.isEmpty());

  const auto response =
      session.Call(QStringLiteral("project.open"),
                   QJsonObject{{"path", QDir(Folder()).filePath(QStringLiteral("missing.alcd"))}});
  ASSERT_TRUE(response.has_value());
  ASSERT_TRUE(response->error.has_value());
  EXPECT_EQ(response->error->code, AutomationErrorCode::InvalidParams);
  EXPECT_EQ(response->error->data.toObject().value("pointer").toString(), QStringLiteral("/path"));
  EXPECT_TRUE(session.host().project()->ServiceReady());
  EXPECT_EQ(session.CallResult(QStringLiteral("state.get"))
                .value("project")
                .toObject()
                .value("path")
                .toString(),
            path);
}

TEST_F(AutomationProjectCommandsTest, SaveAndCloseReportProjectStateErrors) {
  InProcessAutomationSession session;

  const auto                 save_without_project = session.Call(QStringLiteral("project.save"));
  ASSERT_TRUE(save_without_project.has_value() && save_without_project->error.has_value());
  EXPECT_EQ(save_without_project->error->code, AutomationErrorCode::NotReady);
  const auto close_without_project = session.Call(QStringLiteral("project.close"));
  ASSERT_TRUE(close_without_project.has_value() && close_without_project->error.has_value());
  EXPECT_EQ(close_without_project->error->code, AutomationErrorCode::NotReady);

  const QString path = session
                           .CallResult(QStringLiteral("project.create"),
                                       QJsonObject{{"folder", Folder()}, {"name", "saved_project"}})
                           .value("path")
                           .toString();
  EXPECT_EQ(session.CallResult(QStringLiteral("project.save")).value("path").toString(), path);
  EXPECT_EQ(session.CallResult(QStringLiteral("project.close"), QJsonObject{{"persist", false}})
                .value("closed"),
            QJsonValue(true));
  const QJsonObject project =
      session.CallResult(QStringLiteral("state.get")).value("project").toObject();
  EXPECT_EQ(project.value("loaded"), QJsonValue(false));
  EXPECT_EQ(project.value("path").toString(), QString());
}

TEST_F(AutomationProjectCommandsTest, CreateInMissingFolderIsRejected) {
  InProcessAutomationSession session;
  const auto                 response = session.Call(
      QStringLiteral("project.create"),
      QJsonObject{{"folder", QDir(Folder()).filePath(QStringLiteral("no_such_folder"))},
                                  {"name", "p"}});
  ASSERT_TRUE(response.has_value() && response->error.has_value());
  EXPECT_EQ(response->error->code, AutomationErrorCode::InvalidParams);
  EXPECT_EQ(response->error->data.toObject().value("pointer").toString(),
            QStringLiteral("/folder"));
  EXPECT_FALSE(session.host().project()->ServiceReady());
}

}  // namespace
}  // namespace alcedo::automation
