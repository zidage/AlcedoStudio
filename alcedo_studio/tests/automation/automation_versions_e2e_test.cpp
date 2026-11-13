//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Version commands and commit attribution through real alcedo-cli and alcedo_main --headless
// processes on a CI RAW file. The CI preset runs these tests (label ci_automation_flow) with the
// default editor backend of the platform; on Windows a build with CUDA uses CUDA.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <cmath>

#include "automation_editor_e2e_session.hpp"

namespace alcedo::automation {
namespace {

using test::CliRun;

class AutomationVersionsE2ETest : public test::AutomationEditorE2ETestBase {
 protected:
  auto        Versions() -> QJsonObject { return Result(QStringLiteral("versions.list")); }

  /// The listed Version with the id @p version_id; empty when it is not listed.
  static auto FindVersion(const QJsonObject& list, const QJsonValue& version_id) -> QJsonObject {
    for (const auto& entry : list.value("versions").toArray()) {
      if (entry.toObject().value("version_id") == version_id) {
        return entry.toObject();
      }
    }
    return {};
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
};

TEST_F(AutomationVersionsE2ETest, BranchCheckoutAndRenameAppearInVersionList) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("versions_project")).isEmpty());
  OpenPhoto();
  const QJsonObject initial = Versions();
  ASSERT_EQ(initial.value("versions").toArray().size(), 1);
  const QJsonValue first_id = initial.value("active_version_id");
  EXPECT_TRUE(FindVersion(initial, first_id).value("active").toBool());

  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 0.5), 0.1);
  const QJsonValue  first_head = Set(QStringLiteral("exposure"), 0.5).value("head_commit");

  // The name is trimmed like the panel trims it.
  const QJsonObject branched =
      Result(QStringLiteral("versions.branch"), QJsonObject{{"name", "  Warm  "}});
  const QJsonValue branch_id = branched.value("version_id");
  EXPECT_NE(branch_id, first_id);
  EXPECT_EQ(branched.value("name").toString(), QStringLiteral("Warm"));
  EXPECT_EQ(branched.value("head_commit"), first_head);
  Set(QStringLiteral("exposure"), 1.0);

  QJsonObject list = Versions();
  EXPECT_EQ(list.value("versions").toArray().size(), 2);
  EXPECT_EQ(list.value("active_version_id"), branch_id);
  EXPECT_EQ(FindVersion(list, branch_id).value("name").toString(), QStringLiteral("Warm"));

  const QJsonObject checked_out =
      Result(QStringLiteral("versions.checkout"), QJsonObject{{"version_id", first_id}});
  EXPECT_EQ(checked_out.value("version_id"), first_id);
  EXPECT_EQ(checked_out.value("head_commit"), first_head);
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.5, 1e-6);

  const QJsonObject renamed = Result(QStringLiteral("versions.rename"),
                                     QJsonObject{{"version_id", branch_id}, {"name", "Cool"}});
  EXPECT_EQ(renamed.value("name").toString(), QStringLiteral("Cool"));
  // A rename to the same name changes nothing and succeeds.
  EXPECT_EQ(Result(QStringLiteral("versions.rename"),
                   QJsonObject{{"version_id", branch_id}, {"name", " Cool "}})
                .value("name")
                .toString(),
            QStringLiteral("Cool"));
  const CliRun empty = Call(QStringLiteral("versions.rename"),
                            QJsonObject{{"version_id", branch_id}, {"name", "   "}});
  EXPECT_EQ(empty.json.value("error").toObject().value("code").toInt(), -32602);
  EXPECT_EQ(empty.json.value("error").toObject().value("data").toObject().value("pointer"),
            QJsonValue(QStringLiteral("/name")));

  list = Versions();
  EXPECT_EQ(list.value("active_version_id"), first_id);
  EXPECT_EQ(FindVersion(list, branch_id).value("name").toString(), QStringLiteral("Cool"));

  // Without a name the new Version gets the next default name.
  const QJsonObject created = Result(QStringLiteral("versions.create"));
  EXPECT_EQ(created.value("name").toString(), QStringLiteral("Version 3"));
  EXPECT_TRUE(created.value("head_commit").isNull());
  EXPECT_EQ(Versions().value("versions").toArray().size(), 3);

  const QJsonObject removed =
      Result(QStringLiteral("versions.remove"), QJsonObject{{"version_id", branch_id}});
  EXPECT_EQ(removed.value("version_id"), branch_id);
  list = Versions();
  EXPECT_EQ(list.value("versions").toArray().size(), 2);
  EXPECT_TRUE(FindVersion(list, branch_id).isEmpty());

  const CliRun unknown =
      Call(QStringLiteral("versions.checkout"), QJsonObject{{"version_id", "0123"}});
  EXPECT_EQ(unknown.json.value("error").toObject().value("code").toInt(), -32602);
  Stop();
}

// A branch from a commit that is not in the history is refused before the editor saves, so the
// editor stays Interactive and takes the next edit.
TEST_F(AutomationVersionsE2ETest, BranchFromUnknownCommitLeavesEditorInteractive) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("unknown_branch_project")).isEmpty());
  OpenPhoto();
  Set(QStringLiteral("exposure"), 0.5);

  const CliRun unknown = Call(QStringLiteral("versions.branch"),
                              QJsonObject{{"from_commit", "ffffffffffffffffffffffffffffffff"}});
  EXPECT_EQ(unknown.json.value("error").toObject().value("code").toInt(), -32602);
  EXPECT_EQ(unknown.json.value("error").toObject().value("data").toObject().value("pointer"),
            QJsonValue(QStringLiteral("/from_commit")));

  EXPECT_EQ(Result(QStringLiteral("state.get")).value("editor").toObject().value("state"),
            QJsonValue(QStringLiteral("Interactive")));
  EXPECT_EQ(Versions().value("versions").toArray().size(), 1);
  Set(QStringLiteral("exposure"), 0.8);
  EXPECT_NEAR(Value(QStringLiteral("exposure")), 0.8, 1e-6);
  Stop();
}

TEST_F(AutomationVersionsE2ETest, AgentCommitShowsAuthorAfterReopen) {
  const QString project_path = StartWithOnePhoto(QStringLiteral("author_project"));
  ASSERT_FALSE(project_path.isEmpty());
  OpenPhoto();
  ASSERT_GT(std::abs(Value(QStringLiteral("exposure")) - 0.75), 0.1);
  ASSERT_GT(std::abs(Value(QStringLiteral("contrast")) - 12), 1);

  const QJsonValue noted =
      Result(QStringLiteral("editor.set"),
             QJsonObject{{"field", "exposure"}, {"value", 0.75}, {"note", "lift the shadows"}})
          .value("head_commit");
  const QJsonValue plain =
      Result(
          QStringLiteral("editor.batch_set"),
          QJsonObject{{"changes", QJsonArray{QJsonObject{{"field", "contrast"}, {"value", 12}}}}})
          .value("head_commit");
  ASSERT_NE(noted, plain);

  const auto check_authors = [&](const QJsonObject& history) {
    const QJsonObject noted_commit = FindCommit(history, noted);
    EXPECT_EQ(noted_commit.value("author").toString(), QStringLiteral("agent"));
    EXPECT_EQ(noted_commit.value("note").toString(), QStringLiteral("lift the shadows"));
    const QJsonObject plain_commit = FindCommit(history, plain);
    EXPECT_EQ(plain_commit.value("author").toString(), QStringLiteral("agent"));
    EXPECT_EQ(plain_commit.value("note").toString(), QString());
  };
  check_authors(Result(QStringLiteral("editor.history")));
  EXPECT_TRUE(Result(QStringLiteral("editor.close")).value("closed").toBool());
  Stop();

  ASSERT_TRUE(StartWithProject(project_path));
  OpenPhoto();
  const QJsonObject reopened = Result(QStringLiteral("editor.history"));
  EXPECT_EQ(reopened.value("head_commit"), plain);
  check_authors(reopened);
  Stop();
}

}  // namespace
}  // namespace alcedo::automation
