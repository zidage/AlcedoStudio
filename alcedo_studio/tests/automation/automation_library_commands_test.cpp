//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// library.* and tasks.* parameter and state rules in an in-process headless session.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>

#include "automation/automation_protocol.hpp"
#include "automation_in_process_session.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::automation {
namespace {

using test::InProcessAutomationSession;

class AutomationLibraryCommandsTest : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  auto Folder() const -> QString { return ui::test::PathToQString(temp_dir_); }
};

auto ErrorCode(const std::optional<AutomationResponse>& response)
    -> std::optional<AutomationErrorCode> {
  if (!response.has_value() || !response->error.has_value()) {
    return std::nullopt;
  }
  return response->error->code;
}

TEST_F(AutomationLibraryCommandsTest, LibraryCommandsWithoutProjectAreNotReady) {
  InProcessAutomationSession session;
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("library.list"))), AutomationErrorCode::NotReady);
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("library.folders"))),
            AutomationErrorCode::NotReady);
  EXPECT_EQ(
      ErrorCode(session.Call(QStringLiteral("library.import"), QJsonObject{{"folder", Folder()}})),
      AutomationErrorCode::NotReady);
}

TEST_F(AutomationLibraryCommandsTest, EmptyFolderImportFinishesWithZeroCounts) {
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "empty_import"}})
                   .isEmpty());
  const QString empty_folder = QDir(Folder()).filePath(QStringLiteral("empty"));
  ASSERT_TRUE(QDir().mkpath(empty_folder));
  QFile text(QDir(empty_folder).filePath(QStringLiteral("notes.txt")));
  ASSERT_TRUE(text.open(QIODevice::WriteOnly));
  text.write("not an image");
  text.close();

  for (const bool recursive : {false, true}) {
    const QString task_id =
        session
            .CallResult(QStringLiteral("library.import"),
                        QJsonObject{{"folder", empty_folder}, {"recursive", recursive}})
            .value("task_id")
            .toString();
    ASSERT_FALSE(task_id.isEmpty());
    const QJsonObject task =
        session.CallResult(QStringLiteral("tasks.wait"), QJsonObject{{"task_id", task_id}});
    EXPECT_EQ(task.value("state").toString(), QStringLiteral("succeeded"));
    EXPECT_EQ(task.value("counts").toObject().value("total").toInt(), 0);
  }
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list")).value("total").toInt(), 0);
}

TEST_F(AutomationLibraryCommandsTest, TaskWaitRejectsUnknownIdAndAmbiguousArguments) {
  InProcessAutomationSession session;
  const auto                 unknown =
      session.Call(QStringLiteral("tasks.wait"), QJsonObject{{"task_id", "import-99"}});
  ASSERT_EQ(ErrorCode(unknown), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(unknown->error->data.toObject().value("pointer").toString(),
            QStringLiteral("/task_id"));
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("tasks.wait"))),
            AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("tasks.wait"),
                                   QJsonObject{{"task_id", "import-1"}, {"idle", true}})),
            AutomationErrorCode::InvalidParams);
  // Nothing runs, so the idle wait answers at once.
  EXPECT_EQ(
      session.CallResult(QStringLiteral("tasks.wait"), QJsonObject{{"idle", true}}).value("idle"),
      QJsonValue(true));
}

TEST_F(AutomationLibraryCommandsTest, ImportRequiresExactlyOneSource) {
  InProcessAutomationSession session;
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("library.import"))),
            AutomationErrorCode::InvalidParams);
  EXPECT_EQ(
      ErrorCode(session.Call(QStringLiteral("library.import"),
                             QJsonObject{{"folder", Folder()}, {"paths", QJsonArray{Folder()}}})),
      AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("library.list"), QJsonObject{{"limit", 0}})),
            AutomationErrorCode::InvalidParams);
}

}  // namespace
}  // namespace alcedo::automation
