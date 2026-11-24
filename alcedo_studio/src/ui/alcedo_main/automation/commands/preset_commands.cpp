//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/adjustment_transfer_catalog.hpp"
#include "app/adjustment_transfer_package_builder.hpp"
#include "app/adjustment_transfer_types.hpp"
#include "app/preset_file.hpp"
#include "app/preset_library_service.hpp"
#include "edit/history/edit_commit_attribution.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_controller.hpp"
#include "ui/alcedo_main/automation/automation_agent_paste.hpp"
#include "ui/alcedo_main/automation/automation_command_support.hpp"
#include "ui/alcedo_main/automation/automation_preset_commands.hpp"

namespace alcedo::automation {
namespace {

void RegisterOrFail(AutomationCommandRegistry& registry, AutomationCommandSpec spec, bool* ok,
                    QString* error) {
  if (*ok && !registry.Register(std::move(spec), error)) {
    *ok = false;
  }
}

auto Text(const std::string& text) -> QString { return QString::fromStdString(text); }

/// The node rows of @p package: the node names and item names of the Copy dialog.
auto NodesJson(const AdjustmentTransferPackage& package) -> QJsonArray {
  QJsonArray nodes;
  for (const auto& node : SummarizeTransferPackage(package)) {
    QJsonArray items;
    for (const auto& item : node.item_names) {
      items.push_back(Text(item));
    }
    nodes.push_back(QJsonObject{{"name", Text(node.display_name)}, {"items", items}});
  }
  return nodes;
}

/// One preset of presets.list. With @p details, an available preset lists its items; a file that
/// fails a rule since the scan is not available, with the reason.
auto EntryJson(PresetLibraryService& library, const PresetEntry& entry, bool details)
    -> QJsonObject {
  QJsonObject json{{"preset_id", Text(entry.id)},
                   {"name", Text(entry.name)},
                   {"group", Text(entry.group)},
                   {"available", entry.available}};
  if (!entry.available) {
    json.insert("reason", Text(entry.reason));
    return json;
  }
  if (details) {
    const auto package = library.PackageFor(entry.id);
    if (package.Ok()) {
      json.insert("nodes", NodesJson(*package.package));
    } else {
      json.insert("available", false);
      json.insert("reason", Text(package.message));
    }
  }
  return json;
}

/// The answer of presets.list from the published inventory, in the order of the preset browser
/// (PresetListModel): the presets directly in the folder, then the groups.
auto ListJson(PresetLibraryService& library, bool details) -> QJsonObject {
  QJsonArray presets;
  for (const bool grouped : {false, true}) {
    for (const auto& entry : library.Entries()) {
      if (entry.group.empty() != grouped) {
        presets.push_back(EntryJson(library, entry, details));
      }
    }
  }
  QJsonObject result{{"folder", library.root_path()}, {"presets", presets}};
  if (!library.InventoryError().empty()) {
    result.insert("error", Text(library.InventoryError()));
  }
  return result;
}

/// The preset library, or nothing after a -32001 answer.
auto Library(ui::ApplicationModuleHost* host, AutomationReply& reply) -> PresetLibraryService* {
  auto* library = host->preset_library();
  if (library == nullptr) {
    SendAutomationOwnerError(reply, AutomationErrorCode::NotReady,
                             QStringLiteral("The preset library is not available."));
  }
  return library;
}

/// The Version of @p graph that @p params.version_id names, or the active Version without it.
/// Sends -32602 and returns nothing for an unknown id.
auto RequestedVersion(const CommitGraph& graph, const QJsonObject& params, AutomationReply& reply)
    -> std::optional<AdjustmentTransferVersionDescriptor> {
  const std::string id = params.value("version_id").toString().trimmed().toLower().toStdString();
  for (const auto& version : AdjustmentTransferCatalogService::ListVersions(graph)) {
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

/// The preset of presets.save: the package of the Version and the preset name. Sends the error
/// and returns nothing when the photo, the Version, or the name is refused.
struct SavedPreset {
  AdjustmentTransferPackage package;
  std::string               name;
};

auto BuildSavedPreset(ui::ApplicationModuleHost* host, ProjectService& project,
                      const QJsonObject& params, AutomationReply& reply)
    -> std::optional<SavedPreset> {
  const auto element_id = AutomationElementIdParam(params, QStringLiteral("element_id"), reply);
  if (!element_id.has_value()) {
    return std::nullopt;
  }
  const QJsonObject ids{{"element_ids", QJsonArray{static_cast<double>(*element_id)}}};
  if (!ResolveAutomationElementRows(project, ids, reply).has_value()) {
    return std::nullopt;
  }
  // The read of Copy Adjustments: the photo open in the editor is read through the session.
  std::shared_ptr<const CommitGraph>      graph;
  std::shared_ptr<const PipelineDocument> root;
  QString                                 read_error;
  if (!host->adjustment_transfer()->ReadSourceHistory(*element_id, &graph, &root, &read_error)) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed, read_error);
    return std::nullopt;
  }
  const auto version = RequestedVersion(*graph, params, reply);
  if (!version.has_value()) {
    return std::nullopt;
  }
  std::string error;
  const auto  read =
      AdjustmentTransferCatalogService::ReadVersion(*graph, *root, version->version_id, &error);
  if (!read.has_value()) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Failed, Text(error));
    return std::nullopt;
  }
  std::string name = PresetNameFromLabel(version->display_name);
  if (params.contains("name")) {
    name = params.value("name").toString().toStdString();
    if (const auto refused = PresetNameError(name); !refused.empty()) {
      SendAutomationParamError(reply, QStringLiteral("/name"), Text(refused));
      return std::nullopt;
    }
  }
  // The items of a save by drag (plan D4): all but Crop and Rotate and Lens Correction.
  const auto selection = SelectPresetItems(read->document);
  if (selection.nodes.empty()) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Rejected,
                             QStringLiteral("The Version has no item to save."));
    return std::nullopt;
  }
  try {
    return SavedPreset{AdjustmentTransferPackageBuilder::Build(read->document, selection),
                       std::move(name)};
  } catch (const std::exception& e) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Rejected, QString::fromUtf8(e.what()));
    return std::nullopt;
  }
}

/// The package and the default Version name of presets.apply: a listed preset or a preset file
/// read one time. Sends the error and returns nothing when the preset is refused.
struct AppliedPreset {
  std::shared_ptr<const AdjustmentTransferPackage> package;
  QString                                          name;
};

auto ReadAppliedPreset(PresetLibraryService& library, const QJsonObject& params,
                       AutomationReply& reply) -> std::optional<AppliedPreset> {
  const bool by_id   = params.contains("preset_id");
  const bool by_file = params.contains("file");
  if (by_id == by_file) {
    SendAutomationParamError(reply, QString(),
                             QStringLiteral("give either preset_id or file, not both"));
    return std::nullopt;
  }
  if (by_file) {
    const std::filesystem::path path(params.value("file").toString().toStdWString());
    // The one reader of a preset file with every rule of the plan, section 3 (R8: the file does
    // not enter the folder or the cache).
    auto                        read = ReadPresetFile(path);
    if (!read.Ok()) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Rejected, Text(read.message));
      return std::nullopt;
    }
    return AppliedPreset{
        std::make_shared<const AdjustmentTransferPackage>(std::move(*read.package)),
        QString::fromStdWString(path.stem().wstring())};
  }
  const std::string id    = params.value("preset_id").toString().toStdString();
  const auto*       entry = library.FindEntry(id);
  if (entry == nullptr) {
    SendAutomationParamError(reply, QStringLiteral("/preset_id"),
                             QStringLiteral("unknown preset id; presets.list lists them"));
    return std::nullopt;
  }
  if (!entry->available) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Rejected, Text(entry->reason));
    return std::nullopt;
  }
  const QString name    = Text(entry->name);
  auto          package = library.PackageFor(id);
  if (!package.Ok()) {
    SendAutomationOwnerError(reply, AutomationErrorCode::Rejected, Text(package.message));
    return std::nullopt;
  }
  return AppliedPreset{std::move(package.package), name};
}

}  // namespace

auto RegisterAutomationPresetCommands(AutomationCommandRegistry& registry,
                                      ui::ApplicationModuleHost* host, const QString& host_mode,
                                      QString* error) -> bool {
  bool              ok                  = true;
  const bool        selection_available = host_mode != QStringLiteral("headless");
  auto* const       registry_ptr        = &registry;
  const QJsonObject text_schema{{"type", "string"}};
  const QJsonObject node_schema = AutomationObjectSchema(
      QJsonObject{{"name", text_schema},
                  {"items", QJsonObject{{"type", "array"}, {"items", text_schema}}}},
      QJsonArray{"name", "items"});
  const QJsonObject preset_schema = AutomationObjectSchema(
      QJsonObject{{"preset_id", text_schema},
                  {"name", text_schema},
                  {"group", text_schema},
                  {"available", QJsonObject{{"type", "boolean"}}},
                  {"reason", text_schema},
                  {"nodes", QJsonObject{{"type", "array"}, {"items", node_schema}}}},
      QJsonArray{"preset_id", "name", "group", "available"});

  AutomationCommandSpec list;
  list.method      = QStringLiteral("presets.list");
  list.description = QStringLiteral(
      "Lists the presets of the preset folder, like the preset browser: the presets directly in "
      "the folder first, then each subfolder (its 'group'). 'preset_id' is the path below the "
      "folder. A preset with 'available' false cannot be applied; 'reason' says why. With "
      "'details' each available preset also lists its items by node. Scans the folder first, "
      "like the preset browser when it opens, so a file copied into the folder is listed. Needs "
      "no open project.");
  list.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"details",
       QJsonObject{{"type", "boolean"},
                   {"description", "List the items of each preset; reads each file."}}}});
  list.result_schema = AutomationObjectSchema(
      QJsonObject{{"folder", text_schema},
                  {"presets", QJsonObject{{"type", "array"}, {"items", preset_schema}}},
                  {"error", text_schema}},
      QJsonArray{"folder", "presets"});
  list.handler = [host](const QJsonObject& params, AutomationReply reply) {
    auto* library = Library(host, reply);
    if (library == nullptr) {
      return;
    }
    const bool          details = params.value("details").toBool(false);
    // The service has no file watcher (plan D9): like the preset browser when it opens, the list
    // scans the folder first and answers with the inventory of that scan.
    const std::uint64_t before  = library->LastSubmittedRequest();
    library->Refresh();
    const std::uint64_t request = library->LastSubmittedRequest();
    if (request == before) {
      // The service is shut down and queues nothing: the published inventory.
      reply.SendResult(ListJson(*library, details));
      return;
    }
    auto* wait = new AutomationCommandWait(std::move(reply), host);
    QObject::connect(library, &PresetLibraryService::OperationFinished, wait,
                     [wait, library, request, details](PresetLibraryService::Operation,
                                                       PresetLibraryService::Status) {
                       if (library->LastResult().request == request) {
                         wait->SendResult(ListJson(*library, details));
                       }
                     });
  };
  RegisterOrFail(registry, std::move(list), &ok, error);

  AutomationCommandSpec save;
  save.method      = QStringLiteral("presets.save");
  save.description = QStringLiteral(
      "Saves a Version of a photo (the active Version without 'version_id') as a preset in the "
      "preset folder, like a drag of the Version onto the preset part of the Versions panel: "
      "every transferable item except crop_rotate and lens_calib. The name is 'name', or the "
      "Version name made a valid file name; a name that exists gets a number. Answers when the "
      "file is written.");
  save.changes_state = true;
  save.params_schema = AutomationClosedParamsSchema(
      QJsonObject{
          {"element_id", AutomationElementIdSchema(1, QStringLiteral("The photo (library.list)."))},
          {"version_id", QJsonObject{{"type", "string"},
                                     {"description", "Hex id of a Version (adjustments.items)."}}},
          {"name", QJsonObject{{"type", "string"},
                               {"description",
                                "Preset name: no path separator, control character, or one of "
                                "<>:\"|?*, at most 120 characters."}}}},
      QJsonArray{"element_id"});
  save.result_schema = AutomationObjectSchema(
      QJsonObject{{"preset_id", text_schema},
                  {"name", text_schema},
                  {"nodes", QJsonObject{{"type", "array"}, {"items", node_schema}}}},
      QJsonArray{"preset_id", "name", "nodes"});
  save.handler = [host](const QJsonObject& params, AutomationReply reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    auto* library = Library(host, reply);
    if (library == nullptr) {
      return;
    }
    auto saved = BuildSavedPreset(host, *project, params, reply);
    if (!saved.has_value()) {
      return;
    }
    if (library->Save(saved->package, saved->name) != PresetLibraryService::Status::kOk) {
      SendAutomationOwnerError(reply, AutomationErrorCode::Rejected,
                               Text(library->LastResult().message));
      return;
    }
    // The worker runs the operations in order; the answer waits for this request, not for a
    // save that the GUI queued before it.
    const std::uint64_t request = library->LastSubmittedRequest();
    const QJsonArray    nodes   = NodesJson(saved->package);
    auto*               wait    = new AutomationCommandWait(std::move(reply), host);
    QObject::connect(
        library, &PresetLibraryService::OperationFinished, wait,
        [wait, library, request, nodes](PresetLibraryService::Operation,
                                        PresetLibraryService::Status status) {
          const auto& last = library->LastResult();
          if (last.request != request) {
            return;
          }
          if (status != PresetLibraryService::Status::kOk || last.affected_ids.empty()) {
            wait->SendOwnerError(AutomationErrorCode::Failed, Text(last.message));
            return;
          }
          const auto& id    = last.affected_ids.front();
          const auto* entry = library->FindEntry(id);
          wait->SendResult(QJsonObject{{"preset_id", Text(id)},
                                       {"name", entry != nullptr ? Text(entry->name) : Text(id)},
                                       {"nodes", nodes}});
        });
  };
  RegisterOrFail(registry, std::move(save), &ok, error);

  AutomationCommandSpec apply;
  apply.method      = QStringLiteral("presets.apply");
  apply.description = QStringLiteral(
      "Applies a preset onto photos, like Apply Preset...: each photo gets one new Version, "
      "named 'version_name' or after the preset, with one commit by the agent. Give "
      "'preset_id' (presets.list) or 'file', a preset file that is applied one time and does "
      "not enter the preset folder. Without 'element_ids' the photos selected in the library "
      "(GUI host only). The photo open in the editor applies first through the editor; "
      "'editor' reports it. The other photos apply in a background Paste task: wait for "
      "'task_id' with tasks.wait; its 'result' lists the applied photos and each failed photo "
      "with the reason. Busy while another Paste or an editor save runs.");
  apply.changes_state = true;
  apply.params_schema = AutomationClosedParamsSchema(QJsonObject{
      {"preset_id",
       QJsonObject{{"type", "string"}, {"description", "A preset id from presets.list."}}},
      {"file", QJsonObject{{"type", "string"},
                           {"description",
                            "Path of a .alcpreset file to apply one time, instead of "
                            "preset_id."}}},
      {"element_ids", AutomationElementIdListSchema(
                          QStringLiteral("The photos; the library selection without it."))},
      {"version_name", AgentPasteVersionNameSchema()},
      {"note", AgentPasteNoteSchema()}});
  apply.result_schema = AgentPasteResultSchema();
  apply.handler       = [host, registry_ptr, selection_available](const QJsonObject& params,
                                                            AutomationReply    reply) {
    const auto project = AutomationEnteredProject(host, reply);
    if (!project) {
      return;
    }
    auto* library = Library(host, reply);
    if (library == nullptr) {
      return;
    }
    // One read of the preset for every photo (plan 6.2).
    auto preset = ReadAppliedPreset(*library, params, reply);
    if (!preset.has_value()) {
      return;
    }
    const auto version_name = AgentPasteVersionName(params, preset->name, reply);
    if (!version_name.has_value()) {
      return;
    }
    auto ids = AgentPasteTargets(host, *project, params, selection_available, reply);
    if (!ids.has_value()) {
      return;
    }
    const EditCommitAttribution attribution{std::string(kEditCommitAuthorAgent),
                                            params.value("note").toString().toStdString()};
    StartAgentPaste(registry_ptr, host, std::move(reply), std::move(preset->package),
                          std::move(*ids), *version_name, attribution);
  };
  RegisterOrFail(registry, std::move(apply), &ok, error);

  return ok;
}

}  // namespace alcedo::automation
