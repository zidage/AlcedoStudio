//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_agent_paste.hpp"

#include <QJsonArray>
#include <QJsonValue>
#include <QPointer>
#include <QVariant>
#include <algorithm>
#include <string>
#include <utility>

#include "app/editor_session_types.hpp"
#include "app/version_name_rules.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/library_selection.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_editor_operation.hpp"

namespace alcedo::automation {
namespace {

using Package = std::shared_ptr<const AdjustmentTransferPackage>;

/// The answer of a paste after the paste into the editor photo, when there is one.
struct PasteAnswer {
  QString     task_id;
  int         target_count = 0;
  QJsonObject editor;
};

auto PasteAnswerJson(const PasteAnswer& answer) -> QJsonObject {
  QJsonObject json{{"task_id", answer.task_id.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                        : QJsonValue(answer.task_id)},
                   {"target_count", answer.target_count}};
  if (!answer.editor.isEmpty()) {
    json.insert("editor", answer.editor);
  }
  return json;
}

/// Starts the background Paste of @p package on @p ids and answers @p wait with its task id.
/// Without ids, answers at once. Agent control is checked again first, because the paste into
/// the editor photo ran before.
void StartLibraryPaste(AutomationCommandRegistry* registry, ui::ApplicationModuleHost* host,
                       AutomationCommandWait* wait, const Package& package,
                       std::vector<sl_element_id_t> ids, const QString& version_name,
                       const EditCommitAttribution& attribution, PasteAnswer answer) {
  if (ids.empty()) {
    if (!answer.editor.isEmpty() && !answer.editor.value("pasted").toBool()) {
      wait->SendOwnerError(AutomationErrorCode::Rejected,
                           answer.editor.value("message").toString());
      return;
    }
    wait->SendResult(PasteAnswerJson(answer));
    return;
  }
  if (const auto refusal = registry->AdmitStateChange()) {
    wait->SendError(refusal->code, refusal->message, refusal->data);
    return;
  }
  ui::AdjustmentTransferApplyCoordinator* coordinator =
      host->adjustment_transfer()->apply_coordinator();
  QString error;
  if (!coordinator->StartApplyToTargets(*package, std::move(ids), version_name, &error,
                                        attribution)) {
    wait->SendOwnerError(AutomationErrorCode::Busy, error);
    return;
  }
  answer.task_id = coordinator->running_task_id();
  wait->SendResult(PasteAnswerJson(answer));
}

/// The ids of the photos selected in the library, or nothing after an error answer.
auto SelectedPhotoIds(ui::ApplicationModuleHost* host, bool selection_available,
                      AutomationReply& reply) -> std::optional<QJsonArray> {
  if (!selection_available || host->library() == nullptr ||
      host->library()->selection() == nullptr) {
    SendAutomationOwnerError(
        reply, AutomationErrorCode::NotReady,
        QStringLiteral("The headless host has no library selection; give element_ids."));
    return std::nullopt;
  }
  QJsonArray ids;
  for (const QVariant& value : host->library()->selection()->SelectedItems()) {
    ids.push_back(static_cast<double>(value.toMap().value(QStringLiteral("elementId")).toUInt()));
  }
  if (ids.isEmpty()) {
    SendAutomationParamError(reply, QStringLiteral("/element_ids"),
                             QStringLiteral("no photo is selected; give element_ids"));
    return std::nullopt;
  }
  return ids;
}

}  // namespace

auto AgentPasteTargets(ui::ApplicationModuleHost* host, ProjectService& project,
                       const QJsonObject& params, bool selection_available, AutomationReply& reply)
    -> std::optional<std::vector<sl_element_id_t>> {
  QJsonObject target_params{{"element_ids", params.value("element_ids")}};
  if (!params.contains("element_ids")) {
    const auto selected = SelectedPhotoIds(host, selection_available, reply);
    if (!selected.has_value()) {
      return std::nullopt;
    }
    target_params.insert("element_ids", *selected);
  }
  const auto rows = ResolveAutomationElementRows(project, target_params, reply);
  if (!rows.has_value()) {
    return std::nullopt;
  }
  std::vector<sl_element_id_t> ids;
  ids.reserve(rows->size());
  for (const auto& row : *rows) {
    ids.push_back(row.file_id_);
  }
  return ids;
}

auto AgentPasteVersionName(const QJsonObject& params, const QString& default_name,
                           AutomationReply& reply) -> std::optional<QString> {
  if (!params.contains("version_name")) {
    return default_name;
  }
  std::string reason;
  const auto  name =
      NormalizeVersionName(params.value("version_name").toString().toStdString(), &reason);
  if (!name.has_value()) {
    SendAutomationParamError(reply, QStringLiteral("/version_name"),
                             QString::fromStdString(reason));
    return std::nullopt;
  }
  return QString::fromStdString(*name);
}

auto AgentPasteVersionNameSchema() -> QJsonObject {
  return QJsonObject{
      {"type", "string"},
      {"description", "Name of the new Versions. A name that a photo already has gets a number."}};
}

auto AgentPasteNoteSchema() -> QJsonObject {
  return QJsonObject{{"type", "string"},
                     {"description", "Why the agent pastes; stored with each commit."}};
}

auto AgentPasteResultSchema() -> QJsonObject {
  const QJsonObject text_schema{{"type", "string"}};
  return AutomationObjectSchema(
      QJsonObject{
          {"task_id", QJsonObject{{"type", QJsonArray{"string", "null"}},
                                  {"description",
                                   "The background Paste task; null when only the editor photo "
                                   "was a target."}}},
          {"target_count", QJsonObject{{"type", "integer"}}},
          {"editor",
           AutomationObjectSchema(QJsonObject{{"element_id", QJsonObject{{"type", "integer"}}},
                                              {"pasted", QJsonObject{{"type", "boolean"}}},
                                              {"version_id", text_schema},
                                              {"message", text_schema}},
                                  QJsonArray{"element_id", "pasted"})}},
      QJsonArray{"task_id", "target_count"});
}

void StartAgentPaste(AutomationCommandRegistry* registry, ui::ApplicationModuleHost* host,
                     AutomationReply reply, Package package, std::vector<sl_element_id_t> ids,
                     const QString& version_name, const EditCommitAttribution& attribution) {
  ui::InteractionPolicyController*  policy   = host->interaction_policy();
  ui::AdjustmentTransferController* transfer = host->adjustment_transfer();
  if (transfer->apply_coordinator()->running() ||
      (policy != nullptr && !policy->CanPasteAdjustments())) {
    const QString reason = policy != nullptr && !policy->PasteAdjustmentsReason().isEmpty()
                               ? policy->PasteAdjustmentsReason()
                               : QStringLiteral("Adjustments are already being pasted.");
    SendAutomationOwnerError(reply, AutomationErrorCode::Busy, reason);
    return;
  }
  PasteAnswer answer;
  answer.target_count                 = static_cast<int>(ids.size());

  // The editor session owns the history of the photo it has open, so that photo pastes
  // through the session queue, like Paste in the GUI. The others paste in the background.
  ui::EditorSessionController* editor = host->editor_session();
  const auto                   editor_target =
      editor != nullptr && editor->has_image()
                            ? std::find(ids.begin(), ids.end(), static_cast<sl_element_id_t>(editor->element_id()))
                            : ids.end();
  auto* wait = new AutomationCommandWait(std::move(reply), host);
  if (editor_target == ids.end()) {
    StartLibraryPaste(registry, host, wait, package, std::move(ids), version_name, attribution,
                      std::move(answer));
    return;
  }
  const auto editor_id = *editor_target;
  ids.erase(editor_target);

  QPointer<AutomationCommandWait> guarded   = wait;
  auto                            operation = OperationWait::Begin(
      editor, wait,
      [guarded, host, registry, editor, editor_id, package, ids, version_name, attribution,
       answer](std::optional<OperationFailure> failure) mutable {
        if (guarded == nullptr) {
          return;
        }
        answer.editor = QJsonObject{{"element_id", static_cast<double>(editor_id)},
                                                               {"pasted", !failure.has_value()}};
        if (failure.has_value()) {
          answer.editor.insert("message", failure->message_);
          StartLibraryPaste(registry, host, guarded, package, std::move(ids), version_name,
                                                       attribution, std::move(answer));
          return;
        }
        AfterQueuedChanges(guarded, [guarded, host, registry, editor, package, ids, version_name,
                                     attribution, answer]() mutable {
          if (guarded == nullptr) {
            return;
          }
          // Paste checks out the new Version, so the active Version is the pasted one.
          answer.editor.insert(
              "version_id",
              QString::fromStdString(editor->history_snapshot().active_version_id.ToString()));
          StartLibraryPaste(registry, host, guarded, package, std::move(ids), version_name,
                                                       attribution, std::move(answer));
        });
      },
      OperationEnd::AfterSaveCheckpoint);
  const auto result = editor->RequestPasteAdjustmentPackage(*package, version_name, attribution);
  if (EditorSessionResultIsFailure(result.kind)) {
    operation->Fail(OperationFailure{AutomationErrorCode::Rejected,
                                     result.message.empty()
                                         ? QStringLiteral("The editor cannot paste now.")
                                         : QString::fromStdString(result.message)});
    return;
  }
  operation->Start(result.operation_id);
}

}  // namespace alcedo::automation
