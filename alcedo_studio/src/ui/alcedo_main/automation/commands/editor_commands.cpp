//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
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
#include <string_view>
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
#include "edit/history/edit_commit_attribution.hpp"
#include "json.hpp"
#include "storage/store/sleeve/element_store.hpp"
#include "ui/alcedo_main/album_backend/editor_panel_presentation.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_editor_commands.hpp"
#include "ui/alcedo_main/automation/automation_editor_operation.hpp"
#include "ui/alcedo_main/automation/headless_frame_sink.hpp"
#include "ui/editor_rhi/editor_viewport_item.hpp"

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

/// The panel values of the open image, read from its current document. Each field is read from
/// its owner node (FieldOwnerParameterTarget): a Color Grade field from the selected Color Grade,
/// and crop, white balance, and the display transform from their nodes whatever node is
/// selected. A field without an owner in the image is left out. Empty when the document cannot be
/// read.
auto CurrentPanelValues(const ui::EditorSessionController& editor) -> QVariantMap {
  const auto document = editor.pipeline_document();
  if (!document) {
    return {};
  }
  const NodeId                       selected = editor.selected_node_id();
  const auto                         table    = EditorPanelAdapterTable::Production();
  std::vector<EditorParameterTarget> targets;
  for (const auto& adapter : table.Adapters()) {
    std::string error;
    auto        target =
        FieldOwnerParameterTarget(*document, selected, std::string{adapter.field_key}, &error);
    if (target.has_value()) {
      targets.push_back(std::move(*target));
    }
  }
  EditorPanelProjection projection;
  std::string           error;
  if (!ProjectEditorPanelFields(*document, targets, 0, &projection, &error)) {
    return {};
  }
  return ui::PanelProjectionToVariantMap(projection);
}

/// The owner target of @p field in the open image; nothing when the editor cannot edit or the
/// image has no owner of the field. The write then takes the selection path, which reports why.
auto FieldTarget(const ui::EditorSessionController& editor, std::string field)
    -> std::optional<EditorParameterTarget> {
  const auto document = editor.pipeline_document();
  if (!editor.can_edit() || !document) {
    return std::nullopt;
  }
  std::string error;
  return FieldOwnerParameterTarget(*document, editor.selected_node_id(), std::move(field), &error);
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

/// A field write that the catalog accepted: the canonical field, its typed write, and the
/// target of its owner node (nothing when the editor cannot edit now).
struct PreparedWrite {
  std::string                          field_;
  EditorParameterWrite                 write_;
  std::optional<EditorParameterTarget> target_;
};

/// Source size of the frame that the editor presented last, like the Geometry panel reads it:
/// from the bound viewport, otherwise from the headless sink. Zero before the first frame.
auto PresentedSource(const ui::EditorSessionController& editor) -> EditorParameterSource {
  Extent2D extent{};
  if (const auto* item =
          qobject_cast<const editor_rhi::EditorViewportItem*>(editor.presentation_viewport())) {
    extent = item->presentedMaskGeometry().full_reference_extent;
  } else if (const auto* sink =
                 dynamic_cast<const HeadlessFrameSink*>(editor.presentation_frame_sink())) {
    extent = sink->presented_source_extent();
  }
  return EditorParameterSource{extent.width, extent.height};
}

/// Converts the UI value @p value of @p field to one typed write. Writes the reason to @p error.
auto PrepareWrite(const ui::EditorSessionController& editor, const QString& field,
                  const QJsonValue& value, QString* error) -> std::optional<PreparedWrite> {
  const auto* entry = EditorParameterCatalog::Find(field.toStdString());
  if (entry == nullptr) {
    *error = QStringLiteral("unknown editor field: %1; editor.catalog lists the fields").arg(field);
    return std::nullopt;
  }
  std::string reason;
  // The crop constraint needs the source size: a locked aspect is a ratio of pixels, not of
  // normalized coordinates.
  const auto  model = EditorParameterCatalog::ToModelJson(
      entry->field, ToModelValue(value),
      CurrentModelValue(CurrentPanelValues(editor), entry->field), PresentedSource(editor),
      &reason);
  if (!model.has_value()) {
    *error = QString::fromStdString(reason);
    return std::nullopt;
  }
  auto write = ParseEditorParameterWrite(entry->field, *model, &reason);
  if (!write.has_value()) {
    *error = QString::fromStdString(reason);
    return std::nullopt;
  }
  return PreparedWrite{std::string(entry->field), std::move(*write),
                       FieldTarget(editor, std::string(entry->field))};
}

/// The attribution of the commits of one write command: the author agent and @p params.note.
auto AgentAttribution(const QJsonObject& params) -> EditCommitAttribution {
  return EditCommitAttribution{std::string(kEditCommitAuthorAgent),
                               params.value("note").toString().toStdString()};
}

/// Enqueues @p prepared as a settled write with the author @p attribution and calls @p done
/// after its commit and frame.
void ApplyWrite(ui::EditorSessionController* editor, QObject* context, PreparedWrite prepared,
                EditCommitAttribution attribution, OperationDone done) {
  auto       wait   = OperationWait::Begin(editor, context, std::move(done));
  const auto result = prepared.target_.has_value()
                          ? editor->EnqueueTargetedFieldWrite(
                                std::move(*prepared.target_), std::move(prepared.write_),
                                /*settled=*/true, std::move(attribution))
                          : editor->EnqueueFieldWrite(QString::fromStdString(prepared.field_),
                                                      std::move(prepared.write_),
                                                      /*settled=*/true, std::move(attribution));
  if (EditorSessionResultIsFailure(result.kind)) {
    wait->Fail(OperationFailure{AutomationErrorCode::Rejected,
                                result.message.empty()
                                    ? QStringLiteral("The editor cannot edit now.")
                                    : QString::fromStdString(result.message)});
    return;
  }
  wait->Start(result.operation_id);
}

constexpr std::string_view kOdtField       = "odt";
constexpr std::string_view kFilmGrainField = "film_grain";

/// `odt.method` in the panel values @p values; empty when the image shows no display transform.
auto                       OdtMethod(const QVariantMap& values) -> std::string {
  const auto value = CurrentUiValue(values, *EditorParameterCatalog::Find(kOdtField));
  if (!value.has_value() || !value->isObject()) {
    return {};
  }
  return value->toObject().value("method").toString().toStdString();
}

/// The method before a write of @p field: the input of the Film Grain rule of a method change.
/// Nothing for a write of another field, or when @p explicit_grain (the call also writes
/// film_grain, and the explicit value wins).
auto MethodBeforeWrite(const ui::EditorSessionController& editor, std::string_view field,
                       bool explicit_grain) -> std::optional<std::string> {
  if (field != kOdtField || explicit_grain) {
    return std::nullopt;
  }
  return OdtMethod(CurrentPanelValues(editor));
}

/// After an `odt` write on a document with method @p previous_method, writes the Film Grain of
/// the method rule (EditorParameterCatalog::FilmGrainAfterMethodChange, also the rule of the
/// Display Transform panel) as a second settled write: the second history entry, as the panel
/// makes it. Calls @p done with the failure and whether the rule wrote Film Grain.
void ApplyFilmGrainRule(ui::EditorSessionController* editor, QObject* context,
                        const std::string& previous_method, EditCommitAttribution attribution,
                        std::function<void(std::optional<OperationFailure>, bool)> done) {
  const QVariantMap current = CurrentPanelValues(*editor);
  const auto        grain = CurrentUiValue(current, *EditorParameterCatalog::Find(kFilmGrainField));
  const auto        value = grain.has_value() ? EditorParameterCatalog::FilmGrainAfterMethodChange(
                                             previous_method, OdtMethod(current), grain->toDouble())
                                              : std::nullopt;
  if (!value.has_value()) {
    done(std::nullopt, false);
    return;
  }
  QString    error;
  const auto field    = Text(kFilmGrainField);
  auto       prepared = PrepareWrite(*editor, field, QJsonValue(*value), &error);
  if (!prepared.has_value()) {
    done(OperationFailure{AutomationErrorCode::Failed, error}, false);
    return;
  }
  ApplyWrite(editor, context, std::move(*prepared), std::move(attribution),
             [done = std::move(done)](std::optional<OperationFailure> failure) {
               done(std::move(failure), true);
             });
}

/// Answers @p failure of a write, undo, or redo. A committed failure (its frame failed to render)
/// also carries the head commit, read after the queued history change, so the agent can undo it.
void SendOperationFailure(const QPointer<AutomationCommandWait>& wait,
                          ui::EditorSessionController* editor, const OperationFailure& failure) {
  if (!failure.committed_) {
    wait->SendOwnerError(failure.code_, failure.message_);
    return;
  }
  AfterQueuedChanges(wait, [wait, editor, failure] {
    if (wait == nullptr) {
      return;
    }
    wait->SendError(failure.code_, failure.message_,
                    QJsonObject{{"reason", failure.message_},
                                {"head_commit", HeadValue(editor->history_snapshot())}});
  });
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
          SendOperationFailure(wait, editor, *failure);
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
/// Agent control is checked again before each change after the first: when the user took
/// control, the batch stops with the refusal, and the changes before it stay. A change of the
/// display transform method is followed by the Film Grain of the method rule
/// (ApplyFilmGrainRule) unless a change of the batch writes film_grain.
class BatchApply final : public std::enable_shared_from_this<BatchApply> {
 public:
  BatchApply(const AutomationCommandRegistry* registry, ui::EditorSessionController* editor,
             AutomationCommandWait* wait, QJsonArray changes, EditCommitAttribution attribution)
      : registry_(registry),
        editor_(editor),
        wait_(wait),
        changes_(std::move(changes)),
        attribution_(std::move(attribution)) {
    for (const auto& change : changes_) {
      const auto* entry =
          EditorParameterCatalog::Find(change.toObject().value("field").toString().toStdString());
      explicit_grain_ = explicit_grain_ || (entry != nullptr && entry->field == kFilmGrainField);
    }
  }

  void Next() {
    if (wait_ == nullptr) {
      return;
    }
    if (applied_ == changes_.size()) {
      SendDone();
      return;
    }
    if (applied_ > 0) {
      if (const auto refusal = registry_->AdmitStateChange()) {
        QJsonObject data = FailureData(refusal->message, {});
        for (auto it = refusal->data.begin(); it != refusal->data.end(); ++it) {
          data.insert(it.key(), it.value());
        }
        wait_->SendError(refusal->code, refusal->message, data);
        return;
      }
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
    previous_method_ = MethodBeforeWrite(*editor_, prepared->field_, explicit_grain_);
    auto self        = shared_from_this();
    ApplyWrite(editor_, wait_, std::move(*prepared), attribution_,
               [self](std::optional<OperationFailure> failure) { self->Finished(failure); });
  }

 private:
  void Finished(const std::optional<OperationFailure>& failure) {
    if (wait_ == nullptr) {
      return;
    }
    if (failure.has_value() && !failure->committed_) {
      wait_->SendError(failure->code_, failure->message_, FailureData(failure->message_, {}));
      return;
    }
    ++applied_;
    auto self = shared_from_this();
    if (failure.has_value()) {
      // The change is committed and stays, so it counts as applied; only its frame failed. The
      // head is read after the queued history change.
      AfterQueuedChanges(wait_, [self, failure = *failure] {
        if (self->wait_ != nullptr) {
          self->wait_->SendError(failure.code_, failure.message_,
                                 self->FailureData(failure.message_, {}));
        }
      });
      return;
    }
    AfterQueuedChanges(wait_, [self] { self->ApplyRuleThenNext(); });
  }

  /// After a change of `odt`, the Film Grain write of the method rule; then the next change.
  void ApplyRuleThenNext() {
    if (wait_ == nullptr) {
      return;
    }
    const auto previous_method = std::exchange(previous_method_, std::nullopt);
    if (!previous_method.has_value()) {
      Next();
      return;
    }
    auto self = shared_from_this();
    ApplyFilmGrainRule(editor_, wait_, *previous_method, attribution_,
                       [self](std::optional<OperationFailure> failure, bool wrote) {
                         self->RuleFinished(failure, wrote);
                       });
  }

  /// The Film Grain write of the rule belongs to the change before it, so it does not count in
  /// `applied`.
  void RuleFinished(const std::optional<OperationFailure>& failure, bool wrote) {
    if (wait_ == nullptr) {
      return;
    }
    grain_written_ = grain_written_ || (wrote && (!failure.has_value() || failure->committed_));
    auto self      = shared_from_this();
    if (failure.has_value()) {
      AfterQueuedChanges(wait_, [self, failure = *failure] {
        if (self->wait_ != nullptr) {
          self->wait_->SendError(failure.code_, failure.message_,
                                 self->FailureData(failure.message_, {}));
        }
      });
      return;
    }
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
    if (grain_written_) {
      if (const auto value =
              CurrentUiValue(current, *EditorParameterCatalog::Find(kFilmGrainField))) {
        values.insert(Text(kFilmGrainField), *value);
      }
    }
    wait_->SendResult(QJsonObject{{"applied", applied_},
                                  {"head_commit", HeadValue(editor_->history_snapshot())},
                                  {"values", values}});
  }

  const AutomationCommandRegistry* registry_;
  ui::EditorSessionController*     editor_;
  QPointer<AutomationCommandWait>  wait_;
  QJsonArray                       changes_;
  EditCommitAttribution            attribution_;
  qsizetype                        applied_ = 0;
  /// True when a change of the batch writes film_grain: the method rule then writes nothing.
  bool                             explicit_grain_ = false;
  /// The method before the change in progress, when it is a change of `odt` that the method
  /// rule follows.
  std::optional<std::string>       previous_method_;
  /// True when the method rule wrote Film Grain; the result then lists film_grain.
  bool                             grain_written_ = false;
};

/// Answers editor.set: the UI value of @p field and, in `values`, each field that the call
/// wrote: @p field, and film_grain when @p grain_written (the method rule wrote it).
void SendSetResult(const QPointer<AutomationCommandWait>& wait,
                   const ui::EditorSessionController& editor, const QString& field,
                   bool grain_written) {
  const QVariantMap current = CurrentPanelValues(editor);
  const auto*       entry   = EditorParameterCatalog::Find(field.toStdString());
  const auto        value   = CurrentUiValue(current, *entry);
  QJsonObject       values;
  if (value.has_value()) {
    values.insert(Text(entry->field), *value);
  }
  if (grain_written) {
    if (const auto grain =
            CurrentUiValue(current, *EditorParameterCatalog::Find(kFilmGrainField))) {
      values.insert(Text(kFilmGrainField), *grain);
    }
  }
  wait->SendResult(QJsonObject{{"field", field},
                               {"value", value.value_or(QJsonValue(QJsonValue::Null))},
                               {"values", values},
                               {"head_commit", HeadValue(editor.history_snapshot())}});
}

auto IsPersistBusy(const ui::EditorSessionController& editor) -> bool {
  const auto state = editor.session_state();
  return editor.close_in_flight() || editor.persist_in_flight() ||
         state == EditorSessionState::Saving || state == EditorSessionState::Switching;
}

auto NoteSchema() -> QJsonObject {
  return QJsonObject{{"type", "string"},
                     {"description",
                      "A note for the commits of this call. The History panel shows it with the "
                      "author agent."}};
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

  const QJsonObject id_schema = AutomationElementIdSchema();
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
    const auto service = AutomationEnteredProject(host, reply);
    if (!service) {
      return;
    }
    const auto element_id_param =
        AutomationElementIdParam(params, QStringLiteral("element_id"), reply);
    if (!element_id_param.has_value()) {
      return;
    }
    const auto                   element_id = *element_id_param;
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
      "commit. The commit has the author agent and the optional 'note'. When the frame of the "
      "commit fails to render, the commit stays and the error data has 'reason' and "
      "'head_commit'; editor.undo reverts it. A write of 'odt' that changes the method follows "
      "the rule of the Display Transform panel: method film_print turns on film_grain at 100 "
      "when it is 0, and another method on film_print sets film_grain to 0, in a second commit. "
      "'values' lists each field that the call wrote.");
  set.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"field", text_schema}, {"value", ValueSchema()}, {"note", NoteSchema()}},
      QJsonArray{"field", "value"});
  set.result_schema = AutomationObjectSchema(
      QJsonObject{{"field", text_schema},
                  {"value", QJsonObject{{"type", QJsonArray{"number", "object", "null"}}}},
                  {"values", QJsonObject{{"type", "object"}}},
                  {"head_commit", head_schema}},
      QJsonArray{"field", "value", "values", "head_commit"});
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
    const auto previous_method           = MethodBeforeWrite(*editor, prepared->field_, false);
    ApplyWrite(editor, wait, std::move(*prepared), AgentAttribution(params),
               [wait, editor, field, previous_method,
                attribution = AgentAttribution(params)](std::optional<OperationFailure> failure) {
                 if (wait == nullptr) {
                   return;
                 }
                 if (failure.has_value()) {
                   SendOperationFailure(wait, editor, *failure);
                   return;
                 }
                 AfterQueuedChanges(wait, [wait, editor, field, previous_method, attribution] {
                   if (wait == nullptr) {
                     return;
                   }
                   if (!previous_method.has_value()) {
                     SendSetResult(wait, *editor, field, false);
                     return;
                   }
                   ApplyFilmGrainRule(
                       editor, wait, *previous_method, attribution,
                       [wait, editor, field](std::optional<OperationFailure> failure, bool wrote) {
                         if (wait == nullptr) {
                           return;
                         }
                         if (failure.has_value()) {
                           SendOperationFailure(wait, editor, *failure);
                           return;
                         }
                         AfterQueuedChanges(wait, [wait, editor, field, wrote] {
                           if (wait != nullptr) {
                             SendSetResult(wait, *editor, field, wrote);
                           }
                         });
                       });
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
      "'applied' (the changes before it, which stay) and 'head_commit'. A change whose frame "
      "fails to render is committed and stays, so 'applied' counts it. Each commit has the "
      "author agent and the optional 'note'. A change of the 'odt' method follows the film_grain "
      "rule of editor.set unless a change of the batch writes film_grain; the rule commit "
      "belongs to that change and 'values' then lists film_grain.");
  batch.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"changes",
           QJsonObject{{"type", "array"},
                       {"minItems", 1},
                       {"items", AutomationClosedParamsSchema(
                                     QJsonObject{{"field", text_schema}, {"value", ValueSchema()}},
                                     QJsonArray{"field", "value"})}}},
          {"note", NoteSchema()}},
      QJsonArray{"changes"});
  batch.result_schema =
      AutomationObjectSchema(QJsonObject{{"applied", QJsonObject{{"type", "integer"}}},
                                         {"head_commit", head_schema},
                                         {"values", QJsonObject{{"type", "object"}}}},
                             QJsonArray{"applied", "head_commit", "values"});
  batch.changes_state = true;
  batch.handler       = [host, &registry](const QJsonObject& params, AutomationReply reply) {
    const QJsonArray changes = params.value("changes").toArray();
    if (changes.size() > kMaxBatchChanges) {
      SendAutomationParamError(
          reply, QStringLiteral("/changes"),
          QStringLiteral("a batch has at most %1 changes").arg(kMaxBatchChanges));
      return;
    }
    auto* wait = new AutomationCommandWait(std::move(reply), host);
    std::make_shared<BatchApply>(&registry, host->editor_session(), wait, changes,
                                 AgentAttribution(params))
        ->Next();
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
    move.description =
        undo ? QStringLiteral(
                   "Moves the history head back one commit, like Edit > Undo. "
                   "Answers after the frame of the restored values. When that frame fails to "
                   "render, the head stays moved and the error data has "
                   "'head_commit'.")
             : QStringLiteral(
                   "Moves the history head forward one commit, like Edit > Redo. "
                   "Answers after the frame of the restored values. When that frame fails to "
                   "render, the head stays moved and the error data has "
                   "'head_commit'.");
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
      "head), or future (undone, can be redone). 'author' is user or agent; 'note' is the note "
      "of an agent commit.");
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
          {"position", QString::fromLatin1(position)},
          {"author", QString::fromStdString(commit.author)},
          {"note", QString::fromStdString(commit.note)}});
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
