//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// adjustments.copy and adjustments.paste through real alcedo-cli and alcedo_main --headless
// processes on two CI RAW files. The CI preset runs these tests (label ci_automation_flow) with
// the default editor backend of the platform; on Windows a build with CUDA uses CUDA.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <cmath>

#include "automation_editor_e2e_session.hpp"

namespace alcedo::automation {
namespace {

class AutomationAdjustmentsE2ETest : public test::AutomationEditorE2ETestBase {
 protected:
  /// Starts a session with a new project that holds two CI RAW files. Sets element_ to the
  /// first photo and other_ to the second.
  auto StartWithTwoPhotos(const QString& name) -> bool {
    const auto raw_files = test::CollectCiRawFiles();
    if (raw_files.size() < 2) {
      ADD_FAILURE() << "two CI RAW files are needed under " << TEST_IMG_PATH;
      return false;
    }
    if (StartWithNewProject(name).isEmpty()) {
      return false;
    }
    EXPECT_EQ(ImportAndWait({raw_files.at(0), raw_files.at(1)}).value("state").toString(),
              QStringLiteral("succeeded"));
    const QJsonArray items = ListItems();
    if (items.size() != 2) {
      ADD_FAILURE() << "expected two photos, listed " << items.size();
      return false;
    }
    element_ = items.at(0).toObject().value("element_id");
    other_   = items.at(1).toObject().value("element_id");
    return true;
  }

  /// The commit with the hash @p commit_hash in @p history; empty when it is not there.
  static auto FindCommit(const QJsonObject& history, const QJsonValue& commit_hash) -> QJsonObject {
    for (const auto& entry : history.value("commits").toArray()) {
      if (entry.toObject().value("commit_hash") == commit_hash) {
        return entry.toObject();
      }
    }
    return {};
  }

  /// The name of the active Version of the open photo.
  auto ActiveVersionName() -> QString {
    const QJsonObject list = Result(QStringLiteral("versions.list"));
    for (const auto& entry : list.value("versions").toArray()) {
      if (entry.toObject().value("active").toBool()) {
        return entry.toObject().value("name").toString();
      }
    }
    return {};
  }

  /// Checks that the head of the open photo is one Paste commit of the agent with @p note.
  void ExpectAgentPasteHead(const QString& note) {
    const QJsonObject history = Result(QStringLiteral("editor.history"));
    const QJsonObject head    = FindCommit(history, history.value("head_commit"));
    ASSERT_FALSE(head.isEmpty());
    EXPECT_EQ(head.value("author").toString(), QStringLiteral("agent"));
    EXPECT_EQ(head.value("note").toString(), note);
    EXPECT_EQ(history.value("commits").toArray().size(), 1);
  }

  QJsonValue other_;
};

TEST_F(AutomationAdjustmentsE2ETest, TunedLookPastesAsOneAgentVersionOnEditorAndLibraryPhotos) {
  ASSERT_TRUE(StartWithTwoPhotos(QStringLiteral("paste_project")));
  OpenPhoto();
  Set(QStringLiteral("exposure"), 0.8);
  Set(QStringLiteral("contrast"), 14);
  ASSERT_NEAR(Value(QStringLiteral("exposure")), 0.8, 1e-6);

  // Copy the adjustments of the Color Grade that holds exposure and contrast.
  const QJsonObject items =
      Result(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", element_}});
  QJsonArray chosen;
  for (const auto& node_value : items.value("nodes").toArray()) {
    const QJsonObject node = node_value.toObject();
    if (node.value("kind").toString() != QStringLiteral("color_grade")) {
      continue;
    }
    for (const auto& item_value : node.value("items").toArray()) {
      const QJsonObject item = item_value.toObject();
      if (item.value("item").toString() == QStringLiteral("adjustment")) {
        chosen.push_back(QJsonObject{{"node_id", node.value("node_id")},
                                     {"item", "adjustment"},
                                     {"adjustment_id", item.value("adjustment_id")}});
      }
    }
  }
  ASSERT_FALSE(chosen.isEmpty());
  const QJsonObject copied     = Result(QStringLiteral("adjustments.copy"),
                                        QJsonObject{{"element_id", element_}, {"items", chosen}});
  const QString     package_id = copied.value("package_id").toString();
  ASSERT_FALSE(package_id.isEmpty());
  EXPECT_EQ(copied.value("items").toArray().size(), chosen.size());

  // The open photo pastes through the editor; the other photo pastes in the background task.
  const QString     note   = QStringLiteral("share the look");
  const QJsonObject pasted = Result(QStringLiteral("adjustments.paste"),
                                    QJsonObject{{"package_id", package_id},
                                                {"element_ids", QJsonArray{element_, other_}},
                                                {"note", note}});
  EXPECT_EQ(pasted.value("target_count").toInt(), 2);
  const QJsonObject editor = pasted.value("editor").toObject();
  EXPECT_EQ(editor.value("element_id"), element_);
  EXPECT_TRUE(editor.value("pasted").toBool()) << editor.value("message").toString().toStdString();
  EXPECT_FALSE(editor.value("version_id").toString().isEmpty());
  const QString task_id = pasted.value("task_id").toString();
  ASSERT_FALSE(task_id.isEmpty());
  const QJsonObject task = Result(QStringLiteral("tasks.wait"), QJsonObject{{"task_id", task_id}});
  EXPECT_EQ(task.value("state").toString(), QStringLiteral("succeeded"));
  EXPECT_EQ(task.value("result").toObject().value("applied_ids").toArray(), QJsonArray{other_});
  EXPECT_TRUE(task.value("result").toObject().value("failures").toArray().isEmpty());

  // The editor photo shows the pasted Version: one agent commit with the copied values.
  EXPECT_EQ(ActiveVersionName(), QStringLiteral("Pasted Adjustments"));
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.8, 1e-6);
  ExpectAgentPasteHead(note);

  // The library photo has the same Version and values.
  Result(QStringLiteral("editor.open"), QJsonObject{{"element_id", other_}});
  EXPECT_EQ(ActiveVersionName(), QStringLiteral("Pasted Adjustments"));
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.8, 1e-6);
  EXPECT_NEAR(Value(QStringLiteral("contrast")), 14, 1e-6);
  ExpectAgentPasteHead(note);
  Stop();
}

}  // namespace
}  // namespace alcedo::automation
