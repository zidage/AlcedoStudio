//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// search.query through real alcedo-cli and alcedo_main --headless processes on a CI RAW file:
// the text search finds the imported file by its name and pages with offset and limit.

#include <gtest/gtest.h>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>

#include "automation_editor_e2e_session.hpp"

namespace alcedo::automation {
namespace {

class AutomationSearchE2ETest : public test::AutomationEditorE2ETestBase {};

TEST_F(AutomationSearchE2ETest, FuzzySearchFindsImportedFileName) {
  ASSERT_FALSE(StartWithOnePhoto(QStringLiteral("search_project")).isEmpty());
  const QString file_name = ListItems().first().toObject().value("file_name").toString();
  ASSERT_FALSE(file_name.isEmpty());
  const QString     stem = QFileInfo(file_name).completeBaseName();

  const QJsonObject found =
      Result(QStringLiteral("search.query"), QJsonObject{{"text", stem}, {"mode", "fuzzy"}});
  EXPECT_NE(found.value("route").toString(), QStringLiteral("semantic"));
  EXPECT_FALSE(found.contains("semantic_unavailable"));
  EXPECT_EQ(found.value("total").toInt(), 1);
  EXPECT_FALSE(found.value("has_more").toBool());
  const QJsonArray matches = found.value("matches").toArray();
  ASSERT_EQ(matches.size(), 1);
  EXPECT_EQ(matches.first().toObject().value("element_id"), element_);
  EXPECT_EQ(matches.first().toObject().value("file_name").toString(), file_name);

  // A page after the last match is empty, and a text that matches no file finds nothing.
  const QJsonObject after = Result(QStringLiteral("search.query"),
                                   QJsonObject{{"text", stem}, {"mode", "fuzzy"}, {"offset", 1}});
  EXPECT_EQ(after.value("offset").toInt(), 1);
  EXPECT_TRUE(after.value("matches").toArray().isEmpty());
  const QJsonObject none =
      Result(QStringLiteral("search.query"),
             QJsonObject{{"text", "no-file-has-this-name"}, {"mode", "fuzzy"}});
  EXPECT_TRUE(none.value("matches").toArray().isEmpty());

  // An empty text gives the recommendations, and a limit over the window is rejected.
  const QJsonObject empty = Result(QStringLiteral("search.query"), QJsonObject{{"text", ""}});
  EXPECT_EQ(empty.value("route").toString(), QStringLiteral("empty"));
  EXPECT_TRUE(empty.value("recommendations").isArray());
  const test::CliRun too_many =
      Call(QStringLiteral("search.query"), QJsonObject{{"text", stem}, {"limit", 49}});
  EXPECT_EQ(too_many.exit_code, 1);
  EXPECT_EQ(too_many.json.value("error").toObject().value("code").toInt(), -32602);
  Stop();
}

}  // namespace
}  // namespace alcedo::automation
