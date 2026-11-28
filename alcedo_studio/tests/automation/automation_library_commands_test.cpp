//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// library.* and tasks.* parameter and state rules in an in-process headless session.

#include <OpenImageIO/imageio.h>
#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <cstdint>
#include <string>
#include <vector>

#include "app/project_service.hpp"
#include "app/sleeve_service.hpp"
#include "automation/automation_protocol.hpp"
#include "automation_in_process_session.hpp"
#include "ui/album_backend_test_fixture.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"

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

/// Encode a 24x16 JPEG whose pixels depend on @p seed at @p path.
void WriteJpeg(const QString& path, int seed) {
  ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
  constexpr int             kWidth = 24, kHeight = 16, kChannels = 3;
  std::vector<std::uint8_t> pixels(kWidth * kHeight * kChannels);
  for (std::size_t index = 0; index < pixels.size(); ++index) {
    pixels[index] = static_cast<std::uint8_t>(index * 7 + seed * 31);
  }
  const std::string file   = path.toStdString();
  auto              output = OIIO::ImageOutput::create(file);
  ASSERT_TRUE(output);
  const OIIO::ImageSpec spec(kWidth, kHeight, kChannels, OIIO::TypeDesc::UINT8);
  ASSERT_TRUE(output->open(file, spec));
  ASSERT_TRUE(output->write_image(OIIO::TypeDesc::UINT8, pixels.data()));
  ASSERT_TRUE(output->close());
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

// #1076: a mount is always a child of the library root, so library.mount takes no parent.
TEST_F(AutomationLibraryCommandsTest, MountCommandSchemaHasNoParentId) {
  InProcessAutomationSession session;
  const QJsonObject          mount = DescribedCommand(session, QStringLiteral("library.mount"));
  ASSERT_FALSE(mount.isEmpty());
  const QJsonObject params = mount.value("params_schema").toObject();
  EXPECT_EQ(SortedStrings(params.value("required").toArray()), (QStringList{"path"}));
  EXPECT_TRUE(params.value("properties").toObject().contains("path"));
  EXPECT_FALSE(params.value("properties").toObject().contains("parent_id"));
  EXPECT_FALSE(params.value("additionalProperties").toBool(true));
  EXPECT_FALSE(mount.value("description").toString().contains(QStringLiteral("parent")));

  const QJsonObject mounts = DescribedCommand(session, QStringLiteral("library.mounts"));
  ASSERT_FALSE(mounts.isEmpty());
  EXPECT_TRUE(mounts.value("description").toString().contains(QStringLiteral("display name")));
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
// A mounted folder takes its files only from its directory (mounted_folder_plan.md, MF5).
TEST_F(AutomationLibraryCommandsTest, ImportWithMountedCurrentFolderReturnsTheNamedError) {
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "mounted_import"}})
                   .isEmpty());
  const auto project = session.host().project()->handler().project();
  ASSERT_NE(project, nullptr);
  auto [mount_root, sync] = project->GetSleeveService()->CreateMountRootFolder(L"Trip", 1);
  ASSERT_TRUE(sync.success_) << sync.message_;
  session.host().folders()->ReloadTree(L"/Trip");
  ASSERT_TRUE(ui::test::WaitForFolderTree(*session.host().folders()));
  ASSERT_EQ(session.host().folders()->CurrentFolderElementId(), mount_root->element_id_);

  const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
  const QString file =
      QDir(raw_root).entryInfoList(QDir::Files, QDir::Name).value(0).absoluteFilePath();
  ASSERT_FALSE(file.isEmpty());
  for (const QJsonObject& params : {QJsonObject{{"paths", QJsonArray{file}}},
                                    QJsonObject{{"folder", raw_root}, {"recursive", false}}}) {
    const auto response = session.Call(QStringLiteral("library.import"), params);
    ASSERT_EQ(ErrorCode(response), AutomationErrorCode::Rejected);
    EXPECT_EQ(response->error->data.toObject().value("error").toString(),
              QStringLiteral("mounted_folder_structure_is_read_only"));
  }

  // No import started, and the project holds no photo.
  EXPECT_EQ(
      session.CallResult(QStringLiteral("tasks.wait"), QJsonObject{{"idle", true}}).value("idle"),
      QJsonValue(true));
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list")).value("total").toInt(), 0);
  EXPECT_TRUE(project->GetSleeveService()->ListFolderEntries(L"/Trip").empty());
}

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

// Mounted Folder Plan, MF8 (#990): the mount commands answer with the results of the service
// operations that the Collections panel runs.
TEST_F(AutomationLibraryCommandsTest, MountSyncAndUnmountReturnTheServiceResults) {
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "mounted_folder"}})
                   .isEmpty());
  const QString trip = QDir(Folder()).filePath(QStringLiteral("disk/Trip"));
  WriteJpeg(trip + QStringLiteral("/a.jpg"), 1);
  WriteJpeg(trip + QStringLiteral("/Day1/b.jpg"), 2);

  const QJsonObject mounted =
      session.CallResult(QStringLiteral("library.mount"), QJsonObject{{"path", trip}});
  const int mount_id = mounted.value("mount_id").toInt();
  ASSERT_GT(mount_id, 0);
  EXPECT_GT(mounted.value("folder_id").toInt(), 0);
  const QJsonObject first_scan = mounted.value("scan").toObject();
  EXPECT_EQ(first_scan.value("status").toString(), QStringLiteral("complete"));
  EXPECT_EQ(first_scan.value("new").toInt(), 2);

  const QJsonArray mounts =
      session.CallResult(QStringLiteral("library.mounts")).value("mounts").toArray();
  ASSERT_EQ(mounts.size(), 1);
  const QJsonObject listed = mounts.at(0).toObject();
  EXPECT_EQ(listed.value("mount_id").toInt(), mount_id);
  EXPECT_EQ(listed.value("folder_id").toInt(), mounted.value("folder_id").toInt());
  EXPECT_EQ(QFileInfo(listed.value("path").toString()).canonicalFilePath(),
            QFileInfo(trip).canonicalFilePath());
  EXPECT_EQ(listed.value("state").toString(), QStringLiteral("online"));
  EXPECT_EQ(listed.value("present_count").toInt(), 2);
  EXPECT_EQ(listed.value("missing_count").toInt(), 0);
  EXPECT_TRUE(listed.value("last_scan_at").isString());
  // #1076: the volume values of the Disk section; the first scan reached the volume.
  EXPECT_TRUE(listed.value("volume_name").isString());
  EXPECT_FALSE(listed.value("volume_mount_point").toString().isEmpty());

  // A mount inside the mounted directory is refused with the name of the rule.
  const auto nested = session.Call(QStringLiteral("library.mount"),
                                   QJsonObject{{"path", trip + QStringLiteral("/Day1")}});
  ASSERT_EQ(ErrorCode(nested), AutomationErrorCode::Rejected);
  EXPECT_EQ(nested->error->data.toObject().value("error").toString(),
            QStringLiteral("nested_mount"));

  // library.sync with no mount id scans each mount.
  ASSERT_TRUE(QFile::remove(trip + QStringLiteral("/a.jpg")));
  const QJsonArray scans =
      session.CallResult(QStringLiteral("library.sync")).value("scans").toArray();
  ASSERT_EQ(scans.size(), 1);
  EXPECT_EQ(scans.at(0).toObject().value("mount_id").toInt(), mount_id);
  EXPECT_EQ(scans.at(0).toObject().value("status").toString(), QStringLiteral("complete"));
  EXPECT_EQ(scans.at(0).toObject().value("missing").toInt(), 1);
  EXPECT_EQ(session.CallResult(QStringLiteral("library.mounts"))
                .value("mounts")
                .toArray()
                .at(0)
                .toObject()
                .value("missing_count")
                .toInt(),
            1);
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("library.sync"), QJsonObject{{"mount_id", 99}})),
            AutomationErrorCode::Rejected);

  const QJsonObject unmounted =
      session.CallResult(QStringLiteral("library.unmount"), QJsonObject{{"mount_id", mount_id}});
  EXPECT_EQ(unmounted.value("mount_id").toInt(), mount_id);
  EXPECT_TRUE(unmounted.value("unmounted").toBool());
  EXPECT_TRUE(
      session.CallResult(QStringLiteral("library.mounts")).value("mounts").toArray().isEmpty());
  // The photos stay as ordinary photos.
  EXPECT_EQ(session.CallResult(QStringLiteral("library.list")).value("total").toInt(), 2);
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("library.unmount"),
                                   QJsonObject{{"mount_id", mount_id}})),
            AutomationErrorCode::Rejected);
}

// Sleeve Node Cache Plan SN3: library.folders lists the whole tree from one tree read, each
// folder after its parent, with the id of the parent and its depth below the root.
TEST_F(AutomationLibraryCommandsTest, FoldersListsEachFolderAfterItsParentWithParentIdAndDepth) {
  InProcessAutomationSession session;
  ASSERT_FALSE(session
                   .CallResult(QStringLiteral("project.create"),
                               QJsonObject{{"folder", Folder()}, {"name", "folder_tree"}})
                   .isEmpty());
  const auto project = session.host().project()->handler().project();
  ASSERT_NE(project, nullptr);
  auto             sleeve  = project->GetSleeveService();
  const auto       trip    = sleeve->CreateFolder(L"/", L"Trip").first->element_id_;
  const auto       day     = sleeve->CreateFolder(L"/Trip", L"Day 1").first->element_id_;
  const auto       selects = sleeve->CreateFolder(L"/Trip/Day 1", L"Selects").first->element_id_;
  const auto       archive = sleeve->CreateFolder(L"/", L"Archive").first->element_id_;
  const auto       root_id = sleeve->ResolveFolder(L"/")->element_id_;

  const QJsonArray folders =
      session.CallResult(QStringLiteral("library.folders")).value("folders").toArray();
  struct Row {
    double  folder_id;
    double  parent_id;
    QString path;
    int     depth;
  };
  std::vector<Row> rows;
  for (const auto& value : folders) {
    const auto folder = value.toObject();
    rows.push_back({folder.value("folder_id").toDouble(), folder.value("parent_id").toDouble(),
                    folder.value("path").toString(), folder.value("depth").toInt()});
  }
  ASSERT_EQ(rows.size(), 5u);
  EXPECT_EQ(rows[0].folder_id, static_cast<double>(root_id));
  EXPECT_EQ(rows[0].depth, 0);
  const auto expect_row = [&rows](std::size_t index, sl_element_id_t id, sl_element_id_t parent,
                                  int depth) {
    EXPECT_EQ(rows[index].folder_id, static_cast<double>(id)) << rows[index].path.toStdString();
    EXPECT_EQ(rows[index].parent_id, static_cast<double>(parent)) << rows[index].path.toStdString();
    EXPECT_EQ(rows[index].depth, depth) << rows[index].path.toStdString();
  };
  expect_row(1, trip, root_id, 1);
  expect_row(2, day, trip, 2);
  expect_row(3, selects, day, 3);
  expect_row(4, archive, root_id, 1);
}

}  // namespace
}  // namespace alcedo::automation
