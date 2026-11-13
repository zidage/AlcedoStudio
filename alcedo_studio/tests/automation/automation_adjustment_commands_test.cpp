//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// adjustments.* parameter rules and the background Paste of library photos in an in-process
// headless session.

#include <gtest/gtest.h>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QVariantMap>
#include <optional>

#include "app/pipeline_service.hpp"
#include "automation/automation_protocol.hpp"
#include "automation_in_process_session.hpp"
#include "edit/history/edit_commit_attribution.hpp"
#include "support/document_transfer_test_support.hpp"
#include "ui/album_backend_test_fixture.hpp"

namespace alcedo::automation {
namespace {

using test::InProcessAutomationSession;

auto ErrorCode(const std::optional<AutomationResponse>& response)
    -> std::optional<AutomationErrorCode> {
  if (!response.has_value() || !response->error.has_value()) {
    return std::nullopt;
  }
  return response->error->code;
}

auto ErrorPointer(const std::optional<AutomationResponse>& response) -> QString {
  if (!response.has_value() || !response->error.has_value()) {
    return {};
  }
  return response->error->data.toObject().value("pointer").toString();
}

class AutomationAdjustmentCommandsTest : public ui::test::ApplicationModuleHostTestFixture {
 protected:
  auto Folder() const -> QString { return ui::test::PathToQString(temp_dir_); }

  /// Creates a project in @p session with the CI RAW files and returns the listed photos.
  auto StartWithCiPhotos(InProcessAutomationSession& session, const QString& name) -> QJsonArray {
    const QString raw_root = QStringLiteral(TEST_IMG_PATH) + QStringLiteral("/ci_rawfiles");
    if (session
            .CallResult(QStringLiteral("project.create"),
                        QJsonObject{{"folder", Folder()}, {"name", name}})
            .isEmpty()) {
      return {};
    }
    const QString task_id = session
                                .CallResult(QStringLiteral("library.import"),
                                            QJsonObject{{"folder", raw_root}, {"recursive", false}})
                                .value("task_id")
                                .toString();
    session.CallResult(QStringLiteral("tasks.wait"),
                       QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
    return session.CallResult(QStringLiteral("library.list")).value("items").toArray();
  }
};

/// The node with @p kind in an adjustments.items result; empty when there is none.
auto NodeOfKind(const QJsonObject& items, const QString& kind) -> QJsonObject {
  for (const auto& node : items.value("nodes").toArray()) {
    if (node.toObject().value("kind").toString() == kind) {
      return node.toObject();
    }
  }
  return {};
}

/// The adjustment item with the type @p type of @p node; empty when there is none.
auto ItemOfType(const QJsonObject& node, const QString& type) -> QJsonObject {
  for (const auto& item : node.value("items").toArray()) {
    if (item.toObject().value("type").toString() == type) {
      return item.toObject();
    }
  }
  return {};
}

TEST_F(AutomationAdjustmentCommandsTest, AdjustmentCommandsWithoutProjectAreNotReady) {
  InProcessAutomationSession session;
  EXPECT_EQ(
      ErrorCode(session.Call(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", 1}})),
      AutomationErrorCode::NotReady);
  EXPECT_EQ(
      ErrorCode(session.Call(QStringLiteral("adjustments.copy"), QJsonObject{{"element_id", 1}})),
      AutomationErrorCode::NotReady);
  EXPECT_EQ(ErrorCode(session.Call(
                QStringLiteral("adjustments.paste"),
                QJsonObject{{"package_id", "package-1"}, {"element_ids", QJsonArray{1}}})),
            AutomationErrorCode::NotReady);
}

TEST_F(AutomationAdjustmentCommandsTest, CopyAndPasteRejectUnknownIdsAndItems) {
  InProcessAutomationSession session;
  const QJsonArray           photos = StartWithCiPhotos(session, QStringLiteral("reject_project"));
  ASSERT_GE(photos.size(), 2);
  const QJsonValue source = photos.at(0).toObject().value("element_id");

  const auto       unknown_photo =
      session.Call(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", 999999}});
  ASSERT_EQ(ErrorCode(unknown_photo), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(unknown_photo->error->data.toObject().value("unknown_ids").toArray(),
            QJsonArray{999999});

  const auto unknown_version = session.Call(
      QStringLiteral("adjustments.items"),
      QJsonObject{{"element_id", source}, {"version_id", "00000000000000000000000000000000"}});
  ASSERT_EQ(ErrorCode(unknown_version), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(unknown_version), QStringLiteral("/version_id"));

  const auto unknown_node =
      session.Call(QStringLiteral("adjustments.copy"),
                   QJsonObject{{"element_id", source},
                               {"items", QJsonArray{QJsonObject{{"node_id", "no.such.node"}}}}});
  ASSERT_EQ(ErrorCode(unknown_node), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(unknown_node), QStringLiteral("/items/0/node_id"));

  const QJsonObject items =
      session.CallResult(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", source}});
  const QString develop_id =
      NodeOfKind(items, QStringLiteral("develop")).value("node_id").toString();
  ASSERT_FALSE(develop_id.isEmpty());
  // An adjustment item needs its adjustment id, and a Develop node has no Mask item.
  const auto missing_adjustment = session.Call(
      QStringLiteral("adjustments.copy"),
      QJsonObject{
          {"element_id", source},
          {"items", QJsonArray{QJsonObject{{"node_id", develop_id}, {"item", "adjustment"}}}}});
  ASSERT_EQ(ErrorCode(missing_adjustment), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(missing_adjustment), QStringLiteral("/items/0/adjustment_id"));
  const auto not_on_node = session.Call(
      QStringLiteral("adjustments.copy"),
      QJsonObject{{"element_id", source},
                  {"items", QJsonArray{QJsonObject{{"node_id", develop_id}, {"item", "masks"}}}}});
  ASSERT_EQ(ErrorCode(not_on_node), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(not_on_node), QStringLiteral("/items/0"));
  const auto twice = session.Call(
      QStringLiteral("adjustments.copy"),
      QJsonObject{
          {"element_id", source},
          {"items", QJsonArray{QJsonObject{{"node_id", develop_id}, {"item", "color_temp"}},
                               QJsonObject{{"node_id", develop_id}, {"item", "color_temp"}}}}});
  ASSERT_EQ(ErrorCode(twice), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(twice), QStringLiteral("/items/1"));

  const auto unknown_package =
      session.Call(QStringLiteral("adjustments.paste"),
                   QJsonObject{{"package_id", "package-9"}, {"element_ids", QJsonArray{source}}});
  ASSERT_EQ(ErrorCode(unknown_package), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(unknown_package), QStringLiteral("/package_id"));

  const QString package_id =
      session.CallResult(QStringLiteral("adjustments.copy"), QJsonObject{{"element_id", source}})
          .value("package_id")
          .toString();
  ASSERT_FALSE(package_id.isEmpty());
  // The headless host has no library selection, so a paste needs element_ids.
  EXPECT_EQ(ErrorCode(session.Call(QStringLiteral("adjustments.paste"),
                                   QJsonObject{{"package_id", package_id}})),
            AutomationErrorCode::NotReady);
  const auto empty_name = session.Call(
      QStringLiteral("adjustments.paste"),
      QJsonObject{
          {"package_id", package_id}, {"element_ids", QJsonArray{source}}, {"version_name", "  "}});
  ASSERT_EQ(ErrorCode(empty_name), AutomationErrorCode::InvalidParams);
  EXPECT_EQ(ErrorPointer(empty_name), QStringLiteral("/version_name"));
}

TEST_F(AutomationAdjustmentCommandsTest, PasteGivesEachPhotoOneAgentVersionAndReportsEachPhoto) {
  InProcessAutomationSession session;
  const QJsonArray           photos = StartWithCiPhotos(session, QStringLiteral("paste_project"));
  ASSERT_GE(photos.size(), 3);
  const QJsonValue  source = photos.at(0).toObject().value("element_id");
  // The source look: exposure +1.5 EV, written as a stored Version of the source.
  const QVariantMap source_look =
      session.host().adjustment_transfer()->apply_coordinator()->ApplyToTargets(
          alcedo::test::MakeExposureTransferPackage(1.5),
          {static_cast<sl_element_id_t>(source.toDouble())}, QStringLiteral("Source look"));
  ASSERT_EQ(source_look.value("appliedCount").toInt(), 1);

  const QJsonObject items =
      session.CallResult(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", source}});
  const QJsonObject develop = NodeOfKind(items, QStringLiteral("develop"));
  const QJsonObject grade   = NodeOfKind(items, QStringLiteral("color_grade"));
  ASSERT_FALSE(develop.isEmpty());
  ASSERT_FALSE(grade.isEmpty());
  EXPECT_EQ(items.value("version_name").toString(), QStringLiteral("Source look"));
  EXPECT_FALSE(items.value("versions").toArray().isEmpty());
  const QJsonObject source_exposure =
      ItemOfType(grade, QStringLiteral("alcedo.adjustment.exposure"));
  ASSERT_FALSE(source_exposure.isEmpty());

  // A node without an item copies its available items; the result lists them in node order.
  const QJsonObject copied = session.CallResult(
      QStringLiteral("adjustments.copy"),
      QJsonObject{{"element_id", source},
                  {"items", QJsonArray{QJsonObject{{"node_id", grade.value("node_id")}},
                                       QJsonObject{{"node_id", develop.value("node_id")},
                                                   {"item", "color_temp"}}}}});
  const QString package_id = copied.value("package_id").toString();
  ASSERT_FALSE(package_id.isEmpty());
  const QJsonArray copied_items = copied.value("items").toArray();
  ASSERT_GE(copied_items.size(), 2);
  EXPECT_EQ(copied_items.at(0).toObject().value("node_id"), develop.value("node_id"));
  EXPECT_EQ(copied_items.at(0).toObject().value("item").toString(), QStringLiteral("color_temp"));
  EXPECT_EQ(copied_items.at(1).toObject().value("node_id"), grade.value("node_id"));

  auto pipeline_service = session.host().project()->handler().pipeline_service();
  ASSERT_TRUE(pipeline_service);
  const auto source_commits_before =
      pipeline_service->LoadHistorySnapshot(static_cast<sl_element_id_t>(source.toDouble()))
          .graph_->CommitCount();

  QJsonArray targets;
  for (qsizetype index = 1; index < photos.size(); ++index) {
    targets.push_back(photos.at(index).toObject().value("element_id"));
  }
  const QJsonObject pasted =
      session.CallResult(QStringLiteral("adjustments.paste"),
                         QJsonObject{{"package_id", package_id},
                                     {"element_ids", targets},
                                     {"version_name", "Shared look"},
                                     {"note", "apply the look of the first photo"}});
  EXPECT_EQ(pasted.value("target_count").toInt(), targets.size());
  EXPECT_FALSE(pasted.contains("editor"));
  const QString task_id = pasted.value("task_id").toString();
  ASSERT_TRUE(task_id.startsWith(QStringLiteral("bgtask-"))) << task_id.toStdString();

  const QJsonObject task = session.CallResult(
      QStringLiteral("tasks.wait"), QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
  EXPECT_EQ(task.value("kind").toString(), QStringLiteral("adjustmentPaste"));
  const QJsonObject result = task.value("result").toObject();
  EXPECT_EQ(task.value("state").toString(), QStringLiteral("succeeded"))
      << QJsonDocument(result).toJson().toStdString();
  EXPECT_EQ(result.value("applied_count").toInt(), targets.size());
  EXPECT_EQ(result.value("failure_count").toInt(), 0);
  EXPECT_TRUE(result.value("failures").toArray().isEmpty());
  EXPECT_EQ(result.value("applied_ids").toArray(), targets);
  // The finished task keeps its result for a later wait.
  EXPECT_EQ(session.CallResult(QStringLiteral("tasks.wait"), QJsonObject{{"task_id", task_id}})
                .value("result")
                .toObject(),
            result);

  // Each target has one new Version, active, with one Paste commit by the agent.
  for (const QJsonValue& target : targets) {
    const auto history =
        pipeline_service->LoadHistorySnapshot(static_cast<sl_element_id_t>(target.toDouble()));
    ASSERT_NE(history.graph_, nullptr);
    const auto& active = history.graph_->GetActiveVersionRef();
    EXPECT_EQ(active.display_name, "Shared look");
    ASSERT_TRUE(active.head_commit_hash.has_value());
    const auto* attribution = history.graph_->FindCommitAttribution(*active.head_commit_hash);
    ASSERT_NE(attribution, nullptr);
    EXPECT_EQ(attribution->author_, kEditCommitAuthorAgent);
    EXPECT_EQ(attribution->note_, "apply the look of the first photo");
    // The target shows the exposure of the source.
    const QJsonObject target_items = session.CallResult(QStringLiteral("adjustments.items"),
                                                        QJsonObject{{"element_id", target}});
    EXPECT_EQ(ItemOfType(NodeOfKind(target_items, QStringLiteral("color_grade")),
                         QStringLiteral("alcedo.adjustment.exposure"))
                  .value("value"),
              source_exposure.value("value"));
  }
  // The source keeps its history.
  EXPECT_EQ(pipeline_service->LoadHistorySnapshot(static_cast<sl_element_id_t>(source.toDouble()))
                .graph_->CommitCount(),
            source_commits_before);
}

TEST_F(AutomationAdjustmentCommandsTest, PasteOfExposureOnlyKeepsImportLookOfOtherAdjustments) {
  InProcessAutomationSession session;
  const QJsonArray           photos = StartWithCiPhotos(session, QStringLiteral("exposure_only"));
  ASSERT_GE(photos.size(), 2);
  const QJsonValue  source = photos.at(0).toObject().value("element_id");
  const QJsonValue  target = photos.at(1).toObject().value("element_id");
  // The source look: exposure +0.7 EV; its other adjustments have the import look.
  const QVariantMap source_look =
      session.host().adjustment_transfer()->apply_coordinator()->ApplyToTargets(
          alcedo::test::MakeExposureTransferPackage(0.7),
          {static_cast<sl_element_id_t>(source.toDouble())}, QStringLiteral("Source look"));
  ASSERT_EQ(source_look.value("appliedCount").toInt(), 1);

  const QJsonObject target_before = NodeOfKind(
      session.CallResult(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", target}}),
      QStringLiteral("color_grade"));
  const QJsonObject source_grade = NodeOfKind(
      session.CallResult(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", source}}),
      QStringLiteral("color_grade"));
  const QJsonObject source_exposure =
      ItemOfType(source_grade, QStringLiteral("alcedo.adjustment.exposure"));
  ASSERT_FALSE(source_exposure.isEmpty());

  const QString package_id =
      session
          .CallResult(
              QStringLiteral("adjustments.copy"),
              QJsonObject{
                  {"element_id", source},
                  {"items", QJsonArray{QJsonObject{
                                {"node_id", source_grade.value("node_id")},
                                {"item", "adjustment"},
                                {"adjustment_id", source_exposure.value("adjustment_id")}}}}})
          .value("package_id")
          .toString();
  ASSERT_FALSE(package_id.isEmpty());
  const QString task_id =
      session
          .CallResult(QStringLiteral("adjustments.paste"),
                      QJsonObject{{"package_id", package_id}, {"element_ids", QJsonArray{target}}})
          .value("task_id")
          .toString();
  const QJsonObject task = session.CallResult(
      QStringLiteral("tasks.wait"), QJsonObject{{"task_id", task_id}, {"timeout_ms", 600000}});
  ASSERT_EQ(task.value("result").toObject().value("applied_count").toInt(), 1)
      << QJsonDocument(task).toJson().toStdString();

  const QJsonObject target_after = NodeOfKind(
      session.CallResult(QStringLiteral("adjustments.items"), QJsonObject{{"element_id", target}}),
      QStringLiteral("color_grade"));
  EXPECT_EQ(ItemOfType(target_after, QStringLiteral("alcedo.adjustment.exposure")).value("value"),
            source_exposure.value("value"));
  // Contrast +15 and saturation +30 of the import look stay on the target.
  for (const QString type : {QStringLiteral("alcedo.adjustment.contrast"),
                             QStringLiteral("alcedo.adjustment.saturation")}) {
    EXPECT_EQ(ItemOfType(target_after, type).value("value"),
              ItemOfType(target_before, type).value("value"))
        << type.toStdString();
  }
  EXPECT_EQ(ItemOfType(target_after, QStringLiteral("alcedo.adjustment.contrast"))
                .value("value")
                .toString(),
            QStringLiteral("15.00"));
}

}  // namespace
}  // namespace alcedo::automation
