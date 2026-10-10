//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QVariant>
#include <QVariantMap>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/album_browse_service.hpp"
#include "app/editor_action_policy.hpp"
#include "app/editor_adjustment_context.hpp"
#include "app/editor_history_types.hpp"
#include "app/editor_panel_projection.hpp"
#include "app/editor_parameter_catalog.hpp"
#include "app/editor_parameter_write.hpp"
#include "app/editor_session_service.hpp"
#include "app/editor_session_types.hpp"
#include "json.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "ui/alcedo_main/album_backend/editor_panel_presentation.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"

namespace alcedo::automation {
namespace {

/// Most changes of one editor.batch_set call.
constexpr int kMaxBatchChanges = 64;

auto OptionSchema() -> QJsonObject {
  const QJsonObject text{{"type", "string"}};
  return AutomationObjectSchema(QJsonObject{{"value", text}, {"label", text}},
                                QJsonArray{"value", "label"});
}

auto SliderSchema() -> QJsonObject {
  const QJsonObject integer{{"type", "integer"}};
  return AutomationObjectSchema(
      QJsonObject{{"scale", QJsonObject{{"type", "string"}, {"enum", QJsonArray{"kelvin_pivot"}}}},
                  {"position_min", integer},
                  {"position_max", integer},
                  {"pivot_position", integer},
                  {"pivot_kelvin", QJsonObject{{"type", "number"}}}},
      QJsonArray{"scale", "position_min", "position_max", "pivot_position", "pivot_kelvin"});
}

auto PropertySchema() -> QJsonObject {
  const QJsonObject number{{"type", "number"}};
  return AutomationObjectSchema(
      QJsonObject{{"name", QJsonObject{{"type", "string"}}},
                  {"type", QJsonObject{{"type", "string"},
                                       {"enum", QJsonArray{"number", "boolean", "string",
                                                           "option", "number_list"}}}},
                  {"ui_min", number},
                  {"ui_max", number},
                  {"ui_step", number},
                  {"ui_decimals", QJsonObject{{"type", "integer"}}},
                  {"ui_count", QJsonObject{{"type", "integer"}}},
                  {"ui_slider", SliderSchema()},
                  {"options", QJsonObject{{"type", "array"}, {"items", OptionSchema()}}}},
      QJsonArray{"name", "type"});
}

auto EotfBySpaceSchema() -> QJsonObject {
  const QJsonObject values{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}};
  return AutomationObjectSchema(
      QJsonObject{{"rec709", values}, {"p3_d65", values}, {"rec2020", values}},
      QJsonArray{"rec709", "p3_d65", "rec2020"});
}

auto CatalogEntrySchema() -> QJsonObject {
  const QJsonObject number{{"type", "number"}};
  return AutomationObjectSchema(
      QJsonObject{
          {"field", QJsonObject{{"type", "string"}}},
          {"kind",
           QJsonObject{{"type", "string"}, {"enum", QJsonArray{"scalar", "object", "model"}}}},
          {"panel", QJsonObject{{"type", "string"}}},
          {"ui_min", number},
          {"ui_max", number},
          {"ui_default", QJsonObject{{"type", QJsonArray{"number", "object"}}}},
          {"ui_step", number},
          {"ui_decimals", QJsonObject{{"type", "integer"}}},
          {"properties", QJsonObject{{"type", "array"}, {"items", PropertySchema()}}},
          {"model_shape", QJsonObject{{"type", "string"}}},
          {"encoding_eotf_by_space", EotfBySpaceSchema()},
          {"aliases",
           QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "string"}}}}}},
      QJsonArray{"field", "kind", "panel", "ui_default"});
}

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

auto ToModelValue(const QJsonValue& value) -> nlohmann::json {
  const QByteArray text = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
  return nlohmann::json::parse(text.toStdString()).at(0);
}

auto ToJsonValue(const nlohmann::json& value) -> QJsonValue {
  const std::string text = nlohmann::json::array({value}).dump();
  return QJsonDocument::fromJson(QByteArray::fromStdString(text)).array().at(0);
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

/// The panel values of the open image, read from its current document like the panel shows them:
/// the fields of the selected node when one is selected, otherwise the fields of the current
/// panel nodes. Empty when the document cannot be read.
auto CurrentPanelValues(const ui::EditorSessionController& editor) -> QVariantMap {
  const auto document = editor.pipeline_document();
  if (!document) {
    return {};
  }
  EditorPanelProjection projection;
  std::string           error;
  const NodeId          selected = editor.selected_node_id();
  const bool read = selected.Empty() ? ProjectCurrentPanelFields(*document, 0, &projection, &error)
                                     : ProjectSelectedNodePanelFields(*document, selected, 0,
                                                                      &projection, &error);
  if (!read) {
    return {};
  }
  return ui::PanelProjectionToVariantMap(projection);
}

/// The value of @p field in @p values, as the catalog reads it; null when the open image does
/// not show the field (for example a Color Grade field without a Color Grade).
auto CurrentModelValue(const QVariantMap& values, std::string_view field) -> nlohmann::json {
  const QVariant value = values.value(Text(field));
  if (!value.isValid()) {
    return nullptr;
  }
  return ToModelValue(QJsonValue::fromVariant(value));
}

/// The UI value of @p entry in @p values; nothing when the image does not show the field.
auto CurrentUiValue(const QVariantMap& values, const EditorParameterCatalogEntry& entry)
    -> std::optional<QJsonValue> {
  const auto model = CurrentModelValue(values, entry.field);
  if (model.is_null()) {
    return std::nullopt;
  }
  std::string error;
  const auto  ui = EditorParameterCatalog::ToUiValue(entry.field, model, &error);
  if (!ui.has_value()) {
    return std::nullopt;
  }
  return ToJsonValue(*ui);
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

auto EditorIdentity(const ui::EditorSessionController& editor) -> QJsonObject {
  return QJsonObject{{"element_id", static_cast<double>(editor.element_id())},
                     {"image_id", static_cast<double>(editor.image_id())},
                     {"state", QString::fromLatin1(EditorSessionStateName(editor.session_state()))},
                     {"head_commit", HeadValue(editor.history_snapshot())}};
}

/// A failed session command: the error code of the response and the owner message.
struct OperationFailure {
  AutomationErrorCode code_ = AutomationErrorCode::Failed;
  QString             message_;
};

using OperationDone = std::function<void(std::optional<OperationFailure>)>;

/**
 * @brief Waits for the results of one session command.
 *
 * The command is done after its terminal result. When one of its results routed a render, it is
 * done after a presented frame of that render request or a later one (a later request renders the
 * later document, which includes the change). A Rejected or Failed result of the command fails
 * it. Results that arrive before the command id is known are kept and checked once it is.
 */
class OperationWait final {
 public:
  /// Connects to @p editor with @p context as the connection context. Call Start with the id
  /// that the submission returned.
  static auto Begin(ui::EditorSessionController* editor, QObject* context, OperationDone done)
      -> std::shared_ptr<OperationWait> {
    auto wait         = std::shared_ptr<OperationWait>(new OperationWait(std::move(done)));
    wait->self_       = wait;
    wait->connection_ = QObject::connect(
        editor, &ui::EditorSessionController::SessionResultObserved, context,
        [weak = std::weak_ptr<OperationWait>(wait)](const EditorSessionResult& result) {
          if (auto locked = weak.lock()) {
            locked->Observe(result);
          }
        });
    // A wait whose reply timed out never finishes; it ends with its context.
    QObject::connect(context, &QObject::destroyed, [weak = std::weak_ptr<OperationWait>(wait)] {
      if (auto locked = weak.lock()) {
        locked->self_.reset();
      }
    });
    return wait;
  }

  void Start(std::uint64_t operation_id) {
    if (finished_) {
      return;
    }
    if (operation_id == 0) {
      Finish(OperationFailure{AutomationErrorCode::Failed,
                              QStringLiteral("The editor did not assign a command id.")});
      return;
    }
    operation_id_ = operation_id;
    auto early    = std::exchange(early_, {});
    for (const auto& result : early) {
      Observe(result);
    }
  }

  void Fail(OperationFailure failure) { Finish(std::move(failure)); }

 private:
  explicit OperationWait(OperationDone done) : done_(std::move(done)) {}

  void Observe(const EditorSessionResult& result) {
    if (finished_) {
      return;
    }
    if (operation_id_ == 0) {
      early_.push_back(result);
      return;
    }
    if (result.kind == EditorSessionResultKind::FramePresented) {
      if (render_request_id_ != 0 && result.render_request_id >= render_request_id_) {
        Finish(std::nullopt);
      }
      return;
    }
    if (result.operation_id != operation_id_) {
      return;
    }
    if (EditorSessionResultIsFailure(result.kind)) {
      Finish(OperationFailure{result.kind == EditorSessionResultKind::Rejected
                                  ? AutomationErrorCode::Rejected
                                  : AutomationErrorCode::Failed,
                              QString::fromStdString(result.message)});
      return;
    }
    if (result.render_request_id != 0) {
      render_request_id_ = result.render_request_id;
      return;
    }
    if (render_request_id_ == 0 && EditorSessionResultIsTerminal(result.kind)) {
      Finish(std::nullopt);
    }
  }

  void Finish(std::optional<OperationFailure> failure) {
    if (finished_) {
      return;
    }
    finished_ = true;
    QObject::disconnect(connection_);
    auto done = std::move(done_);
    auto self = std::move(self_);
    if (done) {
      done(std::move(failure));
    }
  }

  OperationDone                    done_;
  QMetaObject::Connection          connection_;
  std::vector<EditorSessionResult> early_;
  std::uint64_t                    operation_id_      = 0;
  std::uint64_t                    render_request_id_ = 0;
  bool                             finished_          = false;
  /// Keeps the wait alive until it finishes; the connection holds only a weak reference.
  std::shared_ptr<OperationWait>   self_;
};

/// Runs @p task on the event loop of @p context after the change notifications that are queued
/// now: the session publishes a result before the change that updates the snapshots.
void AfterQueuedChanges(QObject* context, std::function<void()> task) {
  QMetaObject::invokeMethod(context, std::move(task), Qt::QueuedConnection);
}

/// A field write that the catalog accepted: the canonical field and its typed write.
struct PreparedWrite {
  std::string          field_;
  EditorParameterWrite write_;
};

/// Converts the UI value @p value of @p field to one typed write. Writes the reason to @p error.
auto PrepareWrite(const ui::EditorSessionController& editor, const QString& field,
                  const QJsonValue& value, QString* error) -> std::optional<PreparedWrite> {
  const auto* entry = EditorParameterCatalog::Find(field.toStdString());
  if (entry == nullptr) {
    *error = QStringLiteral("unknown editor field: %1; editor.catalog lists the fields").arg(field);
    return std::nullopt;
  }
  std::string reason;
  const auto  model = EditorParameterCatalog::ToModelJson(
      entry->field, ToModelValue(value),
      CurrentModelValue(CurrentPanelValues(editor), entry->field), &reason);
  if (!model.has_value()) {
    *error = QString::fromStdString(reason);
    return std::nullopt;
  }
  auto write = ParseEditorParameterWrite(entry->field, *model, &reason);
  if (!write.has_value()) {
    *error = QString::fromStdString(reason);
    return std::nullopt;
  }
  return PreparedWrite{std::string(entry->field), std::move(*write)};
}

/// Enqueues @p prepared as a settled write and calls @p done after its commit and frame.
void ApplyWrite(ui::EditorSessionController* editor, QObject* context, PreparedWrite prepared,
                OperationDone done) {
  auto       wait   = OperationWait::Begin(editor, context, std::move(done));
  const auto result = editor->EnqueueFieldWrite(QString::fromStdString(prepared.field_),
                                                std::move(prepared.write_), /*settled=*/true);
  if (EditorSessionResultIsFailure(result.kind)) {
    wait->Fail(OperationFailure{AutomationErrorCode::Rejected,
                                result.message.empty()
                                    ? QStringLiteral("The editor cannot edit now.")
                                    : QString::fromStdString(result.message)});
    return;
  }
  wait->Start(result.operation_id);
}

/// Answers an undo or redo after its command and frame.
void SendHistoryMove(ui::EditorSessionController* editor, AutomationCommandWait* reply_wait,
                     bool undo) {
  QPointer<AutomationCommandWait> wait      = reply_wait;
  auto                            operation = OperationWait::Begin(
      editor, reply_wait, [wait, editor](std::optional<OperationFailure> failure) {
        if (wait == nullptr) {
          return;
        }
        if (failure.has_value()) {
          wait->SendOwnerError(failure->code_, failure->message_);
          return;
        }
        AfterQueuedChanges(wait, [wait, editor] {
          if (wait == nullptr) {
            return;
          }
          const auto history = editor->history_snapshot();
          wait->SendResult(QJsonObject{{"head_commit", HeadValue(history)},
                                                                  {"can_undo", history.can_undo},
                                                                  {"can_redo", history.can_redo}});
        });
      });
  const auto result = undo ? editor->RequestUndo() : editor->RequestRedo();
  if (EditorSessionResultIsFailure(result.kind)) {
    operation->Fail(
        OperationFailure{AutomationErrorCode::Rejected, QString::fromStdString(result.message)});
    return;
  }
  operation->Start(result.operation_id);
}

/// One editor.batch_set call: applies the changes in order, each after the previous frame.
class BatchApply final : public std::enable_shared_from_this<BatchApply> {
 public:
  BatchApply(ui::EditorSessionController* editor, AutomationCommandWait* wait, QJsonArray changes)
      : editor_(editor), wait_(wait), changes_(std::move(changes)) {}

  void Next() {
    if (wait_ == nullptr) {
      return;
    }
    if (applied_ == changes_.size()) {
      SendDone();
      return;
    }
    const QJsonObject change  = changes_.at(applied_).toObject();
    const QString     field   = change.value("field").toString();
    const QString     pointer = QStringLiteral("/changes/%1/%2")
                                .arg(applied_)
                                .arg(EditorParameterCatalog::Find(field.toStdString()) == nullptr
                                         ? QStringLiteral("field")
                                         : QStringLiteral("value"));
    QString error;
    auto    prepared = PrepareWrite(*editor_, field, change.value("value"), &error);
    if (!prepared.has_value()) {
      wait_->SendError(AutomationErrorCode::InvalidParams, error, FailureData(error, pointer));
      return;
    }
    auto self = shared_from_this();
    ApplyWrite(editor_, wait_, std::move(*prepared),
               [self](std::optional<OperationFailure> failure) { self->Finished(failure); });
  }

 private:
  void Finished(const std::optional<OperationFailure>& failure) {
    if (wait_ == nullptr) {
      return;
    }
    if (failure.has_value()) {
      wait_->SendError(failure->code_, failure->message_, FailureData(failure->message_, {}));
      return;
    }
    ++applied_;
    auto self = shared_from_this();
    AfterQueuedChanges(wait_, [self] { self->Next(); });
  }

  auto FailureData(const QString& reason, const QString& pointer) const -> QJsonObject {
    QJsonObject data{{"reason", reason},
                     {"applied", applied_},
                     {"head_commit", HeadValue(editor_->history_snapshot())}};
    if (!pointer.isEmpty()) {
      data.insert("pointer", pointer);
    }
    return data;
  }

  void SendDone() {
    const QVariantMap current = CurrentPanelValues(*editor_);
    QJsonObject       values;
    for (const auto& change : changes_) {
      const QString field = change.toObject().value("field").toString();
      if (const auto* entry = EditorParameterCatalog::Find(field.toStdString())) {
        if (const auto value = CurrentUiValue(current, *entry)) {
          values.insert(field, *value);
        }
      }
    }
    wait_->SendResult(QJsonObject{{"applied", applied_},
                                  {"head_commit", HeadValue(editor_->history_snapshot())},
                                  {"values", values}});
  }

  ui::EditorSessionController*    editor_;
  QPointer<AutomationCommandWait> wait_;
  QJsonArray                      changes_;
  qsizetype                       applied_ = 0;
};

auto IsPersistBusy(const ui::EditorSessionController& editor) -> bool {
  const auto state = editor.session_state();
  return editor.close_in_flight() || editor.persist_in_flight() ||
         state == EditorSessionState::Saving || state == EditorSessionState::Switching;
}

auto ValueSchema() -> QJsonObject {
  return QJsonObject{
      {"type", QJsonArray{"number", "object"}},
      {"description",
       "The UI value: a number for a scalar field, an object for an object or Model field. "
       "editor.catalog gives the range, the properties, and the Model shape."}};
}

}  // namespace

auto AutomationEditorCatalogFields() -> QJsonArray {
  const std::string text = EditorParameterCatalog::CatalogJson().dump();
  return QJsonDocument::fromJson(QByteArray::fromStdString(text)).array();
}

auto RegisterAutomationEditorCatalogCommand(AutomationCommandRegistry& registry, QString* error)
    -> bool {
  AutomationCommandSpec catalog;
  catalog.method      = QStringLiteral("editor.catalog");
  catalog.description = QStringLiteral(
      "Returns every editor field that editor.set accepts, in UI units: the values that the "
      "editor panels show. A scalar field has its range (ui_min, ui_max), default, step, and "
      "shown decimals. An object field lists its properties (number range, boolean, string, or "
      "option list, or a list of numbers) and its default object; a write may give some of the "
      "properties. A model field (curve, lut) takes its Model JSON, described in model_shape; a "
      "write gives the complete value. A Kelvin "
      "property also has its slider position scale (ui_slider), and odt lists the valid "
      "encoding_eotf values of each encoding_space. A value outside the range or the option "
      "list is rejected; it is not clamped.");
  catalog.params_schema = AutomationClosedParamsSchema();
  catalog.result_schema = AutomationObjectSchema(
      QJsonObject{{"fields", QJsonObject{{"type", "array"}, {"items", CatalogEntrySchema()}}}},
      QJsonArray{"fields"});
  catalog.handler = [](const QJsonObject&, AutomationReply reply) {
    reply.SendResult(QJsonObject{{"fields", AutomationEditorCatalogFields()}});
  };
  return registry.Register(std::move(catalog), error);
}

auto RegisterAutomationEditorCommands(AutomationCommandRegistry& registry,
                                      ui::ApplicationModuleHost* host, QString* error) -> bool {
  bool              ok = RegisterAutomationEditorCatalogCommand(registry, error);

  const QJsonObject id_schema{{"type", "integer"}, {"minimum", 1}};
  const QJsonObject text_schema{{"type", "string"}};
  const QJsonObject head_schema{{"type", QJsonArray{"string", "null"}},
                                {"description", "Hex hash of the head commit; null at the root."}};
  const QJsonObject identity_schema =
      AutomationObjectSchema(QJsonObject{{"element_id", QJsonObject{{"type", "integer"}}},
                                         {"image_id", QJsonObject{{"type", "integer"}}},
                                         {"state", text_schema},
                                         {"head_commit", head_schema}},
                             QJsonArray{"element_id", "image_id", "state", "head_commit"});

  AutomationCommandSpec open;
  open.method      = QStringLiteral("editor.open");
  open.description = QStringLiteral(
      "Opens a photo in the editor, like a double-click in the library. An open image is saved "
      "and replaced. Answers when the editor can edit the photo.");
  open.params_schema = AutomationClosedParamsSchema(QJsonObject{{"element_id", id_schema}},
                                                    QJsonArray{"element_id"});
  open.result_schema = identity_schema;
  open.changes_state = true;
  open.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::ProjectModule* project = host->project();
    const auto         service = project->handler().project();
    if (!service || !project->ProjectEntered() || project->ProjectLoading()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                                     QStringLiteral("No project is open."));
      return;
    }
    const auto element_id = static_cast<sl_element_id_t>(params.value("element_id").toDouble());
    const auto browse     = service->GetAlbumBrowseService();
    std::vector<SearchResultRow> rows;
    try {
      if (browse) {
        const std::array<sl_element_id_t, 1> ids{element_id};
        rows = browse->ReadAlbumFileRows(ids);
      }
    } catch (const std::exception& e) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Failed, QString::fromUtf8(e.what()));
      return;
    }
    if (rows.empty() || rows.front().image_id_ == 0) {
      SendAutomationParamError(reply, QStringLiteral("/element_id"),
                                     QStringLiteral("unknown element id: %1").arg(element_id));
      return;
    }
    const auto                      image_id = rows.front().image_id_;
    ui::EditorSessionController*    editor   = host->editor_session();
    QPointer<AutomationCommandWait> wait     = new AutomationCommandWait(std::move(reply), host);
    const auto                      evaluate = [wait, editor, element_id, image_id] {
      if (wait == nullptr) {
        return;
      }
      const auto* backend = editor->session_backend();
      if (backend == nullptr) {
        wait->SendOwnerError(AutomationErrorCode::Failed,
                                                        QStringLiteral("The editor session is not available."));
        return;
      }
      const auto state    = backend->state();
      const auto identity = backend->identity();
      if (identity.element_id == element_id && identity.image_id == image_id &&
          state == EditorSessionState::Interactive && editor->can_edit()) {
        wait->SendResult(EditorIdentity(*editor));
        return;
      }
      if (state == EditorSessionState::Failed ||
          state == EditorSessionState::RetainedImageFailure) {
        wait->SendOwnerError(AutomationErrorCode::Failed, editor->last_error());
      }
    };
    QObject::connect(editor, &ui::EditorSessionController::StateChanged, wait, evaluate);
    QObject::connect(editor, &ui::EditorSessionController::ActionAvailabilityChanged, wait,
                           evaluate);
    host->workspace_router()->OpenEditor(static_cast<uint>(element_id),
                                               static_cast<uint>(image_id));
    AfterQueuedChanges(wait, evaluate);
  };
  RegisterOrFail(registry, std::move(open), &ok, error);

  AutomationCommandSpec close;
  close.method      = QStringLiteral("editor.close");
  close.description = QStringLiteral(
      "Closes the editor image and returns to the library. With persist (the default) the edits "
      "are saved first; without it, edits that are not saved are discarded. Answers when the "
      "image is closed. A failed save keeps the image open and answers with the reason.");
  close.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"persist", QJsonObject{{"type", "boolean"}, {"default", true}}}});
  close.result_schema = AutomationObjectSchema(
      QJsonObject{{"closed", QJsonObject{{"type", "boolean"}}}}, QJsonArray{"closed"});
  close.changes_state = true;
  close.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor  = host->editor_session();
    const bool                   persist = params.value("persist").toBool(true);
    if (editor == nullptr ||
        (!editor->active() && !editor->has_image() && !IsPersistBusy(*editor) &&
         editor->session_state() == EditorSessionState::NoImage)) {
      reply.SendResult(QJsonObject{{"closed", true}});
      return;
    }
    QPointer<AutomationCommandWait> wait     = new AutomationCommandWait(std::move(reply), host);
    // The same evaluation as the project close: wait while a save runs, then the image must
    // be gone; a recovery state or a refused close keeps the image and reports why.
    const auto                      evaluate = [wait, editor] {
      if (wait == nullptr || IsPersistBusy(*editor)) {
        return;
      }
      const auto state = editor->session_state();
      if (editor->has_pending_recovery() || state == EditorSessionState::RetainedImageFailure ||
          state == EditorSessionState::Failed) {
        wait->SendOwnerError(AutomationErrorCode::Failed, editor->last_error());
        return;
      }
      if (state == EditorSessionState::NoImage || state == EditorSessionState::ShuttingDown) {
        wait->SendResult(QJsonObject{{"closed", true}});
        return;
      }
      wait->SendOwnerError(AutomationErrorCode::Rejected,
                           editor->last_error().isEmpty()
                                                          ? QStringLiteral("The editor did not close the image.")
                                                          : editor->last_error());
    };
    QObject::connect(editor, &ui::EditorSessionController::StateChanged, wait, evaluate,
                           Qt::QueuedConnection);
    editor->Finalize(persist);
    host->workspace_router()->OpenLibrary();
    AfterQueuedChanges(wait, evaluate);
  };
  RegisterOrFail(registry, std::move(close), &ok, error);

  AutomationCommandSpec get;
  get.method      = QStringLiteral("editor.get");
  get.description = QStringLiteral(
      "Returns the UI values of the open image: the values that the editor panels show, in the "
      "units of editor.catalog. Without 'fields', every field that the image shows.");
  get.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"fields", QJsonObject{{"type", "array"}, {"items", text_schema}}}});
  get.result_schema = AutomationObjectSchema(
      QJsonObject{{"element_id", QJsonObject{{"type", "integer"}}},
                  {"image_id", QJsonObject{{"type", "integer"}}},
                  {"state", text_schema},
                  {"head_commit", head_schema},
                  {"values", QJsonObject{{"type", "object"}}}},
      QJsonArray{"element_id", "image_id", "state", "head_commit", "values"});
  get.handler = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const QVariantMap current = CurrentPanelValues(*editor);
    QJsonObject       values;
    if (params.contains("fields")) {
      const QJsonArray fields = params.value("fields").toArray();
      for (qsizetype i = 0; i < fields.size(); ++i) {
        const QString field = fields.at(i).toString();
        const auto*   entry = EditorParameterCatalog::Find(field.toStdString());
        if (entry == nullptr) {
          SendAutomationParamError(
              reply, QStringLiteral("/fields/%1").arg(i),
              QStringLiteral("unknown editor field: %1; editor.catalog lists the fields")
                  .arg(field));
          return;
        }
        if (const auto value = CurrentUiValue(current, *entry)) {
          values.insert(field, *value);
        }
      }
    } else {
      for (const auto& entry : EditorParameterCatalog::Entries()) {
        if (const auto value = CurrentUiValue(current, entry)) {
          values.insert(Text(entry.field), *value);
        }
      }
    }
    QJsonObject result = EditorIdentity(*editor);
    result.insert("values", values);
    reply.SendResult(result);
  };
  RegisterOrFail(registry, std::move(get), &ok, error);

  AutomationCommandSpec set;
  set.method      = QStringLiteral("editor.set");
  set.description = QStringLiteral(
      "Sets one field of the open image to a UI value, like releasing a panel control: one "
      "history commit. Answers after the commit and its rendered frame, with the value that the "
      "editor now shows (a crop can be constrained). A value equal to the current one makes no "
      "commit.");
  set.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"field", text_schema}, {"value", ValueSchema()}}, QJsonArray{"field", "value"});
  set.result_schema = AutomationObjectSchema(
      QJsonObject{{"field", text_schema},
                  {"value", QJsonObject{{"type", QJsonArray{"number", "object", "null"}}}},
                  {"head_commit", head_schema}},
      QJsonArray{"field", "value", "head_commit"});
  set.changes_state = true;
  set.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    ui::EditorSessionController* editor = host->editor_session();
    const QString                field  = params.value("field").toString();
    QString                      reason;
    auto prepared = PrepareWrite(*editor, field, params.value("value"), &reason);
    if (!prepared.has_value()) {
      SendAutomationParamError(reply,
                               EditorParameterCatalog::Find(field.toStdString()) == nullptr
                                         ? QStringLiteral("/field")
                                         : QStringLiteral("/value"),
                                     reason);
      return;
    }
    QPointer<AutomationCommandWait> wait = new AutomationCommandWait(std::move(reply), host);
    ApplyWrite(
        editor, wait, std::move(*prepared),
        [wait, editor, field](std::optional<OperationFailure> failure) {
          if (wait == nullptr) {
            return;
          }
          if (failure.has_value()) {
            wait->SendOwnerError(failure->code_, failure->message_);
            return;
          }
          AfterQueuedChanges(wait, [wait, editor, field] {
            if (wait == nullptr) {
              return;
            }
            const auto* entry = EditorParameterCatalog::Find(field.toStdString());
            const auto  value = CurrentUiValue(CurrentPanelValues(*editor), *entry);
            wait->SendResult(QJsonObject{{"field", field},
                                               {"value", value.value_or(QJsonValue(QJsonValue::Null))},
                                               {"head_commit", HeadValue(editor->history_snapshot())}});
          });
        });
  };
  RegisterOrFail(registry, std::move(set), &ok, error);

  AutomationCommandSpec batch;
  batch.method      = QStringLiteral("editor.batch_set");
  batch.description = QStringLiteral(
      "Sets several fields in order. Each change that alters a value is one history commit, "
      "applied after the frame "
      "of the previous one. The first change that fails stops the batch: the error data has "
      "'applied' (the changes before it, which stay) and 'head_commit'.");
  batch.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"changes", QJsonObject{{"type", "array"},
                                          {"minItems", 1},
                                          {"items", AutomationClosedParamsSchema(
                                                        QJsonObject{{"field", text_schema},
                                                                    {"value", ValueSchema()}},
                                                        QJsonArray{"field", "value"})}}}},
      QJsonArray{"changes"});
  batch.result_schema =
      AutomationObjectSchema(QJsonObject{{"applied", QJsonObject{{"type", "integer"}}},
                                         {"head_commit", head_schema},
                                         {"values", QJsonObject{{"type", "object"}}}},
                             QJsonArray{"applied", "head_commit", "values"});
  batch.changes_state = true;
  batch.handler       = [host](const QJsonObject& params, AutomationReply reply) {
    const QJsonArray changes = params.value("changes").toArray();
    if (changes.size() > kMaxBatchChanges) {
      SendAutomationParamError(
          reply, QStringLiteral("/changes"),
          QStringLiteral("a batch has at most %1 changes").arg(kMaxBatchChanges));
      return;
    }
    auto* wait = new AutomationCommandWait(std::move(reply), host);
    std::make_shared<BatchApply>(host->editor_session(), wait, changes)->Next();
  };
  RegisterOrFail(registry, std::move(batch), &ok, error);

  const QJsonObject move_result_schema =
      AutomationObjectSchema(QJsonObject{{"head_commit", head_schema},
                                         {"can_undo", QJsonObject{{"type", "boolean"}}},
                                         {"can_redo", QJsonObject{{"type", "boolean"}}}},
                             QJsonArray{"head_commit", "can_undo", "can_redo"});
  for (const bool undo : {true, false}) {
    AutomationCommandSpec move;
    move.method        = undo ? QStringLiteral("editor.undo") : QStringLiteral("editor.redo");
    move.description   = undo ? QStringLiteral(
                                  "Moves the history head back one commit, like Edit > Undo. "
                                    "Answers after the frame of the restored values.")
                              : QStringLiteral(
                                  "Moves the history head forward one commit, like Edit > Redo. "
                                    "Answers after the frame of the restored values.");
    move.params_schema = AutomationClosedParamsSchema();
    move.result_schema = move_result_schema;
    move.changes_state = true;
    move.handler       = [host, undo](const QJsonObject&, AutomationReply reply) {
      ui::EditorSessionController* editor = OpenEditor(host, reply);
      if (editor == nullptr) {
        return;
      }
      SendHistoryMove(editor, new AutomationCommandWait(std::move(reply), host), undo);
    };
    RegisterOrFail(registry, std::move(move), &ok, error);
  }

  AutomationCommandSpec history;
  history.method      = QStringLiteral("editor.history");
  history.description = QStringLiteral(
      "Returns the history of the active Version of the open image: its commits from the root, "
      "the head, and whether undo and redo are possible. 'position' is applied, current (the "
      "head), or future (undone, can be redone).");
  history.params_schema = AutomationClosedParamsSchema();
  history.result_schema = AutomationObjectSchema(
      QJsonObject{{"version_id", text_schema},
                  {"head_commit", head_schema},
                  {"can_undo", QJsonObject{{"type", "boolean"}}},
                  {"can_redo", QJsonObject{{"type", "boolean"}}},
                  {"commits", QJsonObject{{"type", "array"}}}},
      QJsonArray{"version_id", "head_commit", "can_undo", "can_redo", "commits"});
  history.handler = [host](const QJsonObject&, AutomationReply reply) {
    ui::EditorSessionController* editor = OpenEditor(host, reply);
    if (editor == nullptr) {
      return;
    }
    const auto snapshot = editor->history_snapshot();
    QJsonArray commits;
    for (const auto& commit : snapshot.commits) {
      const char* position = "applied";
      if (commit.position == EditorHistoryTimelinePosition::Current) {
        position = "current";
      } else if (commit.position == EditorHistoryTimelinePosition::Future) {
        position = "future";
      }
      commits.push_back(QJsonObject{
          {"commit_hash", QString::fromStdString(commit.commit_hash.ToString())},
          {"parent_hash",
           commit.first_parent_hash.has_value()
               ? QJsonValue(QString::fromStdString(commit.first_parent_hash->ToString()))
               : QJsonValue(QJsonValue::Null)},
          {"field", QString::fromStdString(commit.field_key)},
          {"operation_kind", QString::fromStdString(commit.operation_kind)},
          {"position", QString::fromLatin1(position)}});
    }
    reply.SendResult(
        QJsonObject{{"version_id", QString::fromStdString(snapshot.active_version_id.ToString())},
                    {"head_commit", HeadValue(snapshot)},
                    {"can_undo", snapshot.can_undo},
                    {"can_redo", snapshot.can_redo},
                    {"commits", commits}});
  };
  RegisterOrFail(registry, std::move(history), &ok, error);

  AutomationCommandSpec actions;
  actions.method      = QStringLiteral("editor.actions");
  actions.description = QStringLiteral(
      "Returns which editor actions the session admits now, with the reason for each refused "
      "one. The GUI enables its controls from the same decisions.");
  actions.params_schema = AutomationClosedParamsSchema();
  actions.result_schema = AutomationObjectSchema(
      QJsonObject{{"actions", QJsonObject{{"type", "array"}}}}, QJsonArray{"actions"});
  actions.handler = [host](const QJsonObject&, AutomationReply reply) {
    ui::EditorSessionController* editor  = host->editor_session();
    const auto*                  backend = editor != nullptr ? editor->session_backend() : nullptr;
    if (backend == nullptr) {
      SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                               QStringLiteral("The editor session is not available."));
      return;
    }
    const auto availability = backend->action_availability();
    QJsonArray list;
    for (std::size_t i = 0; i < EditorActionCount(); ++i) {
      const auto  action   = static_cast<EditorAction>(i);
      const auto& decision = availability.For(action);
      QJsonObject entry{{"name", QString::fromLatin1(EditorActionName(action))},
                        {"allowed", decision.allowed}};
      if (!decision.allowed) {
        entry.insert("reason", QString::fromStdString(decision.reason));
      }
      list.push_back(entry);
    }
    reply.SendResult(QJsonObject{{"actions", list}});
  };
  RegisterOrFail(registry, std::move(actions), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
