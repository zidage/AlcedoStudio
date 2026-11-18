//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// library.* and tasks.* parameter and state rules in an in-process headless session.

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSettings>
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
  void SetUp() override {
    ui::test::ApplicationModuleHostTestFixture::SetUp();
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, Folder());
  }
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

TEST_F(AutomationLibraryCommandsTest,
       AutomationOmittedOptionInheritsPreferenceAndExplicitOptionOverridesIt) {
  InProcessAutomationSession session;
  const auto                 created =
      session.CallResult("project.create", {{"folder", Folder()}, {"name", "duplicates"}});
  ASSERT_FALSE(created.isEmpty());
  const auto       source = std::filesystem::path(__FILE__).parent_path().parent_path() /
                            "resources/raster/adobe_rgb_exif_r03_no_icc.jpg";
  const QJsonArray paths{ui::test::PathToQString(source)};
  session.host().import_behavior()->SetSkipDuplicateImports(false);
  auto import = [&](QJsonObject options) {
    options.insert("paths", paths);
    const auto id = session.CallResult("library.import", options).value("task_id");
    return session.CallResult("tasks.wait", {{"task_id", id}}).value("counts").toObject();
  };
  EXPECT_EQ(import({}).value("completed").toInt(), 1);
  const auto skipped = import({{"skip_duplicates", true}});
  EXPECT_EQ(skipped.value("duplicates").toInt(), 1);
  EXPECT_EQ(skipped.value("completed").toInt(), 0);
  EXPECT_EQ(skipped.value("failed").toInt(), 0);
  EXPECT_FALSE(session.host().import_behavior()->SkipDuplicateImports());
  EXPECT_EQ(import({}).value("completed").toInt(), 1);
  EXPECT_EQ(session.CallResult("library.list").value("total").toInt(), 2);
  const auto reopened = session.CallResult("project.open", {{"path", created.value("path")}});
  EXPECT_EQ(reopened.value("photo_count").toInt(), 2);
  EXPECT_EQ(import({{"skip_duplicates", true}}).value("duplicates").toInt(), 1);
}

TEST_F(AutomationLibraryCommandsTest, FolderRequestsKeepThePreferenceAndPerRequestOverride) {
  InProcessAutomationSession session;
  ASSERT_FALSE(
      session.CallResult("project.create", {{"folder", Folder()}, {"name", "folder_policy"}})
          .isEmpty());
  const auto input = QDir(Folder()).filePath("incoming");
  ASSERT_TRUE(QDir().mkpath(input));
  const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                       "resources/raster/adobe_rgb_exif_r03_no_icc.jpg";
  ASSERT_TRUE(QFile::copy(ui::test::PathToQString(fixture), QDir(input).filePath("image.jpg")));
  session.host().import_behavior()->SetSkipDuplicateImports(true);
  const auto first =
      session
          .CallResult("library.import",
                      {{"folder", input}, {"recursive", false}, {"skip_duplicates", false}})
          .value("task_id");
  EXPECT_EQ(session.CallResult("tasks.wait", {{"task_id", first}})
                .value("counts")
                .toObject()
                .value("completed")
                .toInt(),
            1);
  const auto second = session.CallResult("library.import", {{"folder", input}, {"recursive", true}})
                          .value("task_id");
  const auto counts =
      session.CallResult("tasks.wait", {{"task_id", second}}).value("counts").toObject();
  EXPECT_EQ(counts.value("duplicates").toInt(), 1);
  EXPECT_EQ(counts.value("failed").toInt(), 0);
  EXPECT_TRUE(session.host().import_behavior()->SkipDuplicateImports());
}

TEST_F(AutomationLibraryCommandsTest,
       ChangedSourceReportsUpdatedCountAndNoticeAndReopensAsOneEntry) {
  InProcessAutomationSession session;
  const auto                 created =
      session.CallResult("project.create", {{"folder", Folder()}, {"name", "changed_source"}});
  ASSERT_FALSE(created.isEmpty());
  const auto fixture = std::filesystem::path(__FILE__).parent_path().parent_path() /
                       "resources/raster/adobe_rgb_exif_r03_no_icc.jpg";
  const auto source  = QDir(Folder()).filePath("source.jpg");
  ASSERT_TRUE(QFile::copy(ui::test::PathToQString(fixture), source));
  const auto import = [&] {
    const auto id = session
                        .CallResult("library.import",
                                    {{"paths", QJsonArray{source}}, {"skip_duplicates", true}})
                        .value("task_id");
    return session.CallResult("tasks.wait", {{"task_id", id}});
  };
  EXPECT_EQ(import().value("counts").toObject().value("completed").toInt(), 1);
  QFile changed(source);
  ASSERT_TRUE(changed.open(QIODevice::Append));
  ASSERT_EQ(changed.write("x"), 1);
  changed.close();
  const auto result = import();
  EXPECT_EQ(result.value("state").toString(), QStringLiteral("succeeded"));
  const auto counts = result.value("counts").toObject();
  EXPECT_EQ(counts.value("completed").toInt(), 1);
  EXPECT_EQ(counts.value("updated").toInt(), 1);
  EXPECT_EQ(counts.value("duplicates").toInt(), 0);
  EXPECT_EQ(counts.value("failed").toInt(), 0);
  EXPECT_TRUE(result.value("message").toString().contains("Existing edits and ratings"));
  EXPECT_EQ(session.CallResult("library.list").value("total").toInt(), 1);
  EXPECT_EQ(session.CallResult("project.open", {{"path", created.value("path")}})
                .value("photo_count")
                .toInt(),
            1);
  EXPECT_EQ(import().value("counts").toObject().value("duplicates").toInt(), 1);
}

TEST_F(AutomationLibraryCommandsTest, InvalidDuplicateOverrideIsRejectedBeforeImport) {
  InProcessAutomationSession session;
  const auto                 response =
      session.Call("library.import", {{"folder", Folder()}, {"skip_duplicates", "true"}});
  EXPECT_EQ(ErrorCode(response), AutomationErrorCode::InvalidParams);
  EXPECT_FALSE(session.host().import_export()->ImportRunning());
}

TEST_F(AutomationLibraryCommandsTest, NativePickerCancellationAndFileDropUseTheCommonPreference) {
  InProcessAutomationSession session;
  ASSERT_FALSE(session.CallResult("project.create", {{"folder", Folder()}, {"name", "file_drop"}})
                   .isEmpty());
  auto* handler = session.host().import_export();
  handler->StartImport({});
  EXPECT_FALSE(handler->ImportRunning());
  session.host().import_behavior()->SetSkipDuplicateImports(true);
  const auto source = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "resources/raster/adobe_rgb_exif_r03_no_icc.jpg";
  handler->StartImport({ui::test::PathToQString(source), ui::test::PathToQString(source)});
  session.host().import_behavior()->SetSkipDuplicateImports(false);
  session.CallResult("tasks.wait", {{"idle", true}});
  EXPECT_EQ(handler->ImportTotal(), 2);
  EXPECT_EQ(handler->ImportCompleted(), 1);
  EXPECT_EQ(handler->ImportDuplicates(), 1);
  EXPECT_EQ(handler->ImportFailed(), 0);
  handler->StartImport({ui::test::PathToQString(source)});
  session.CallResult("tasks.wait", {{"idle", true}});
  EXPECT_EQ(handler->ImportCompleted(), 1);
  EXPECT_EQ(handler->ImportDuplicates(), 0);
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

TEST_F(AutomationLibraryCommandsTest, DeleteOfPhotoInSavingMarkBatchIsRejected) {
  const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "marks_delete"}})
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

  // The mark batch answers before its save finishes, so the delete finds the photo locked.
  const QJsonObject started = session.CallResult(
      QStringLiteral("library.color_label"),
      QJsonObject{{"element_ids", QJsonArray{first, second}}, {"code", "red"}, {"on", true}});
  ASSERT_FALSE(started.value("task_id").toString().isEmpty());
  const auto locked =
      session.Call(QStringLiteral("library.delete"),
                   QJsonObject{{"element_ids", QJsonArray{first}}, {"scope", "project"}});
  ASSERT_EQ(ErrorCode(locked), AutomationErrorCode::Rejected);
  EXPECT_FALSE(locked->error->message.isEmpty());
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list")).value("total").toInt(),
            items.size());

  // The same delete starts once the batch is complete.
  EXPECT_EQ(session
                .CallResult(QStringLiteral("tasks.wait"),
                            QJsonObject{{"task_id", started.value("task_id")}})
                .value("state")
                .toString(),
            QStringLiteral("succeeded"));
  const QJsonObject deleted =
      session.CallResult(QStringLiteral("library.delete"),
                         QJsonObject{{"element_ids", QJsonArray{first}}, {"scope", "project"}});
  EXPECT_EQ(deleted.value("deleted_ids").toArray(), QJsonArray{first});
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list")).value("total").toInt(),
            items.size() - 1);
}

// Each bucket that library.stats returns selects its photo count in the bucket parameter of
// library.list and of library.stats.
TEST_F(AutomationLibraryCommandsTest, StatsBucketLabelsSelectTheirPhotoCounts) {
  const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "stats_project"}})
                   .isEmpty());
  const QString import_id = session
                                .CallResult(QStringLiteral("library.import"),
                                            QJsonObject{{"folder", raw_root}, {"recursive", false}})
                                .value("task_id")
                                .toString();
  session.CallResult(QStringLiteral("tasks.wait"),
                     QJsonObject{{"task_id", import_id}, {"timeout_ms", 600000}});
  const QJsonArray items =
      session.CallResult(QStringLiteral("library.list"), QJsonObject{{"limit", 1000}})
          .value("items")
          .toArray();
  ASSERT_GE(items.size(), 2);
  const QJsonObject marked = session.CallResult(
      QStringLiteral("library.color_label"),
      QJsonObject{{"element_ids", QJsonArray{items.at(0).toObject().value("element_id")}},
                  {"code", "green"},
                  {"on", true}});
  session.CallResult(QStringLiteral("tasks.wait"),
                     QJsonObject{{"task_id", marked.value("task_id")}});

  const QString     zone = QStringLiteral("America/New_York");
  const QJsonObject stats =
      session.CallResult(QStringLiteral("library.stats"), QJsonObject{{"time_zone", zone}});
  EXPECT_EQ(stats.value("total").toInt(), items.size());
  EXPECT_EQ(stats.value("delete_mark").toInt(), 0);
  const QJsonArray color_labels = stats.value("color_labels").toArray();
  ASSERT_EQ(color_labels.size(), 7);
  for (const QJsonValue& value : color_labels) {
    const QJsonObject code = value.toObject();
    EXPECT_EQ(code.value("count").toInt(), code.value("code").toString() == u"green" ? 1 : 0)
        << code.value("code").toString().toStdString();
  }

  for (const char* dimension : {"capture_date", "camera", "lens", "rating", "import_date"}) {
    const QJsonArray buckets = stats.value(QLatin1String(dimension)).toArray();
    ASSERT_FALSE(buckets.isEmpty()) << dimension;
    int sum = 0;
    for (const QJsonValue& value : buckets) {
      const QJsonObject bucket = value.toObject();
      const int         count  = bucket.value("count").toInt();
      sum += count;
      const QJsonObject filter{
          {QLatin1String(dimension), bucket.value("label")}, {"time_zone", zone}, {"limit", 1000}};
      const QJsonObject list = session.CallResult(QStringLiteral("library.list"), filter);
      EXPECT_EQ(list.value("total").toInt(), count)
          << dimension << " " << bucket.value("label").toString().toStdString();
      EXPECT_EQ(list.value("items").toArray().size(), count) << dimension;
      QJsonObject stats_filter = filter;
      stats_filter.remove("limit");
      EXPECT_EQ(
          session.CallResult(QStringLiteral("library.stats"), stats_filter).value("total").toInt(),
          count)
          << dimension << " " << bucket.value("label").toString().toStdString();
    }
    EXPECT_EQ(sum, items.size()) << dimension;
  }

  // The import_date buckets need a time zone, and so does the import_date filter.
  EXPECT_FALSE(session.CallResult(QStringLiteral("library.stats")).contains("import_date"));
  const auto no_zone =
      session.Call(QStringLiteral("library.list"), QJsonObject{{"import_date", "2026-01-01"}});
  EXPECT_EQ(ErrorCode(no_zone), AutomationErrorCode::InvalidParams);
  const auto bad_day =
      session.Call(QStringLiteral("library.stats"),
                   QJsonObject{{"import_date", "2026-02-30"}, {"time_zone", zone}});
  EXPECT_EQ(ErrorCode(bad_day), AutomationErrorCode::InvalidParams);

  // Without an active semantic model no photo has a label bucket.
  EXPECT_TRUE(stats.value("label").toArray().isEmpty());
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list"), QJsonObject{{"label", "sky"}})
                .value("total")
                .toInt(),
            0);

  const QJsonObject described = DescribedCommand(session, QStringLiteral("library.stats"));
  ASSERT_FALSE(described.isEmpty());
  EXPECT_FALSE(described.value("changes_state").toBool());
}

// A failed read answers with the database message; the SQL text of the statement stays in the
// log (#814).
TEST_F(AutomationLibraryCommandsTest, FailedReadAnswersWithTheDatabaseMessageWithoutSql) {
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "stats_error_project"}})
                   .isEmpty());
  // DuckDB reads the time zone only for a photo: an empty library gives an empty answer.
  const QString import_id =
      session
          .CallResult(QStringLiteral("library.import"),
                      QJsonObject{{"folder", QStringLiteral(TEST_IMG_PATH "/ci_rawfiles")},
                                  {"recursive", false}})
          .value("task_id")
          .toString();
  session.CallResult(QStringLiteral("tasks.wait"),
                     QJsonObject{{"task_id", import_id}, {"timeout_ms", 600000}});
  const auto failed = session.Call(QStringLiteral("library.stats"),
                                   QJsonObject{{"time_zone", "Mars/Olympus_Mons"}});
  ASSERT_EQ(ErrorCode(failed), AutomationErrorCode::Failed);
  const QString message = failed->error->message;
  EXPECT_TRUE(message.contains(QStringLiteral("Mars/Olympus_Mons"))) << message.toStdString();
  EXPECT_FALSE(message.contains(QStringLiteral("SQL"))) << message.toStdString();
  EXPECT_FALSE(message.contains(QStringLiteral("SELECT"))) << message.toStdString();
}

}  // namespace
}  // namespace alcedo::automation
