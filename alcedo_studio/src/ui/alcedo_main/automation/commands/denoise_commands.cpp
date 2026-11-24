//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// denoise.* commands (AI Denoise integration plan, DN13): the switch of the open photo through
// the owner of the editor switch.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <optional>
#include <string>
#include <utility>

#include "app/editor_adjustment_context.hpp"
#include "app/editor_history_types.hpp"
#include "app/editor_pipeline_command_service.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "edit/history/edit_commit_attribution.hpp"
#include "edit/input/ai_denoise_error.hpp"
#include "json.hpp"
#include "ui/alcedo_main/album_backend/editor_ai_denoise_control.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_denoise_commands.hpp"
#include "ui/alcedo_main/automation/automation_editor_operation.hpp"

namespace alcedo::automation {
namespace {

using ui::EditorAiDenoiseControl;

constexpr auto kRawDecodeField = "raw_decode";

auto           RefusalToken(EditorAiDenoiseControl::TurnOnRefusal refusal) -> QString {
  switch (refusal) {
    case EditorAiDenoiseControl::TurnOnRefusal::PlatformUnavailable:
      return QStringLiteral("platform_unavailable");
    case EditorAiDenoiseControl::TurnOnRefusal::ModelNotInstalled:
      return QStringLiteral("model_not_installed");
    case EditorAiDenoiseControl::TurnOnRefusal::ModelUnreadable:
      return QStringLiteral("model_unreadable");
  }
  return QStringLiteral("model_unreadable");
}

auto HeadValue(const ui::EditorSessionController& editor) -> QJsonValue {
  const auto history = editor.history_snapshot();
  if (!history.active_head.has_value()) {
    return QJsonValue(QJsonValue::Null);
  }
  return QString::fromStdString(history.active_head->ToString());
}

/// The Develop target of the `raw_decode` field in the open document; nothing when the editor
/// cannot edit now or the document has no Develop node.
auto RawDecodeTarget(const ui::EditorSessionController& editor, std::string* error)
    -> std::optional<EditorParameterTarget> {
  const auto document = editor.pipeline_document();
  if (!editor.can_edit() || !document) {
    *error = "the editor cannot edit now";
    return std::nullopt;
  }
  return FieldOwnerParameterTarget(*document, editor.selected_node_id(), kRawDecodeField, error);
}

/// The model version that the switch of the open photo records, or nothing while it is off.
/// Read from the Develop JSON of the current document, where `ai_denoise` is present only while
/// the switch is on.
auto RecordedModelVersion(const ui::EditorSessionController& editor) -> std::optional<QString> {
  const auto document = editor.pipeline_document();
  if (!document) {
    return std::nullopt;
  }
  std::string error;
  const auto  target =
      FieldOwnerParameterTarget(*document, editor.selected_node_id(), kRawDecodeField, &error);
  nlohmann::json json;
  if (!target.has_value() || !ReadEditorParameterJson(*document, *target, &json, &error)) {
    return std::nullopt;
  }
  const nlohmann::json& raw = json.is_object() && json.contains("raw") && json.at("raw").is_object()
                                  ? json.at("raw")
                                  : json;
  const auto            found = raw.find("ai_denoise");
  if (found == raw.end() || !found->is_object()) {
    return std::nullopt;
  }
  const auto version = found->find("model_version");
  if (version == found->end() || !version->is_string()) {
    return std::nullopt;
  }
  return QString::fromStdString(version->get<std::string>());
}

/// The switch of the open photo: its ids, the sensor layout, the switch and its recorded model
/// version, and the AI Denoise error of the last frame.
auto PhotoJson(const ui::EditorSessionController& editor, const EditorAiDenoiseControl& control)
    -> QJsonObject {
  const auto                   version = RecordedModelVersion(editor);
  // Read from the session, which sets the error with the render result. The `aiDenoiseError`
  // property of the controller follows at the next change notification, after the frame.
  const IEditorSessionBackend* backend = editor.session_backend();
  const auto error = backend != nullptr ? backend->ai_denoise_error() : std::nullopt;
  QJsonValue error_value(QJsonValue::Null);
  if (error.has_value()) {
    const auto kind = AiDenoiseErrorKindToken(error->kind);
    error_value =
        QJsonObject{{"kind", QString::fromUtf8(kind.data(), static_cast<qsizetype>(kind.size()))},
                    {"text", QString::fromStdString(error->text)}};
  }
  const QString sensor = control.sensor_layout();
  return QJsonObject{
      {"element_id", static_cast<double>(editor.element_id())},
      {"image_id", static_cast<double>(editor.image_id())},
      {"sensor", sensor.isEmpty() ? QStringLiteral("unknown") : sensor},
      {"enabled", version.has_value()},
      {"model_version", version.has_value() ? QJsonValue(*version) : QJsonValue(QJsonValue::Null)},
      {"error", error_value}};
}

auto ModelJson(const EditorAiDenoiseControl& control) -> QJsonObject {
  const auto model = control.ReadInstalledModel();
  if (model.refusal.has_value()) {
    return QJsonObject{{"installed", false},
                       {"version", QJsonValue(QJsonValue::Null)},
                       {"problem", RefusalToken(*model.refusal)},
                       {"reason", QString::fromStdString(model.reason)}};
  }
  return QJsonObject{{"installed", true},
                     {"version", QString::fromStdString(model.version)},
                     {"problem", QJsonValue(QJsonValue::Null)},
                     {"reason", QJsonValue(QJsonValue::Null)}};
}

auto PhotoSchema() -> QJsonObject {
  const QJsonObject text{{"type", "string"}};
  const QJsonObject integer{{"type", "integer"}};
  return AutomationObjectSchema(
      QJsonObject{
          {"element_id", integer},
          {"image_id", integer},
          {"sensor",
           QJsonObject{{"type", "string"},
                       {"enum", QJsonArray{"bayer", "xtrans", "other", "unknown"}},
                       {"description",
                        "The sensor of the RAW file. AI Denoise supports bayer only; unknown "
                        "while the file is read or when it cannot be read."}}},
          {"enabled", QJsonObject{{"type", "boolean"}}},
          {"model_version", QJsonObject{{"type", QJsonArray{"string", "null"}},
                                        {"description",
                                         "The model version that the switch recorded when it "
                                         "was turned on; null while it is off."}}},
          {"error", QJsonObject{{"type", QJsonArray{"object", "null"}},
                                {"description",
                                 "The AI Denoise error of the last frame (kind, text); null "
                                 "after a frame without one."}}}},
      QJsonArray{"element_id", "image_id", "sensor", "enabled", "model_version", "error"});
}

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

/// Sends the refusal of a switch write before any change: -32002 with `data.refusal`.
void SendRefusal(AutomationReply& reply, const QString& refusal, const QString& reason) {
  reply.SendError(AutomationErrorCode::Rejected, reason,
                  QJsonObject{{"reason", reason}, {"refusal", refusal}});
}

/// Answers a failed switch write. A committed failure (the frame of the commit failed) keeps the
/// change; its data has `head_commit`, `enabled`, and the `ai_denoise_error` of the frame.
void SendSetFailure(AutomationCommandWait* wait, ui::EditorSessionController* editor,
                    const OperationFailure& failure) {
  if (!failure.committed_) {
    wait->SendOwnerError(failure.code_, failure.message_);
    return;
  }
  QPointer<AutomationCommandWait> guarded = wait;
  AfterQueuedChanges(wait, [guarded, editor, failure] {
    if (guarded == nullptr) {
      return;
    }
    QJsonObject data = AutomationOwnerErrorData(failure.message_);
    data.insert("head_commit", HeadValue(*editor));
    data.insert("enabled", RecordedModelVersion(*editor).has_value());
    guarded->SendError(failure.code_, failure.message_, data);
  });
}

}  // namespace

auto RegisterAutomationDenoiseCommands(AutomationCommandRegistry& registry,
                                       ui::ApplicationModuleHost* host, QString* error) -> bool {
  if (host == nullptr || host->editor_ai_denoise() == nullptr) {
    if (error != nullptr) {
      *error = QStringLiteral("the denoise commands need a host with the AI Denoise switch");
    }
    return false;
  }
  bool                  ok = true;

  AutomationCommandSpec status;
  status.method      = QStringLiteral("denoise.status");
  status.description = QStringLiteral(
      "Returns whether this computer can run AI Denoise ('available': a supported device), the "
      "installed model ('installed', 'version', or 'problem' model_not_installed or "
      "model_unreadable with 'reason'), and for the photo that the editor has open ('photo', "
      "null without one): its sensor, whether AI Denoise is on, the model version that the "
      "switch recorded, and the AI Denoise error of its last frame. AI Denoise runs a neural "
      "network on the RAW mosaic in place of the demosaic of the full-size render (editor, "
      "render.preview, export); thumbnails do not show it.");
  status.params_schema = AutomationClosedParamsSchema();
  status.result_schema = AutomationObjectSchema(
      QJsonObject{
          {"available", QJsonObject{{"type", "boolean"}}},
          {"model",
           AutomationObjectSchema(
               QJsonObject{
                   {"installed", QJsonObject{{"type", "boolean"}}},
                   {"version", QJsonObject{{"type", QJsonArray{"string", "null"}}}},
                   {"problem", QJsonObject{{"type", QJsonArray{"string", "null"}},
                                           {"description",
                                            "model_not_installed or model_unreadable; null when "
                                            "the model is installed."}}},
                   {"reason", QJsonObject{{"type", QJsonArray{"string", "null"}}}}},
               QJsonArray{"installed", "version", "problem", "reason"})},
          {"photo",
           [] {
             QJsonObject schema = PhotoSchema();
             schema.insert("type", QJsonArray{"object", "null"});
             return schema;
           }()}},
      QJsonArray{"available", "model", "photo"});
  status.handler = [host](const QJsonObject&, AutomationReply reply) {
    const EditorAiDenoiseControl&      control = *host->editor_ai_denoise();
    const ui::EditorSessionController* editor  = host->editor_session();
    const bool                         open    = editor != nullptr && editor->has_image();
    reply.SendResult(QJsonObject{
        {"available", control.available()},
        {"model", ModelJson(control)},
        {"photo", open ? QJsonValue(PhotoJson(*editor, control)) : QJsonValue(QJsonValue::Null)}});
  };
  RegisterOrFail(registry, std::move(status), &ok, error);

  AutomationCommandSpec set;
  set.method      = QStringLiteral("denoise.set");
  set.description = QStringLiteral(
      "Turns AI Denoise on or off for the photo that the editor has open, as the switch of the "
      "RAW Decode panel does: one history commit of 'raw_decode' with the author agent and the "
      "optional 'note'; on records the installed model version. Answers after the commit and "
      "its rendered frame. The frame of a full-size render runs the model, so it takes longer. "
      "Turning on is refused (-32002, data.refusal platform_unavailable, model_not_installed, "
      "model_unreadable, or sensor_not_supported) before any change. When the frame fails, the "
      "commit stays and the error data has 'head_commit', 'enabled', and 'ai_denoise_error' "
      "(kind, text); turn AI Denoise off again with enabled false. For several photos, copy the "
      "raw_decode item with adjustments.copy and paste it.");
  set.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"enabled",
                   QJsonObject{{"type", "boolean"}, {"description", "true turns AI Denoise on."}}},
                  {"note", QJsonObject{{"type", "string"},
                                       {"description", "Note of the history commit."}}}},
      QJsonArray{"enabled"});
  set.result_schema = AutomationObjectSchema(
      QJsonObject{{"photo", PhotoSchema()},
                  {"head_commit", QJsonObject{{"type", QJsonArray{"string", "null"}}}}},
      QJsonArray{"photo", "head_commit"});
  set.changes_state = true;
  set.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = host->editor_session();
    if (editor == nullptr || !editor->has_image()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                                     QStringLiteral("No editor image is open."));
      return;
    }
    const EditorAiDenoiseControl& control = *host->editor_ai_denoise();
    EditorParameterWrite          write   = EditorAiDenoiseControl::TurnOffWrite();
    if (params.value("enabled").toBool()) {
      // The switch of the panel is disabled for a known sensor that is not Bayer (D14).
      const QString sensor = control.sensor_layout();
      if (!sensor.isEmpty() && sensor != QStringLiteral("bayer")) {
        SendRefusal(reply, QStringLiteral("sensor_not_supported"),
                          QStringLiteral("AI Denoise supports Bayer sensors only; this photo has the "
                                               "sensor %1")
                              .arg(sensor));
        return;
      }
      auto preparation = control.PrepareTurnOn();
      if (preparation.refusal.has_value()) {
        SendRefusal(reply, RefusalToken(*preparation.refusal),
                          QString::fromStdString(preparation.reason));
        return;
      }
      write = std::move(*preparation.write);
    }
    std::string reason;
    auto        target = RawDecodeTarget(*editor, &reason);
    if (!target.has_value()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Rejected,
                                     QString::fromStdString(reason));
      return;
    }

    QPointer<AutomationCommandWait> wait      = new AutomationCommandWait(std::move(reply), host);
    auto                            operation = OperationWait::Begin(
        editor, wait, [wait, editor, control = &control](std::optional<OperationFailure> failure) {
          if (wait == nullptr) {
            return;
          }
          if (failure.has_value()) {
            SendSetFailure(wait, editor, *failure);
            return;
          }
          AfterQueuedChanges(wait, [wait, editor, control] {
            if (wait != nullptr) {
              wait->SendResult(QJsonObject{{"photo", PhotoJson(*editor, *control)},
                                                                            {"head_commit", HeadValue(*editor)}});
            }
          });
        });
    const auto result = editor->EnqueueTargetedFieldWrite(
        std::move(*target), std::move(write), /*settled=*/true,
        EditCommitAttribution{std::string(kEditCommitAuthorAgent),
                              params.value("note").toString().toStdString()});
    if (EditorSessionResultIsFailure(result.kind)) {
      operation->Fail(OperationFailure{AutomationErrorCode::Rejected,
                                       result.message.empty()
                                                 ? QStringLiteral("The editor cannot edit now.")
                                                 : QString::fromStdString(result.message)});
      return;
    }
    operation->Start(result.operation_id);
  };
  RegisterOrFail(registry, std::move(set), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
