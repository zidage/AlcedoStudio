//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/adjustment_transfer_catalog.hpp"
#include "app/adjustment_transfer_package_builder.hpp"
#include "app/adjustment_transfer_types.hpp"
#include "app/editor_session_types.hpp"
#include "app/version_name_rules.hpp"
#include "edit/history/edit_commit_attribution.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_apply_coordinator.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"
#include "ui/alcedo_main/album_backend/interaction_policy_controller.hpp"
#include "ui/alcedo_main/album_backend/library_selection.hpp"
#include "ui/alcedo_main/automation/automation_adjustment_commands.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_editor_operation.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::automation {
namespace {

using ItemKind = AdjustmentTransferItemKind;

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

/// Protocol names of the item kinds, in the order of AdjustmentTransferItemKind.
const std::vector<std::pair<ItemKind, QString>>& ItemKindNames() {
  static const std::vector<std::pair<ItemKind, QString>> names{
      {ItemKind::NodeEnabled, QStringLiteral("enabled")},
      {ItemKind::NodeMix, QStringLiteral("mix")},
      {ItemKind::Adjustment, QStringLiteral("adjustment")},
      {ItemKind::Masks, QStringLiteral("masks")},
      {ItemKind::DrtParameters, QStringLiteral("drt")},
      {ItemKind::RawDecode, QStringLiteral("raw_decode")},
      {ItemKind::WhiteBalance, QStringLiteral("color_temp")},
      {ItemKind::LensCalibration, QStringLiteral("lens_calib")},
      {ItemKind::Geometry, QStringLiteral("crop_rotate")},
  };
  return names;
}

auto ItemKindName(ItemKind kind) -> QString {
  for (const auto& [value, name] : ItemKindNames()) {
    if (value == kind) {
      return name;
    }
  }
  return {};
}

auto ParseItemKind(const QString& name) -> std::optional<ItemKind> {
  for (const auto& [value, text] : ItemKindNames()) {
    if (text == name) {
      return value;
    }
  }
  return std::nullopt;
}

auto ItemKindEnum() -> QJsonArray {
  QJsonArray names;
  for (const auto& entry : ItemKindNames()) {
    names.push_back(entry.second);
  }
  return names;
}

auto NodeKindName(AdjustmentTransferNodeKind kind) -> QString {
  switch (kind) {
    case AdjustmentTransferNodeKind::ColorGrade:
      return QStringLiteral("color_grade");
    case AdjustmentTransferNodeKind::DrtPost:
      return QStringLiteral("drt_post");
    case AdjustmentTransferNodeKind::Develop:
      return QStringLiteral("develop");
  }
  return {};
}

auto SectionName(AdjustmentTransferItemSection section) -> QString {
  switch (section) {
    case AdjustmentTransferItemSection::Node:
      return QStringLiteral("node");
    case AdjustmentTransferItemSection::Tone:
      return QStringLiteral("tone");
    case AdjustmentTransferItemSection::Look:
      return QStringLiteral("look");
    case AdjustmentTransferItemSection::Lut:
      return QStringLiteral("lut");
    case AdjustmentTransferItemSection::DisplayTransform:
      return QStringLiteral("display_transform");
    case AdjustmentTransferItemSection::Masks:
      return QStringLiteral("masks");
    case AdjustmentTransferItemSection::Raw:
      return QStringLiteral("raw");
    case AdjustmentTransferItemSection::Geometry:
      return QStringLiteral("geometry");
  }
  return {};
}

auto Text(std::string_view text) -> QString {
  return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

/// One item of @p node as the protocol shows it.
auto ItemJson(const AdjustmentTransferItemDescriptor& item) -> QJsonObject {
  QJsonObject json{{"item", ItemKindName(item.kind)},
                   {"name", QString::fromStdString(item.display_name)},
                   {"value", QString::fromStdString(item.display_value)},
                   {"section", SectionName(item.section)},
                   {"available", item.enabled}};
  if (item.adjustment_id.has_value()) {
    json.insert("adjustment_id", Text(item.adjustment_id->Value()));
    json.insert("type", Text(item.type.Text()));
  }
  return json;
}

auto VersionJson(const AdjustmentTransferVersionDescriptor& version) -> QJsonObject {
  return QJsonObject{{"version_id", QString::fromStdString(version.version_id.ToString())},
                     {"name", QString::fromStdString(version.display_name)},
                     {"active", version.active}};
}

/// The photo @p element_id of the entered project; sends -32602 with `data.unknown_ids` and
/// returns false for an id that is not a photo of the project.
auto CheckKnownPhoto(ProjectService& project, sl_element_id_t element_id, AutomationReply& reply)
    -> bool {
  const QJsonObject ids{{"element_ids", QJsonArray{static_cast<double>(element_id)}}};
  return ResolveAutomationElementRows(project, ids, reply).has_value();
}

/// The Version of @p graph that @p params.version_id names, or the active Version without it.
/// Sends -32602 and returns nothing for an unknown id.
auto RequestedVersion(const CommitGraph& graph, const QJsonObject& params, AutomationReply& reply)
    -> std::optional<AdjustmentTransferVersionDescriptor> {
  const auto        versions = AdjustmentTransferCatalogService::ListVersions(graph);
  const std::string id = params.value("version_id").toString().trimmed().toLower().toStdString();
  for (const auto& version : versions) {
    if (params.contains("version_id") ? version.version_id.ToString() == id : version.active) {
      return version;
    }
  }
  if (params.contains("version_id")) {
    SendAutomationParamError(reply, QStringLiteral("/version_id"),
                             QStringLiteral("unknown Version id; adjustments.items lists them"));
  } else {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed,
                             QStringLiteral("The photo has no active Version."));
  }
  return std::nullopt;
}

/// The Versions of the source photo and the transferable items of the requested Version.
struct SourceCatalog {
  std::vector<AdjustmentTransferVersionDescriptor> versions_;
  AdjustmentTransferCatalogRead                    read_;
};

/// Reads the source photo @p params.element_id at @p params.version_id (the active Version
/// without it) through AdjustmentTransferController::ReadSourceHistory, the read of Copy
/// Adjustments. Sends the error and returns nothing when the read fails.
auto ReadSourceCatalog(ui::ApplicationModuleHost* host, const QJsonObject& params,
                       AutomationReply& reply) -> std::optional<SourceCatalog> {
  const auto project = AutomationEnteredProject(host, reply);
  if (!project) {
    return std::nullopt;
  }
  const auto element_id_param =
      AutomationElementIdParam(params, QStringLiteral("element_id"), reply);
  if (!element_id_param.has_value()) {
    return std::nullopt;
  }
  const auto element_id = *element_id_param;
  if (!CheckKnownPhoto(*project, element_id, reply)) {
    return std::nullopt;
  }
  std::shared_ptr<const CommitGraph>      graph;
  std::shared_ptr<const PipelineDocument> root;
  QString                                 read_error;
  if (!host->adjustment_transfer()->ReadSourceHistory(element_id, &graph, &root, &read_error)) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed, read_error);
    return std::nullopt;
  }
  const auto version = RequestedVersion(*graph, params, reply);
  if (!version.has_value()) {
    return std::nullopt;
  }
  std::string error;
  auto        read =
      AdjustmentTransferCatalogService::ReadVersion(*graph, *root, version->version_id, &error);
  if (!read.has_value()) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed, QString::fromStdString(error));
    return std::nullopt;
  }
  return SourceCatalog{AdjustmentTransferCatalogService::ListVersions(*graph), std::move(*read)};
}

auto FindNode(const std::vector<AdjustmentTransferNodeDescriptor>& nodes, const QString& node_id)
    -> const AdjustmentTransferNodeDescriptor* {
  const auto found = std::find_if(nodes.begin(), nodes.end(), [&](const auto& node) {
    return Text(node.node_id.Value()) == node_id;
  });
  return found != nodes.end() ? &*found : nullptr;
}

/// One chosen item: its node and descriptor.
struct ChosenItem {
  const AdjustmentTransferNodeDescriptor* node_;
  const AdjustmentTransferItemDescriptor* item_;
};

/// The items that @p params.items chooses from @p nodes, in node and item order of the
/// document; every available item without `items`. Sends -32602 with the pointer of the
/// first entry that names an unknown node or item, an item that is not available, or an
/// item twice, and returns nothing.
auto ChooseItems(const std::vector<AdjustmentTransferNodeDescriptor>& nodes,
                 const QJsonObject& params, AutomationReply& reply)
    -> std::optional<std::vector<ChosenItem>> {
  std::vector<ChosenItem> chosen;
  const auto              is_chosen = [&chosen](const AdjustmentTransferItemDescriptor* item) {
    return std::any_of(chosen.begin(), chosen.end(),
                                    [item](const ChosenItem& entry) { return entry.item_ == item; });
  };
  if (!params.contains("items")) {
    for (const auto& node : nodes) {
      for (const auto& item : node.items) {
        if (item.enabled) {
          chosen.push_back({&node, &item});
        }
      }
    }
    return chosen;
  }

  const QJsonArray entries = params.value("items").toArray();
  for (qsizetype index = 0; index < entries.size(); ++index) {
    const QJsonObject entry   = entries.at(index).toObject();
    const QString     pointer = QStringLiteral("/items/%1").arg(index);
    const QString     node_id = entry.value("node_id").toString();
    const auto*       node    = FindNode(nodes, node_id);
    if (node == nullptr) {
      SendAutomationParamError(
          reply, pointer + QStringLiteral("/node_id"),
          QStringLiteral("unknown node id: %1; adjustments.items lists the nodes").arg(node_id));
      return std::nullopt;
    }
    if (!entry.contains("item")) {
      // A node without an item names every available item of the node.
      for (const auto& item : node->items) {
        if (item.enabled && !is_chosen(&item)) {
          chosen.push_back({node, &item});
        }
      }
      continue;
    }
    const auto    kind          = ParseItemKind(entry.value("item").toString());
    const QString adjustment_id = entry.value("adjustment_id").toString();
    if (kind == ItemKind::Adjustment && adjustment_id.isEmpty()) {
      SendAutomationParamError(reply, pointer + QStringLiteral("/adjustment_id"),
                               QStringLiteral("an adjustment item needs its adjustment_id"));
      return std::nullopt;
    }
    if (kind != ItemKind::Adjustment && entry.contains("adjustment_id")) {
      SendAutomationParamError(reply, pointer + QStringLiteral("/adjustment_id"),
                               QStringLiteral("only an adjustment item has an adjustment_id"));
      return std::nullopt;
    }
    const auto found = std::find_if(node->items.begin(), node->items.end(), [&](const auto& item) {
      return item.kind == kind &&
             (kind != ItemKind::Adjustment || (item.adjustment_id.has_value() &&
                                               Text(item.adjustment_id->Value()) == adjustment_id));
    });
    if (found == node->items.end()) {
      SendAutomationParamError(
          reply, pointer,
          QStringLiteral("node %1 has no such item; adjustments.items lists its items")
              .arg(node_id));
      return std::nullopt;
    }
    if (!found->enabled) {
      SendAutomationParamError(reply, pointer,
                               QStringLiteral("item %1 of node %2 has nothing to copy")
                                   .arg(QString::fromStdString(found->display_name), node_id));
      return std::nullopt;
    }
    if (is_chosen(&*found)) {
      SendAutomationParamError(reply, pointer, QStringLiteral("the item is named twice"));
      return std::nullopt;
    }
    chosen.push_back({node, &*found});
  }
  // The package keeps the document order; report the chosen items in that order too.
  std::vector<ChosenItem> ordered;
  for (const auto& node : nodes) {
    for (const auto& item : node.items) {
      if (is_chosen(&item)) {
        ordered.push_back({&node, &item});
      }
    }
  }
  return ordered;
}

/// The Copy selection of @p chosen items of the Version @p version_id.
auto MakeSelection(const version_ref_id_t& version_id, const std::vector<ChosenItem>& chosen)
    -> AdjustmentTransferSelection {
  AdjustmentTransferSelection selection;
  selection.source_version_id = version_id;
  for (const auto& entry : chosen) {
    if (selection.nodes.empty() || selection.nodes.back().node_id != entry.node_->node_id) {
      selection.nodes.push_back(AdjustmentTransferNodeSelection{entry.node_->node_id, {}});
    }
    selection.nodes.back().items.push_back(
        AdjustmentTransferItemSelection{entry.item_->kind, entry.item_->adjustment_id});
  }
  return selection;
}

/// Packages of adjustments.copy in this session, by package id. A package is the point-in-time
/// copy that Paste needs: the source photo can change after the copy, and the package must
/// still paste the copied values, like the copied package of the GUI. It is immutable and lives
/// until the session ends.
struct AdjustmentPackageStore {
  int                                          next_number_ = 0;
  std::map<QString, AdjustmentTransferPackage> packages_;
};

/// The answer of adjustments.paste after the paste into the editor photo, when there is one.
struct PasteAnswer {
  QString     task_id_;
  int         target_count_ = 0;
  QJsonObject editor_;
};

auto PasteAnswerJson(const PasteAnswer& answer) -> QJsonObject {
  QJsonObject json{{"task_id", answer.task_id_.isEmpty() ? QJsonValue(QJsonValue::Null)
                                                         : QJsonValue(answer.task_id_)},
                   {"target_count", answer.target_count_}};
  if (!answer.editor_.isEmpty()) {
    json.insert("editor", answer.editor_);
  }
  return json;
}

/// Starts the background Paste of @p package on @p ids and answers @p wait with its task id.
/// Without ids, answers at once. Agent control is checked again first, because the paste into
/// the editor photo ran before.
void StartLibraryPaste(AutomationCommandRegistry* registry, ui::ApplicationModuleHost* host,
                       AutomationCommandWait* wait, const AdjustmentTransferPackage& package,
                       std::vector<sl_element_id_t> ids, const QString& version_name,
                       const EditCommitAttribution& attribution, PasteAnswer answer) {
  if (ids.empty()) {
    if (!answer.editor_.isEmpty() && !answer.editor_.value("pasted").toBool()) {
      wait->SendOwnerError(AutomationErrorCode::Rejected,
                           answer.editor_.value("message").toString());
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
  if (!coordinator->StartApplyToTargets(package, std::move(ids), version_name, &error,
                                        attribution)) {
    wait->SendOwnerError(AutomationErrorCode::Busy, error);
    return;
  }
  answer.task_id_ = coordinator->running_task_id();
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

auto RegisterAutomationAdjustmentCommands(AutomationCommandRegistry& registry,
                                          ui::ApplicationModuleHost* host, const QString& host_mode,
                                          QString* error) -> bool {
  bool              ok                  = true;
  auto              store               = std::make_shared<AdjustmentPackageStore>();
  const bool        selection_available = host_mode != QStringLiteral("headless");
  auto* const       registry_ptr        = &registry;

  const QJsonObject text_schema{{"type", "string"}};
  const QJsonObject id_schema =
      AutomationElementIdSchema(1, QStringLiteral("The photo (library.list)."));
  const QJsonObject version_id_schema{
      {"type", "string"},
      {"description", "Hex id of a Version (adjustments.items); the active Version without it."}};
  const QJsonObject item_kind_schema{
      {"type", "string"},
      {"enum", ItemKindEnum()},
      {"description",
       "enabled and mix: node values of a Color Grade. adjustment: one adjustment, with "
       "adjustment_id. masks: all Masks of a Color Grade. drt: the display transform. "
       "raw_decode, color_temp, lens_calib, crop_rotate: the Develop fields."}};
  const QJsonObject item_schema =
      AutomationObjectSchema(QJsonObject{{"item", text_schema},
                                         {"adjustment_id", text_schema},
                                         {"type", text_schema},
                                         {"name", text_schema},
                                         {"value", text_schema},
                                         {"section", text_schema},
                                         {"available", QJsonObject{{"type", "boolean"}}}},
                             QJsonArray{"item", "name", "value", "section", "available"});
  const QJsonObject node_schema = AutomationObjectSchema(
      QJsonObject{{"node_id", text_schema},
                  {"kind", text_schema},
                  {"name", text_schema},
                  {"default_grade", QJsonObject{{"type", "boolean"}}},
                  {"items", QJsonObject{{"type", "array"}, {"items", item_schema}}}},
      QJsonArray{"node_id", "kind", "name", "items"});
  const QJsonObject version_schema =
      AutomationObjectSchema(QJsonObject{{"version_id", text_schema},
                                         {"name", text_schema},
                                         {"active", QJsonObject{{"type", "boolean"}}}},
                             QJsonArray{"version_id", "name", "active"});

  AutomationCommandSpec items;
  items.method      = QStringLiteral("adjustments.items");
  items.description = QStringLiteral(
      "Lists the Versions of a photo and the items of one Version that adjustments.copy can "
      "copy, like the Copy Adjustments dialog: the Develop node (raw_decode, color_temp, "
      "lens_calib, crop_rotate), each Color Grade (enabled, mix, each adjustment, masks), and "
      "the DRT/Post node (drt and each post adjustment). An item with 'available' false has "
      "nothing to copy (a Color Grade without Masks).");
  items.params_schema = AutomationClosedParamsSchema(
      QJsonObject{{"element_id", id_schema}, {"version_id", version_id_schema}},
      QJsonArray{"element_id"});
  items.result_schema = AutomationObjectSchema(
      QJsonObject{{"element_id", QJsonObject{{"type", "integer"}}},
                  {"version_id", text_schema},
                  {"version_name", text_schema},
                  {"versions", QJsonObject{{"type", "array"}, {"items", version_schema}}},
                  {"nodes", QJsonObject{{"type", "array"}, {"items", node_schema}}}},
      QJsonArray{"element_id", "version_id", "version_name", "versions", "nodes"});
  items.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto catalog = ReadSourceCatalog(host, params, reply);
    if (!catalog.has_value()) {
      return;
    }
    QJsonArray versions;
    for (const auto& version : catalog->versions_) {
      versions.push_back(VersionJson(version));
    }
    QJsonArray nodes;
    for (const auto& node : catalog->read_.nodes) {
      QJsonArray node_items;
      for (const auto& item : node.items) {
        node_items.push_back(ItemJson(item));
      }
      nodes.push_back(QJsonObject{{"node_id", Text(node.node_id.Value())},
                                  {"kind", NodeKindName(node.kind)},
                                  {"name", QString::fromStdString(node.display_name)},
                                  {"default_grade", node.is_default_grade},
                                  {"items", node_items}});
    }
    reply.SendResult(QJsonObject{
        {"element_id", params.value("element_id")},
        {"version_id", QString::fromStdString(catalog->read_.version.version_id.ToString())},
        {"version_name", QString::fromStdString(catalog->read_.version.display_name)},
        {"versions", versions},
        {"nodes", nodes}});
  };
  RegisterOrFail(registry, std::move(items), &ok, error);

  AutomationCommandSpec copy;
  copy.method      = QStringLiteral("adjustments.copy");
  copy.description = QStringLiteral(
      "Copies adjustments of a photo, like Copy Adjustments: builds a package from the chosen "
      "items of a Version (the active Version without 'version_id') and returns its "
      "'package_id' for adjustments.paste. Without 'items' every available item is copied, "
      "also crop_rotate and color_temp. An entry of 'items' names a node (adjustments.items) "
      "and one 'item' of it; an entry without 'item' copies every available item of the node. "
      "The package keeps the copied values for the session; later edits of the photo do not "
      "change it.");
  copy.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"element_id", id_schema},
          {"version_id", version_id_schema},
          {"items", QJsonObject{{"type", "array"},
                                {"minItems", 1},
                                {"items", QJsonObject{{"type", "object"},
                                                      {"properties",
                                                       QJsonObject{{"node_id", text_schema},
                                                                   {"item", item_kind_schema},
                                                                   {"adjustment_id", text_schema}}},
                                                      {"required", QJsonArray{"node_id"}},
                                                      {"additionalProperties", false}}}}}},
      QJsonArray{"element_id"});
  const QJsonObject copied_item_schema =
      AutomationObjectSchema(QJsonObject{{"node_id", text_schema},
                                         {"node_name", text_schema},
                                         {"item", text_schema},
                                         {"adjustment_id", text_schema},
                                         {"name", text_schema},
                                         {"value", text_schema}},
                             QJsonArray{"node_id", "node_name", "item", "name", "value"});
  const QJsonObject skipped_ai_mask_schema = AutomationObjectSchema(
      QJsonObject{{"node_id", text_schema}, {"mask_id", text_schema}, {"name", text_schema}},
      QJsonArray{"node_id", "mask_id", "name"});
  copy.result_schema = AutomationObjectSchema(
      QJsonObject{{"package_id", text_schema},
                  {"element_id", QJsonObject{{"type", "integer"}}},
                  {"version_id", text_schema},
                  {"version_name", text_schema},
                  {"items", QJsonObject{{"type", "array"}, {"items", copied_item_schema}}},
                  {"skipped_ai_masks",
                   QJsonObject{{"type", "array"},
                               {"items", skipped_ai_mask_schema},
                               {"description",
                                "AI Masks of the copied Masks items. A package never holds an AI "
                                "Mask: it describes the content of its own photo."}}}},
      QJsonArray{"package_id", "element_id", "version_id", "version_name", "items",
                 "skipped_ai_masks"});
  copy.handler = [host, store](const QJsonObject& params, AutomationReply reply) {
    const auto catalog = ReadSourceCatalog(host, params, reply);
    if (!catalog.has_value()) {
      return;
    }
    const auto chosen = ChooseItems(catalog->read_.nodes, params, reply);
    if (!chosen.has_value()) {
      return;
    }
    if (chosen->empty()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Rejected,
                               QStringLiteral("The Version has no item to copy."));
      return;
    }
    const auto                selection = MakeSelection(catalog->read_.version.version_id, *chosen);
    AdjustmentTransferPackage package;
    try {
      package = AdjustmentTransferPackageBuilder::Build(catalog->read_.document, selection);
    } catch (const std::exception& e) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Rejected, QString::fromUtf8(e.what()));
      return;
    }
    const QString package_id = QStringLiteral("package-%1").arg(++store->next_number_);
    store->packages_.emplace(package_id, std::move(package));

    QJsonArray copied;
    for (const auto& entry : *chosen) {
      QJsonObject row{{"node_id", Text(entry.node_->node_id.Value())},
                      {"node_name", QString::fromStdString(entry.node_->display_name)},
                      {"item", ItemKindName(entry.item_->kind)},
                      {"name", QString::fromStdString(entry.item_->display_name)},
                      {"value", QString::fromStdString(entry.item_->display_value)}};
      if (entry.item_->adjustment_id.has_value()) {
        row.insert("adjustment_id", Text(entry.item_->adjustment_id->Value()));
      }
      copied.push_back(row);
    }
    QJsonArray skipped_ai_masks;
    for (const auto& mask : ListAiMasksExcludedFromTransfer(catalog->read_.document, selection)) {
      skipped_ai_masks.push_back(QJsonObject{{"node_id", Text(mask.node_id.Value())},
                                             {"mask_id", Text(mask.mask_id.Value())},
                                             {"name", QString::fromStdString(mask.display_name)}});
    }
    reply.SendResult(QJsonObject{
        {"package_id", package_id},
        {"element_id", params.value("element_id")},
        {"version_id", QString::fromStdString(catalog->read_.version.version_id.ToString())},
        {"version_name", QString::fromStdString(catalog->read_.version.display_name)},
        {"items", copied},
        {"skipped_ai_masks", skipped_ai_masks}});
  };
  RegisterOrFail(registry, std::move(copy), &ok, error);

  AutomationCommandSpec paste;
  paste.method      = QStringLiteral("adjustments.paste");
  paste.description = QStringLiteral(
      "Pastes a package of adjustments.copy onto photos, like Paste Adjustments: each photo "
      "gets one new Version (named 'version_name', or Pasted Adjustments) with one commit by "
      "the agent. Without 'element_ids' the photos selected in the library (GUI host only). "
      "The photo open in the editor pastes first through the editor; 'editor' reports it. The "
      "other photos paste in a background task: wait for 'task_id' with tasks.wait; its "
      "'result' lists the pasted photos and each failed photo with the reason. Busy while "
      "another Paste or an editor save runs.");
  paste.changes_state = true;
  paste.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"package_id",
           QJsonObject{{"type", "string"}, {"description", "A package id from adjustments.copy."}}},
          {"element_ids", AutomationElementIdListSchema(
                              QStringLiteral("The photos; the library selection without it."))},
          {"version_name",
           QJsonObject{{"type", "string"},
                       {"description",
                        "Name of the new Versions. A name that a photo already has gets a "
                        "number."}}},
          {"note", QJsonObject{{"type", "string"},
                               {"description", "Why the agent pastes; stored with each commit."}}}},
      QJsonArray{"package_id"});
  paste.result_schema = AutomationObjectSchema(
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
  paste.handler = [host, store, registry_ptr, selection_available](const QJsonObject& params,
                                                                   AutomationReply    reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    const QString package_id = params.value("package_id").toString();
    const auto    package    = store->packages_.find(package_id);
    if (package == store->packages_.end()) {
      SendAutomationParamError(
          reply, QStringLiteral("/package_id"),
          QStringLiteral("unknown package id: %1; adjustments.copy makes one").arg(package_id));
      return;
    }
    QString version_name = ui::Tr("Pasted Adjustments");
    if (params.contains("version_name")) {
      std::string reason;
      const auto  name =
          NormalizeVersionName(params.value("version_name").toString().toStdString(), &reason);
      if (!name.has_value()) {
        SendAutomationParamError(reply, QStringLiteral("/version_name"),
                                 QString::fromStdString(reason));
        return;
      }
      version_name = QString::fromStdString(*name);
    }

    QJsonObject target_params{{"element_ids", params.value("element_ids")}};
    if (!params.contains("element_ids")) {
      const auto selected = SelectedPhotoIds(host, selection_available, reply);
      if (!selected.has_value()) {
        return;
      }
      target_params.insert("element_ids", *selected);
    }
    const auto rows = ResolveAutomationElementRows(*project, target_params, reply);
    if (!rows.has_value()) {
      return;
    }

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

    std::vector<sl_element_id_t> ids;
    ids.reserve(rows->size());
    for (const auto& row : *rows) {
      ids.push_back(row.file_id_);
    }
    const EditCommitAttribution attribution{std::string(kEditCommitAuthorAgent),
                                            params.value("note").toString().toStdString()};
    PasteAnswer                 answer;
    answer.target_count_                = static_cast<int>(ids.size());

    // The editor session owns the history of the photo it has open, so that photo pastes
    // through the session queue, like Paste in the GUI. The others paste in the background.
    ui::EditorSessionController* editor = host->editor_session();
    const auto                   editor_target =
        editor != nullptr && editor->has_image()
                              ? std::find(ids.begin(), ids.end(), static_cast<sl_element_id_t>(editor->element_id()))
                              : ids.end();
    auto*                            wait   = new AutomationCommandWait(std::move(reply), host);
    // The store never removes a package, so the pointer stays valid for the session.
    const AdjustmentTransferPackage* copied = &package->second;
    if (editor_target == ids.end()) {
      StartLibraryPaste(registry_ptr, host, wait, *copied, std::move(ids), version_name,
                        attribution, std::move(answer));
      return;
    }
    const auto editor_id = *editor_target;
    ids.erase(editor_target);

    QPointer<AutomationCommandWait> guarded   = wait;
    auto                            operation = OperationWait::Begin(
        editor, wait,
        [guarded, host, registry_ptr, editor, editor_id, copied, ids, version_name, attribution,
         answer](std::optional<OperationFailure> failure) mutable {
          if (guarded == nullptr) {
            return;
          }
          answer.editor_ = QJsonObject{{"element_id", static_cast<double>(editor_id)},
                                                                  {"pasted", !failure.has_value()}};
          if (failure.has_value()) {
            answer.editor_.insert("message", failure->message_);
            StartLibraryPaste(registry_ptr, host, guarded, *copied, std::move(ids), version_name,
                                                         attribution, std::move(answer));
            return;
          }
          AfterQueuedChanges(guarded, [guarded, host, registry_ptr, editor, copied, ids,
                                       version_name, attribution, answer]() mutable {
            if (guarded == nullptr) {
              return;
            }
            // Paste checks out the new Version, so the active Version is the pasted one.
            answer.editor_.insert(
                "version_id",
                QString::fromStdString(editor->history_snapshot().active_version_id.ToString()));
            StartLibraryPaste(registry_ptr, host, guarded, *copied, std::move(ids), version_name,
                                                         attribution, std::move(answer));
          });
        },
        OperationEnd::AfterSaveCheckpoint);
    const auto result = editor->RequestPasteAdjustmentPackage(*copied, version_name, attribution);
    if (EditorSessionResultIsFailure(result.kind)) {
      operation->Fail(OperationFailure{AutomationErrorCode::Rejected,
                                       result.message.empty()
                                           ? QStringLiteral("The editor cannot paste now.")
                                           : QString::fromStdString(result.message)});
      return;
    }
    operation->Start(result.operation_id);
  };
  RegisterOrFail(registry, std::move(paste), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
