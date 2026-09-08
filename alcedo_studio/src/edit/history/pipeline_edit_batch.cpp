//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/pipeline_edit_batch.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "edit/history/pipeline_edit_change.hpp"
#include "edit/history/pipeline_edit_json.hpp"
#include "edit/mask/brush_stroke.hpp"

namespace alcedo {

using pipeline_edit_json::Fail;
using pipeline_edit_json::RejectNonFiniteNumbers;
using pipeline_edit_json::RequireCanonicalDump;
using pipeline_edit_json::RequireExactObjectKeys;
using pipeline_edit_json::RequireNonEmptyString;
using pipeline_edit_json::RequireObject;
using pipeline_edit_json::RequireString;
using pipeline_edit_json::TranslationVectorToJson;

namespace {

auto StringArg(const nlohmann::json& args, const char* key) -> std::string {
  if (!args.contains(key) || !args.at(key).is_string()) {
    return {};
  }
  return args.at(key).get<std::string>();
}

}  // namespace

auto PipelineEditOperationKindText(PipelineEditOperationKind kind) -> std::string_view {
  switch (kind) {
    case PipelineEditOperationKind::SetParameter:
      return "set_parameter";
    case PipelineEditOperationKind::SetNodeEnabled:
      return "set_node_enabled";
    case PipelineEditOperationKind::SetNodeMix:
      return "set_node_mix";
    case PipelineEditOperationKind::RenameColorGrade:
      return "rename_color_grade";
    case PipelineEditOperationKind::AddColorGrade:
      return "add_color_grade";
    case PipelineEditOperationKind::RemoveColorGrade:
      return "remove_color_grade";
    case PipelineEditOperationKind::ReconnectColorGrade:
      return "reconnect_color_grade";
    case PipelineEditOperationKind::AddMask:
      return "add_mask";
    case PipelineEditOperationKind::RemoveMask:
      return "remove_mask";
    case PipelineEditOperationKind::ReplaceMaskSource:
      return "replace_mask_source";
    case PipelineEditOperationKind::ReplaceMaskAsset:
      return "replace_mask_asset";
    case PipelineEditOperationKind::SetMaskField:
      return "set_mask_field";
    case PipelineEditOperationKind::AppendBrushStroke:
      return "append_brush_stroke";
    case PipelineEditOperationKind::RemoveBrushStroke:
      return "remove_brush_stroke";
    case PipelineEditOperationKind::InsertBrushStroke:
      return "insert_brush_stroke";
    case PipelineEditOperationKind::SetBrushTranslation:
      return "set_brush_translation";
    case PipelineEditOperationKind::Paste:
      return "paste";
    case PipelineEditOperationKind::EditNodeGraph:
      return "edit_node_graph";
  }
  Fail("PipelineEditOperationKind: unknown enum value");
}

auto PipelineEditOperationKindFromText(std::string_view text) -> PipelineEditOperationKind {
  if (text == "set_parameter") {
    return PipelineEditOperationKind::SetParameter;
  }
  if (text == "set_node_enabled") {
    return PipelineEditOperationKind::SetNodeEnabled;
  }
  if (text == "set_node_mix") {
    return PipelineEditOperationKind::SetNodeMix;
  }
  if (text == "rename_color_grade") {
    return PipelineEditOperationKind::RenameColorGrade;
  }
  if (text == "add_color_grade") {
    return PipelineEditOperationKind::AddColorGrade;
  }
  if (text == "remove_color_grade") {
    return PipelineEditOperationKind::RemoveColorGrade;
  }
  if (text == "reconnect_color_grade") {
    return PipelineEditOperationKind::ReconnectColorGrade;
  }
  if (text == "add_mask") {
    return PipelineEditOperationKind::AddMask;
  }
  if (text == "remove_mask") {
    return PipelineEditOperationKind::RemoveMask;
  }
  if (text == "replace_mask_source") {
    return PipelineEditOperationKind::ReplaceMaskSource;
  }
  if (text == "replace_mask_asset") {
    return PipelineEditOperationKind::ReplaceMaskAsset;
  }
  if (text == "set_mask_field") {
    return PipelineEditOperationKind::SetMaskField;
  }
  if (text == "append_brush_stroke") {
    return PipelineEditOperationKind::AppendBrushStroke;
  }
  if (text == "remove_brush_stroke") {
    return PipelineEditOperationKind::RemoveBrushStroke;
  }
  if (text == "insert_brush_stroke") {
    return PipelineEditOperationKind::InsertBrushStroke;
  }
  if (text == "set_brush_translation") {
    return PipelineEditOperationKind::SetBrushTranslation;
  }
  if (text == "paste") {
    return PipelineEditOperationKind::Paste;
  }
  if (text == "edit_node_graph") {
    return PipelineEditOperationKind::EditNodeGraph;
  }
  Fail("PipelineEditOperationKind: unknown operation_kind '" + std::string{text} + "'");
}

auto PipelineEditChangeKindText(PipelineEditChangeKind kind) -> std::string_view {
  switch (kind) {
    case PipelineEditChangeKind::SetParameter:
      return "set_parameter";
    case PipelineEditChangeKind::SetNodeEnabled:
      return "set_node_enabled";
    case PipelineEditChangeKind::SetNodeMix:
      return "set_node_mix";
    case PipelineEditChangeKind::RenameColorGrade:
      return "rename_color_grade";
    case PipelineEditChangeKind::AddColorGrade:
      return "add_color_grade";
    case PipelineEditChangeKind::RemoveColorGrade:
      return "remove_color_grade";
    case PipelineEditChangeKind::ReconnectColorGrade:
      return "reconnect_color_grade";
    case PipelineEditChangeKind::AddMask:
      return "add_mask";
    case PipelineEditChangeKind::RemoveMask:
      return "remove_mask";
    case PipelineEditChangeKind::ReplaceMaskSource:
      return "replace_mask_source";
    case PipelineEditChangeKind::ReplaceMaskAsset:
      return "replace_mask_asset";
    case PipelineEditChangeKind::SetMaskField:
      return "set_mask_field";
    case PipelineEditChangeKind::AppendBrushStroke:
      return "append_brush_stroke";
    case PipelineEditChangeKind::RemoveBrushStroke:
      return "remove_brush_stroke";
    case PipelineEditChangeKind::InsertBrushStroke:
      return "insert_brush_stroke";
    case PipelineEditChangeKind::SetBrushTranslation:
      return "set_brush_translation";
    case PipelineEditChangeKind::NodeGraphTopologyChange:
      return "node_graph_topology_change";
  }
  Fail("PipelineEditChangeKind: unknown enum value");
}

auto PipelineEditChangeKindOf(const PipelineEditChange& change) -> PipelineEditChangeKind {
  return std::visit(
      [](const auto& typed) {
        using Typed = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Typed, SetParameterChange>) {
          return PipelineEditChangeKind::SetParameter;
        } else if constexpr (std::is_same_v<Typed, SetNodeEnabledChange>) {
          return PipelineEditChangeKind::SetNodeEnabled;
        } else if constexpr (std::is_same_v<Typed, SetNodeMixChange>) {
          return PipelineEditChangeKind::SetNodeMix;
        } else if constexpr (std::is_same_v<Typed, RenameColorGradeChange>) {
          return PipelineEditChangeKind::RenameColorGrade;
        } else if constexpr (std::is_same_v<Typed, AddColorGradeChange>) {
          return PipelineEditChangeKind::AddColorGrade;
        } else if constexpr (std::is_same_v<Typed, RemoveColorGradeChange>) {
          return PipelineEditChangeKind::RemoveColorGrade;
        } else if constexpr (std::is_same_v<Typed, ReconnectColorGradeChange>) {
          return PipelineEditChangeKind::ReconnectColorGrade;
        } else if constexpr (std::is_same_v<Typed, AddMaskChange>) {
          return PipelineEditChangeKind::AddMask;
        } else if constexpr (std::is_same_v<Typed, RemoveMaskChange>) {
          return PipelineEditChangeKind::RemoveMask;
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskSourceChange>) {
          return PipelineEditChangeKind::ReplaceMaskSource;
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskAssetChange>) {
          return PipelineEditChangeKind::ReplaceMaskAsset;
        } else if constexpr (std::is_same_v<Typed, SetMaskFieldChange>) {
          return PipelineEditChangeKind::SetMaskField;
        } else if constexpr (std::is_same_v<Typed, AppendBrushStrokeChange>) {
          return PipelineEditChangeKind::AppendBrushStroke;
        } else if constexpr (std::is_same_v<Typed, RemoveBrushStrokeChange>) {
          return PipelineEditChangeKind::RemoveBrushStroke;
        } else if constexpr (std::is_same_v<Typed, InsertBrushStrokeChange>) {
          return PipelineEditChangeKind::InsertBrushStroke;
        } else if constexpr (std::is_same_v<Typed, SetBrushTranslationChange>) {
          return PipelineEditChangeKind::SetBrushTranslation;
        } else if constexpr (std::is_same_v<Typed, NodeGraphTopologyChange>) {
          return PipelineEditChangeKind::NodeGraphTopologyChange;
        } else {
          Fail("PipelineEditChange: unhandled typed change");
        }
      },
      change);
}

auto IsPipelineEditBatchJson(const nlohmann::json& json) -> bool {
  return json.is_object() && json.contains("batch_format_version");
}

void PipelineEditBatch::Validate() const {
  if (batch_format_version != kPipelineEditBatchFormatVersion) {
    Fail("PipelineEditBatch: unsupported batch_format_version");
  }
  if (changes.empty()) {
    Fail("PipelineEditBatch: changes must not be empty");
  }
  if (presentation_key.empty()) {
    Fail("PipelineEditBatch: presentation_key must not be empty");
  }
  RequireObject(presentation_args, "PipelineEditBatch presentation_args");
  RejectNonFiniteNumbers(presentation_args, "PipelineEditBatch presentation_args");
  for (const auto& change : changes) {
    ValidatePipelineEditChange(change);
    if (!PipelineEditChangeCompatible(operation_kind, PipelineEditChangeKindOf(change))) {
      Fail("PipelineEditBatch: change kind is incompatible with operation_kind");
    }
  }
}

auto PipelineEditBatch::Make(PipelineEditOperationKind       operation_kind,
                             std::vector<PipelineEditChange> changes, std::string presentation_key,
                             nlohmann::json presentation_args) -> PipelineEditBatch {
  PipelineEditBatch batch;
  batch.batch_format_version = kPipelineEditBatchFormatVersion;
  batch.operation_kind       = operation_kind;
  batch.changes              = std::move(changes);
  batch.presentation_key     = std::move(presentation_key);
  batch.presentation_args    = std::move(presentation_args);
  batch.Validate();
  return batch;
}

auto PipelineEditBatch::CanonicalJSON() const -> nlohmann::json {
  Validate();
  nlohmann::json change_array = nlohmann::json::array();
  for (const auto& change : changes) {
    change_array.push_back(EncodePipelineEditChange(change));
  }
  return nlohmann::json{
      {"batch_format_version", batch_format_version},
      {"changes", std::move(change_array)},
      {"operation_kind", std::string{PipelineEditOperationKindText(operation_kind)}},
      {"presentation_args", presentation_args},
      {"presentation_key", presentation_key}};
}

auto PipelineEditBatch::FromJSON(const nlohmann::json& json) -> PipelineEditBatch {
  if (json.is_object() && json.contains("kind") && json.at("kind").is_string()) {
    const auto kind = json.at("kind").get<std::string>();
    if (kind == "edit" || kind == "merge") {
      Fail("PipelineEditBatch: ordinary and merge payloads are rejected");
    }
  }
  RequireExactObjectKeys(json,
                         {"batch_format_version", "changes", "operation_kind", "presentation_args",
                          "presentation_key"},
                         "PipelineEditBatch");
  if (!json.at("batch_format_version").is_number_unsigned() &&
      !json.at("batch_format_version").is_number_integer()) {
    Fail("PipelineEditBatch: batch_format_version must be an integer");
  }
  if (json.at("batch_format_version").get<std::uint32_t>() != kPipelineEditBatchFormatVersion) {
    Fail("PipelineEditBatch: unsupported batch_format_version");
  }
  if (!json.at("changes").is_array()) {
    Fail("PipelineEditBatch: changes must be an array");
  }
  PipelineEditBatch batch;
  batch.batch_format_version = kPipelineEditBatchFormatVersion;
  batch.operation_kind =
      PipelineEditOperationKindFromText(RequireString(json, "operation_kind", "PipelineEditBatch"));
  batch.presentation_key  = RequireNonEmptyString(json, "presentation_key", "PipelineEditBatch");
  batch.presentation_args = json.at("presentation_args");
  batch.changes.reserve(json.at("changes").size());
  for (const auto& change_json : json.at("changes")) {
    batch.changes.push_back(DecodePipelineEditChange(change_json));
  }
  batch.Validate();
  RequireCanonicalDump(json, batch.CanonicalJSON(), "PipelineEditBatch");
  return batch;
}

auto OrderedChangesForApply(const PipelineEditBatch& batch, PipelineEditApplyDirection direction)
    -> std::vector<PipelineEditChange> {
  batch.Validate();
  if (direction == PipelineEditApplyDirection::Forward) {
    return batch.changes;
  }
  std::vector<PipelineEditChange> reversed = batch.changes;
  std::reverse(reversed.begin(), reversed.end());
  return reversed;
}

auto ProjectPipelineEditHistory(const PipelineEditBatch& batch) -> PipelineEditHistoryProjection {
  batch.Validate();
  PipelineEditHistoryProjection row;
  row.operation_kind    = batch.operation_kind;
  row.presentation_key  = batch.presentation_key;
  row.presentation_args = batch.presentation_args;
  row.node_display_name = StringArg(batch.presentation_args, "node_display_name");
  row.mask_display_name = StringArg(batch.presentation_args, "mask_display_name");
  const auto& change    = batch.changes.front();
  std::visit(
      [&row](const auto& typed) {
        using Typed = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Typed, SetParameterChange>) {
          row.node_id                = std::string{typed.target.node_id.Value()};
          row.adjustment_instance_id = std::string{typed.target.adjustment_instance_id.Value()};
          row.mask_id                = std::string{typed.target.mask_id.Value()};
          row.field_key              = typed.target.field_key;
          row.before_display_value   = typed.before_value;
          row.after_display_value    = typed.after_value;
        } else if constexpr (std::is_same_v<Typed, SetNodeEnabledChange>) {
          row.node_id              = std::string{typed.node_id.Value()};
          row.before_display_value = typed.before_enabled;
          row.after_display_value  = typed.after_enabled;
        } else if constexpr (std::is_same_v<Typed, SetNodeMixChange>) {
          row.node_id              = std::string{typed.node_id.Value()};
          row.before_display_value = typed.before_mix;
          row.after_display_value  = typed.after_mix;
        } else if constexpr (std::is_same_v<Typed, RenameColorGradeChange>) {
          row.node_id = std::string{typed.node_id.Value()};
          if (row.node_display_name.empty()) {
            row.node_display_name = typed.before_display_name;
          }
          row.before_display_value = typed.before_display_name;
          row.after_display_value  = typed.after_display_name;
        } else if constexpr (std::is_same_v<Typed, AddColorGradeChange> ||
                             std::is_same_v<Typed, RemoveColorGradeChange>) {
          row.node_id = std::string{typed.node_id.Value()};
          if (row.node_display_name.empty()) {
            row.node_display_name = StringArg(typed.node, "display_name");
          }
          row.after_display_value = typed.node;
        } else if constexpr (std::is_same_v<Typed, ReconnectColorGradeChange>) {
          row.node_id = std::string{typed.node_id.Value()};
        } else if constexpr (std::is_same_v<Typed, AddMaskChange> ||
                             std::is_same_v<Typed, RemoveMaskChange>) {
          row.node_id = std::string{typed.node_id.Value()};
          row.mask_id = std::string{typed.mask_id.Value()};
          if (row.mask_display_name.empty()) {
            row.mask_display_name = StringArg(typed.mask, "display_name");
          }
          row.after_display_value = typed.mask;
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskSourceChange> ||
                             std::is_same_v<Typed, ReplaceMaskAssetChange>) {
          row.node_id              = std::string{typed.node_id.Value()};
          row.mask_id              = std::string{typed.mask_id.Value()};
          row.before_display_value = typed.before_source;
          row.after_display_value  = typed.after_source;
        } else if constexpr (std::is_same_v<Typed, SetMaskFieldChange>) {
          row.node_id              = std::string{typed.node_id.Value()};
          row.mask_id              = std::string{typed.mask_id.Value()};
          row.field_key            = typed.field_key;
          row.before_display_value = typed.before_value;
          row.after_display_value  = typed.after_value;
        } else if constexpr (std::is_same_v<Typed, AppendBrushStrokeChange> ||
                             std::is_same_v<Typed, InsertBrushStrokeChange>) {
          row.node_id             = std::string{typed.node_id.Value()};
          row.mask_id             = std::string{typed.mask_id.Value()};
          row.after_display_value = BrushStrokeToJson(typed.stroke);
        } else if constexpr (std::is_same_v<Typed, RemoveBrushStrokeChange>) {
          row.node_id              = std::string{typed.node_id.Value()};
          row.mask_id              = std::string{typed.mask_id.Value()};
          row.before_display_value = BrushStrokeToJson(typed.stroke);
        } else if constexpr (std::is_same_v<Typed, SetBrushTranslationChange>) {
          row.node_id              = std::string{typed.node_id.Value()};
          row.mask_id              = std::string{typed.mask_id.Value()};
          row.field_key            = "placement_translation";
          row.before_display_value = TranslationVectorToJson(typed.before);
          row.after_display_value  = TranslationVectorToJson(typed.after);
        } else if constexpr (std::is_same_v<Typed, NodeGraphTopologyChange>) {
          if (!typed.inserted_nodes.empty()) {
            row.node_id             = StringArg(typed.inserted_nodes.front().node, "id");
            row.node_display_name   = StringArg(typed.inserted_nodes.front().node, "display_name");
            row.after_display_value = typed.inserted_nodes.front().node;
          } else if (!typed.removed_nodes.empty()) {
            row.node_id              = StringArg(typed.removed_nodes.front().node, "id");
            row.node_display_name    = StringArg(typed.removed_nodes.front().node, "display_name");
            row.before_display_value = typed.removed_nodes.front().node;
          }
        }
      },
      change);
  if (row.node_id.empty()) {
    row.node_id = StringArg(batch.presentation_args, "node_id");
  }
  if (row.mask_id.empty()) {
    row.mask_id = StringArg(batch.presentation_args, "mask_id");
  }
  return row;
}

}  // namespace alcedo
