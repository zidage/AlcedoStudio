//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// library.* and tasks.* parameter and state rules in an in-process headless session.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <vector>

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

TEST_F(AutomationLibraryCommandsTest, SelectionCommandsAreNotReadyInHeadlessHost) {
  InProcessAutomationSession session;
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("library.selection.get"))),
            AutomationErrorCode::NotReady);
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("library.selection.set"),
                                   QJsonObject{{"element_ids", QJsonArray{1}}})),
            AutomationErrorCode::NotReady);
}

TEST_F(AutomationLibraryCommandsTest, SelectionSetReplacesGuiSelectionAndRejectsUnknownIds) {
  const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
  InProcessAutomationSession session(QStringLiteral("gui"));
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "selection_project"}})
                   .isEmpty());
  const QString task_id = session
                              .CallResult(QStringLiteral("library.import"),
                                          QJsonObject{{"folder", raw_root}, {"recursive", false}})
                              .value("task_id")
                              .toString();
  ASSERT_FALSE(task_id.isEmpty());
  session.CallResult(QStringLiteral("tasks.wait"),
                     QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
  const QJsonArray items =
      session.CallResult(QStringLiteral("library.list")).value("items").toArray();
  ASSERT_GE(items.size(), 2);
  const QJsonValue first  = items.at(0).toObject().value("element_id");
  const QJsonValue second = items.at(1).toObject().value("element_id");

  const QJsonArray selected =
      session
          .CallResult(QStringLiteral("library.selection.set"),
                      QJsonObject{{"element_ids", QJsonArray{second, first}}})
          .value("items")
          .toArray();
  EXPECT_EQ(selected.size(), 2);
  EXPECT_EQ(session.host().library()->selection()->SelectedCount(), 2);
  EXPECT_EQ(session.CallResult(QStringLiteral("library.selection.get")).value("items").toArray(),
            selected);

  const auto unknown = session.Call(QStringLiteral("library.selection.set"),
                                    QJsonObject{{"element_ids", QJsonArray{first, 999999}}});
  ASSERT_EQ(ErrorCode(unknown), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(unknown->error->data.toObject().value("unknown_ids").toArray(), QJsonArray{999999});
  EXPECT_EQ(session.host().library()->selection()->SelectedCount(), 2);

  // A deleted photo leaves the selection.
  const QJsonObject deleted =
      session.CallResult(QStringLiteral("library.delete"),
                         QJsonObject{{"element_ids", QJsonArray{first}}, {"scope", "project"}});
  EXPECT_EQ(deleted.value("deleted_ids").toArray().size(), 1);
  EXPECT_EQ(session.host().library()->selection()->SelectedCount(), 1);
}

TEST_F(AutomationLibraryCommandsTest, DeleteOfUnknownIdOrOtherScopeDeletesNothing) {
  const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "delete_project"}})
                   .isEmpty());
  const QString task_id = session
                              .CallResult(QStringLiteral("library.import"),
                                          QJsonObject{{"folder", raw_root}, {"recursive", false}})
                              .value("task_id")
                              .toString();
  session.CallResult(QStringLiteral("tasks.wait"),
                     QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
  const QJsonArray items =
      session.CallResult(QStringLiteral("library.list")).value("items").toArray();
  ASSERT_FALSE(items.isEmpty());
  const QJsonValue first = items.at(0).toObject().value("element_id");

  const auto       unknown =
      session.Call(QStringLiteral("library.delete"),
                   QJsonObject{{"element_ids", QJsonArray{first, 999999}}, {"scope", "project"}});
  ASSERT_EQ(ErrorCode(unknown), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(unknown->error->data.toObject().value("unknown_ids").toArray(), QJsonArray{999999});
  // The root folder is the current folder, so the album scope does not apply.
  EXPECT_EQ(
      ErrorCode(session.Call(QStringLiteral("library.delete"),
                             QJsonObject{{"element_ids", QJsonArray{first}}, {"scope", "album"}})),
      AutomationErrorCode::Rejected);
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list")).value("total").toInt(),
            items.size());
}

// Issue 779: an id of 2^32 or more was cast to the 32-bit element id and wrapped to a real
// photo, so the commands changed that photo.
TEST_F(AutomationLibraryCommandsTest, ElementIdOf2To32OrMoreIsParamErrorAndChangesNothing) {
  const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "alias_project"}})
                   .isEmpty());
  const QString task_id = session
                              .CallResult(QStringLiteral("library.import"),
                                          QJsonObject{{"folder", raw_root}, {"recursive", false}})
                              .value("task_id")
                              .toString();
  session.CallResult(QStringLiteral("tasks.wait"),
                     QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
  const QJsonArray items =
      session.CallResult(QStringLiteral("library.list")).value("items").toArray();
  ASSERT_FALSE(items.isEmpty());
  const double      first       = items.at(0).toObject().value("element_id").toDouble();
  // 2^32 + the id of a real photo: the old cast read it as that photo.
  const double      alias       = 4294967296.0 + first;
  const QString     alias_text  = QString::number(static_cast<qulonglong>(alias));
  const QJsonObject info_before = session.CallResult(
      QStringLiteral("library.info"), QJsonObject{{"element_ids", QJsonArray{first}}});
  ASSERT_FALSE(info_before.isEmpty());

  const QString out = QDir(Folder()).filePath(QStringLiteral("alias_thumbnail.png"));
  struct Request {
    QString     method;
    QJsonObject params;
    QString     pointer;
  };
  const std::vector<Request> requests{
      {QStringLiteral("library.delete"),
       QJsonObject{{"element_ids", QJsonArray{alias}}, {"scope", "project"}},
       QStringLiteral("/element_ids/0")},
      {QStringLiteral("library.rate"),
       QJsonObject{{"element_ids", QJsonArray{alias}}, {"rating", 5}},
       QStringLiteral("/element_ids/0")},
      {QStringLiteral("library.color_label"),
       QJsonObject{{"element_ids", QJsonArray{alias}}, {"code", "purple"}, {"on", true}},
       QStringLiteral("/element_ids/0")},
      {QStringLiteral("library.delete_mark"),
       QJsonObject{{"element_ids", QJsonArray{alias}}, {"on", true}},
       QStringLiteral("/element_ids/0")},
      {QStringLiteral("library.info"), QJsonObject{{"element_ids", QJsonArray{alias}}},
       QStringLiteral("/element_ids/0")},
      {QStringLiteral("library.thumbnail"), QJsonObject{{"element_id", alias}, {"out", out}},
       QStringLiteral("/element_id")},
      {QStringLiteral("library.list"), QJsonObject{{"folder_id", alias}},
       QStringLiteral("/folder_id")},
  };
  for (const auto& request : requests) {
    const auto response = session.Call(request.method, request.params);
    ASSERT_EQ(ErrorCode(response), AutomationErrorCode::InvalidParams)
        << request.method.toStdString();
    EXPECT_EQ(response->error->data.toObject().value("pointer").toString(), request.pointer)
        << request.method.toStdString();
    // The message names the id as the client sent it and the largest id.
    EXPECT_TRUE(response->error->message.contains(alias_text))
        << request.method.toStdString() << ": " << response->error->message.toStdString();
    EXPECT_TRUE(response->error->message.contains(QStringLiteral("4294967295")))
        << request.method.toStdString() << ": " << response->error->message.toStdString();
  }

  // Ids past 2^53 and past 2^64 are errors at the id too, never "unknown element ids: 0".
  for (const double huge : {9007199254740992.0, 18446744073709551616.0, 1e300}) {
    const auto response =
        session.Call(QStringLiteral("library.delete"),
                     QJsonObject{{"element_ids", QJsonArray{first, huge}}, {"scope", "project"}});
    ASSERT_EQ(ErrorCode(response), AutomationErrorCode::InvalidParams) << huge;
    EXPECT_EQ(response->error->data.toObject().value("pointer").toString(),
              QStringLiteral("/element_ids/1"))
        << huge;
    EXPECT_FALSE(response->error->message.contains(QStringLiteral("unknown element ids")))
        << response->error->message.toStdString();
  }

  // The photo keeps its place, rating, marks, and no thumbnail file was written.
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list")).value("total").toInt(),
            items.size());
  EXPECT_EQ(session.CallResult(QStringLiteral("library.info"),
                               QJsonObject{{"element_ids", QJsonArray{first}}}),
            info_before);
  EXPECT_FALSE(QFile::exists(out));
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list"), QJsonObject{{"deleted", true}})
                .value("total")
                .toInt(),
            0);
}

auto DescribedCommand(InProcessAutomationSession& session, const QString& method) -> QJsonObject {
  for (const QJsonValue& command : session.registry().Describe()) {
    if (command.toObject().value("method").toString() == method) {
      return command.toObject();
    }
  }
  return {};
}

auto SortedStrings(const QJsonArray& values) -> QStringList {
  QStringList strings;
  for (const QJsonValue& value : values) {
    strings.push_back(value.toString());
  }
  strings.sort();
  return strings;
}

TEST_F(AutomationLibraryCommandsTest, MarkCommandsAreRegisteredWithSchemas) {
  InProcessAutomationSession session;
  const QJsonArray  code_keys{"red", "orange", "yellow", "green", "blue", "purple", "gray"};

  const QJsonObject color_label = DescribedCommand(session, QStringLiteral("library.color_label"));
  ASSERT_FALSE(color_label.isEmpty());
  EXPECT_TRUE(color_label.value("changes_state").toBool());
  QJsonObject params = color_label.value("params_schema").toObject();
  EXPECT_EQ(SortedStrings(params.value("required").toArray()),
            (QStringList{"code", "element_ids", "on"}));
  EXPECT_EQ(params.value("properties").toObject().value("code").toObject().value("enum"),
            QJsonValue(code_keys));
  EXPECT_EQ(
      SortedStrings(color_label.value("result_schema").toObject().value("required").toArray()),
      (QStringList{"code", "marked_count", "on", "task_id"}));

  const QJsonObject delete_mark = DescribedCommand(session, QStringLiteral("library.delete_mark"));
  ASSERT_FALSE(delete_mark.isEmpty());
  EXPECT_TRUE(delete_mark.value("changes_state").toBool());
  params = delete_mark.value("params_schema").toObject();
  EXPECT_EQ(SortedStrings(params.value("required").toArray()), (QStringList{"element_ids", "on"}));
  EXPECT_EQ(
      SortedStrings(delete_mark.value("result_schema").toObject().value("required").toArray()),
      (QStringList{"marked_count", "on", "task_id"}));

  const QJsonObject color_labels =
      DescribedCommand(session, QStringLiteral("library.color_labels"));
  ASSERT_FALSE(color_labels.isEmpty());
  EXPECT_FALSE(color_labels.value("changes_state").toBool());
  const QJsonObject code_item = color_labels.value("result_schema")
                                    .toObject()
                                    .value("properties")
                                    .toObject()
                                    .value("codes")
                                    .toObject()
                                    .value("items")
                                    .toObject();
  EXPECT_EQ(SortedStrings(code_item.value("required").toArray()),
            (QStringList{"code", "color", "default_name", "index", "name", "renamed"}));

  const QJsonObject rename =
      DescribedCommand(session, QStringLiteral("library.color_label_name.set"));
  ASSERT_FALSE(rename.isEmpty());
  EXPECT_TRUE(rename.value("changes_state").toBool());
  EXPECT_EQ(SortedStrings(rename.value("params_schema").toObject().value("required").toArray()),
            (QStringList{"code", "name"}));

  const QJsonObject list = DescribedCommand(session, QStringLiteral("library.list"));
  const QJsonObject list_props =
      list.value("params_schema").toObject().value("properties").toObject();
  EXPECT_EQ(list_props.value("color_label").toObject().value("enum"), QJsonValue(code_keys));
  EXPECT_EQ(list_props.value("deleted").toObject().value("type"), QJsonValue("boolean"));
  const QJsonObject list_item = list.value("result_schema")
                                    .toObject()
                                    .value("properties")
                                    .toObject()
                                    .value("items")
                                    .toObject()
                                    .value("items")
                                    .toObject();
  EXPECT_TRUE(SortedStrings(list_item.value("required").toArray()).contains("color_labels"));
  EXPECT_TRUE(SortedStrings(list_item.value("required").toArray()).contains("delete_mark"));
}

TEST_F(AutomationLibraryCommandsTest, UnknownColorLabelCodeIsParamError) {
  InProcessAutomationSession session;
  const auto                 pointer_of = [](const std::optional<AutomationResponse>& response) {
    return response->error->data.toObject().value("pointer").toString();
  };
  const auto color_label =
      session.Call(QStringLiteral("library.color_label"),
                   QJsonObject{{"element_ids", QJsonArray{1}}, {"code", "pink"}, {"on", true}});
  ASSERT_EQ(ErrorCode(color_label), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(pointer_of(color_label), QStringLiteral("/code"));
  // Keys are lower case.
  const auto list =
      session.Call(QStringLiteral("library.list"), QJsonObject{{"color_label", "Red"}});
  ASSERT_EQ(ErrorCode(list), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(pointer_of(list), QStringLiteral("/color_label"));
  const auto rename = session.Call(QStringLiteral("library.color_label_name.set"),
                                   QJsonObject{{"code", "pink"}, {"name", "Picks"}});
  ASSERT_EQ(ErrorCode(rename), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(pointer_of(rename), QStringLiteral("/code"));
}

TEST_F(AutomationLibraryCommandsTest, MarkBatchWhileAnotherSavesIsRejected) {
  const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "marks_project"}})
                   .isEmpty());
  const QString import_id = session
                                .CallResult(QStringLiteral("library.import"),
                                            QJsonObject{{"folder", raw_root}, {"recursive", false}})
                                .value("task_id")
                                .toString();
  session.CallResult(QStringLiteral("tasks.wait"),
                     QJsonObject{{"task_id", import_id}, {"timeout_ms", 600000}});
  const QJsonArray items =
      session.CallResult(QStringLiteral("library.list")).value("items").toArray();
  ASSERT_GE(items.size(), 2);
  const QJsonValue  first   = items.at(0).toObject().value("element_id");
  const QJsonValue  second  = items.at(1).toObject().value("element_id");

  // The answer of the first batch comes before its save finishes on the GUI thread, so the
  // second batch finds the first one still saving.
  const QJsonObject started = session.CallResult(
      QStringLiteral("library.color_label"),
      QJsonObject{{"element_ids", QJsonArray{first, second}}, {"code", "red"}, {"on", true}});
  EXPECT_EQ(started.value("marked_count").toInt(), 2);
  EXPECT_EQ(started.value("code").toString(), QStringLiteral("red"));
  const auto busy = session.Call(QStringLiteral("library.delete_mark"),
                                 QJsonObject{{"element_ids", QJsonArray{first}}, {"on", true}});
  ASSERT_EQ(ErrorCode(busy), AutomationErrorCode::Rejected);
  EXPECT_FALSE(busy->error->message.isEmpty());

  EXPECT_EQ(session
                .CallResult(QStringLiteral("tasks.wait"),
                            QJsonObject{{"task_id", started.value("task_id")}})
                .value("state")
                .toString(),
            QStringLiteral("succeeded"));
  const QJsonObject red =
      session.CallResult(QStringLiteral("library.list"), QJsonObject{{"color_label", "red"}});
  EXPECT_EQ(red.value("total").toInt(), 2);
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list"), QJsonObject{{"deleted", true}})
                .value("total")
                .toInt(),
            0);
}

}  // namespace
}  // namespace alcedo::automation
