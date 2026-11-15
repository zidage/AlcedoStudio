//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QString>
#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/editor_history_types.hpp"
#include "app/editor_session_types.hpp"
#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"
#include "ui/alcedo_main/automation/automation_editor_operation.hpp"

namespace alcedo::automation {
namespace {

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

auto Text(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

auto HeadValue(const EditorHistorySnapshot& history) -> QJsonValue {
  if (!history.active_head.has_value()) {
    return QJsonValue(QJsonValue::Null);
  }
  return QString::fromStdString(history.active_head->ToString());
}

/// The open editor, or null. Sends -32001 when no image is open.
auto OpenEditor(ui::ApplicationModuleHost* host, AutomationReply& reply)
    -> ui::EditorSessionController* {
  ui::EditorSessionController* editor = host->editor_session();
  if (editor == nullptr || !editor->has_image()) {
    SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                             QStringLiteral("No editor image is open."));
    return nullptr;
  }
  return editor;
}

/// The Color Grades on the image backbone of @p graph, Develop side first.
auto BackboneColorGradeIds(const PipelineGraph& graph) -> std::vector<NodeId> {
  std::vector<NodeId> ids;
  for (const NodeId& id : graph.ImageBackboneNodeIds()) {
    if (dynamic_cast<const ColorGradeNodeModel*>(graph.FindNode(id)) != nullptr) {
      ids.push_back(id);
    }
  }
  return ids;
}

/// One `nodes.list` entry: @p grade of @p graph at @p backbone_index among the backbone Color
/// Grades (Develop side first), or off the backbone when nothing.
auto ColorGradeJson(const PipelineGraph& graph, const ColorGradeNodeModel& grade,
                    std::optional<int> backbone_index) -> QJsonObject {
  return QJsonObject{{"node_id", Text(grade.Id().Value())},
                     {"name", Text(grade.DisplayName())},
                     {"hidden", !grade.Enabled()},
                     {"detached", graph.IsDetachedColorGrade(grade.Id())},
                     {"deletion_protected", grade.DeletionProtected()},
                     {"mask_count", static_cast<qint64>(grade.MaskCount())},
                     {"backbone_index", backbone_index.has_value() ? QJsonValue(*backbone_index)
                                                                   : QJsonValue(QJsonValue::Null)}};
}

/// The Color Grades of @p document: the backbone Color Grades from Develop to the display
/// transform, then every other Color Grade (the detached ones) in node storage order.
auto ColorGradeList(const PipelineDocument& document) -> QJsonArray {
  const PipelineGraph&      graph    = document.Graph();
  const std::vector<NodeId> backbone = BackboneColorGradeIds(graph);
  QJsonArray                list;
  for (std::size_t index = 0; index < backbone.size(); ++index) {
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(graph.FindNode(backbone[index]));
    list.push_back(ColorGradeJson(graph, *grade, static_cast<int>(index)));
  }
  for (const auto& node : graph.Nodes()) {
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node.get());
    if (grade == nullptr ||
        std::find(backbone.begin(), backbone.end(), grade->Id()) != backbone.end()) {
      continue;
    }
    list.push_back(ColorGradeJson(graph, *grade, std::nullopt));
  }
  return list;
}

/// The `nodes.list` entry of the Color Grade @p id in @p document; nothing when @p document has
/// no Color Grade with that id.
auto ColorGradeEntry(const PipelineDocument& document, const NodeId& id)
    -> std::optional<QJsonObject> {
  const PipelineGraph& graph = document.Graph();
  const auto*          grade = dynamic_cast<const ColorGradeNodeModel*>(graph.FindNode(id));
  if (grade == nullptr) {
    return std::nullopt;
  }
  const std::vector<NodeId> backbone = BackboneColorGradeIds(graph);
  const auto                position = std::find(backbone.begin(), backbone.end(), id);
  return ColorGradeJson(graph, *grade,
                        position == backbone.end()
                            ? std::nullopt
                            : std::optional<int>(static_cast<int>(position - backbone.begin())));
}

/// Answers nodes.set_visibility after the commit and its frame with the new state of @p id. A
/// committed failure (the frame of the commit failed to render) carries the head commit, read
/// after the queued history change, so the agent can undo it.
void SendVisibilityResult(const QPointer<AutomationCommandWait>& wait,
                          ui::EditorSessionController* editor, const NodeId& id,
                          const std::optional<OperationFailure>& failure) {
  if (wait == nullptr) {
    return;
  }
  if (failure.has_value() && !failure->committed_) {
    wait->SendOwnerError(failure->code_, failure->message_);
    return;
  }
  AfterQueuedChanges(wait, [wait, editor, id, failure] {
    if (wait == nullptr) {
      return;
    }
    if (failure.has_value()) {
      wait->SendError(failure->code_, failure->message_,
                      QJsonObject{{"reason", failure->message_},
                                  {"head_commit", HeadValue(editor->history_snapshot())}});
      return;
    }
    const auto document = editor->pipeline_document();
    const auto entry    = document ? ColorGradeEntry(*document, id) : std::nullopt;
    if (!entry.has_value()) {
      wait->SendOwnerError(AutomationErrorCode::Failed,
                           QStringLiteral("The Color Grade is no longer in the document."));
      return;
    }
    wait->SendResult(*entry);
  });
}

auto ColorGradeSchema() -> QJsonObject {
  const auto flag = [](const char* description) {
    return QJsonObject{{"type", "boolean"}, {"description", description}};
  };
  return AutomationObjectSchema(
      QJsonObject{
          {"node_id",
           QJsonObject{{"type", "string"}, {"description", "Id of the Color Grade node."}}},
          {"name",
           QJsonObject{{"type", "string"},
                       {"description", "Name of the Color Grade, as the editor shows it."}}},
          {"hidden", flag("True when the Color Grade is hidden: the render skips it, and its "
                          "parameters and Masks stay in the document.")},
          {"detached", flag("True when the Color Grade has no scene-image edge: it stays in the "
                            "document outside the render.")},
          {"deletion_protected", flag("True when the Color Grade is locked against deletion.")},
          {"mask_count", QJsonObject{{"type", "integer"},
                                     {"minimum", 0},
                                     {"description", "Number of Masks of the Color Grade."}}},
          {"backbone_index",
           QJsonObject{{"type", QJsonArray{"integer", "null"}},
                       {"description",
                        "Position among the Color Grades on the image path from Develop to the "
                        "display transform, from 0 at the Develop side; null for a Color Grade "
                        "off that path."}}}},
      QJsonArray{"node_id", "name", "hidden", "detached", "deletion_protected", "mask_count",
                 "backbone_index"});
}

}  // namespace

auto RegisterAutomationNodeCommands(AutomationCommandRegistry& registry,
                                    ui::ApplicationModuleHost* host, QString* error) -> bool {
  bool                  ok           = true;
  const QJsonObject     grade_schema = ColorGradeSchema();

  AutomationCommandSpec list;
  list.method      = QStringLiteral("nodes.list");
  list.description = QStringLiteral(
      "Returns the Color Grades of the open image with their visibility: first the Color Grades "
      "on the image path from Develop to the display transform, in that order, then the "
      "detached Color Grades in document order.");
  list.params_schema = AutomationClosedParamsSchema();
  list.result_schema = AutomationObjectSchema(
      QJsonObject{{"color_grades", QJsonObject{{"type", "array"}, {"items", grade_schema}}}},
      QJsonArray{"color_grades"});
  list.handler = [host](const QJsonObject&, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const auto document = editor->pipeline_document();
    if (!document) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                               QStringLiteral("The editor document is not available."));
      return;
    }
    reply.SendResult(QJsonObject{{"color_grades", ColorGradeList(*document)}});
  };
  RegisterOrFail(registry, std::move(list), &ok, error);

  AutomationCommandSpec set_visibility;
  set_visibility.method      = QStringLiteral("nodes.set_visibility");
  set_visibility.description = QStringLiteral(
      "Shows or hides one Color Grade, like the visibility button of its Mask Group: one history "
      "commit (Show Color Grade or Hide Color Grade). A hidden Color Grade keeps its parameters "
      "and Masks, and the render skips it in the preview and the export. A value equal to the "
      "current one makes no commit and succeeds. Answers after the commit and its rendered "
      "frame with the new state of the Color Grade. When the frame of the commit fails to "
      "render, the commit stays and the error data has 'reason' and 'head_commit'; editor.undo "
      "reverts it.");
  set_visibility.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"node_id",
           QJsonObject{{"type", "string"}, {"description", "Id of a Color Grade (nodes.list)."}}},
          {"visible", QJsonObject{{"type", "boolean"},
                                  {"description", "true shows the Color Grade; false hides it."}}}},
      QJsonArray{"node_id", "visible"});
  set_visibility.result_schema = grade_schema;
  set_visibility.changes_state = true;
  set_visibility.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const auto document = editor->pipeline_document();
    if (!document) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                                     QStringLiteral("The editor document is not available."));
      return;
    }
    const QString id_text = params.value("node_id").toString();
    const NodeId  id{id_text.toStdString()};
    const auto*   node = document->Graph().FindNode(id);
    if (node == nullptr) {
      const QString reason =
          QStringLiteral("unknown node id: %1; nodes.list lists the Color Grades").arg(id_text);
      SendAutomationParamError(reply, QStringLiteral("/node_id"), reason);
      return;
    }
    if (dynamic_cast<const ColorGradeNodeModel*>(node) == nullptr) {
      SendAutomationParamError(
          reply, QStringLiteral("/node_id"),
          QStringLiteral("%1 is not a Color Grade; only a Color Grade can be shown or hidden")
              .arg(id_text));
      return;
    }
    const bool visible = params.value("visible").toBool();
    const auto current = ColorGradeEntry(*document, id);
    if (current->value("hidden").toBool() == !visible) {
      // The Color Grade already has the requested visibility: no commit.
      reply.SendResult(*current);
      return;
    }
    QPointer<AutomationCommandWait> wait      = new AutomationCommandWait(std::move(reply), host);
    auto                            operation = OperationWait::Begin(
        editor, wait, [wait, editor, id](std::optional<OperationFailure> failure) {
          SendVisibilityResult(wait, editor, id, failure);
        });
    const auto result = editor->RequestSetColorGradeEnabled(id, visible);
    if (EditorSessionResultIsFailure(result.kind)) {
      const QString reason = result.message.empty()
                                       ? QStringLiteral("The editor refused the visibility change.")
                                       : QString::fromStdString(result.message);
      operation->Fail(OperationFailure{AutomationErrorCode::Rejected, reason});
      return;
    }
    operation->Start(result.operation_id);
  };
  RegisterOrFail(registry, std::move(set_visibility), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
