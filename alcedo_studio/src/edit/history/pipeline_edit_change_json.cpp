//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "edit/history/pipeline_edit_change.hpp"
#include "edit/history/pipeline_edit_json.hpp"
#include "edit/mask/brush_stroke.hpp"

namespace alcedo {

using pipeline_edit_json::CanonicalColorGradeNodeJson;
using pipeline_edit_json::CanonicalMaskJson;
using pipeline_edit_json::CanonicalMaskSourceJson;
using pipeline_edit_json::EdgeFromJson;
using pipeline_edit_json::EdgeToJson;
using pipeline_edit_json::Fail;
using pipeline_edit_json::NodeKindFromText;
using pipeline_edit_json::NodeKindText;
using pipeline_edit_json::RequireBool;
using pipeline_edit_json::RequiredIdFromJson;
using pipeline_edit_json::RequireExactObjectKeys;
using pipeline_edit_json::RequireNonEmptyString;
using pipeline_edit_json::RequireNonNegativeUint32;
using pipeline_edit_json::RequireNormalizedMix;
using pipeline_edit_json::RequireObject;
using pipeline_edit_json::RequirePositiveUint64;
using pipeline_edit_json::RequireString;
using pipeline_edit_json::StrokeFromCanonicalJson;
using pipeline_edit_json::TargetFromJson;
using pipeline_edit_json::TargetToJson;
using pipeline_edit_json::TranslationVectorFromJson;
using pipeline_edit_json::TranslationVectorToJson;

auto PipelineEditChangeKindFromText(std::string_view text) -> PipelineEditChangeKind {
  if (text == "set_parameter") {
    return PipelineEditChangeKind::SetParameter;
  }
  if (text == "set_node_enabled") {
    return PipelineEditChangeKind::SetNodeEnabled;
  }
  if (text == "set_node_mix") {
    return PipelineEditChangeKind::SetNodeMix;
  }
  if (text == "rename_color_grade") {
    return PipelineEditChangeKind::RenameColorGrade;
  }
  if (text == "add_color_grade") {
    return PipelineEditChangeKind::AddColorGrade;
  }
  if (text == "remove_color_grade") {
    return PipelineEditChangeKind::RemoveColorGrade;
  }
  if (text == "reconnect_color_grade") {
    return PipelineEditChangeKind::ReconnectColorGrade;
  }
  if (text == "node_graph_topology_change") {
    return PipelineEditChangeKind::NodeGraphTopologyChange;
  }
  if (text == "add_mask") {
    return PipelineEditChangeKind::AddMask;
  }
  if (text == "remove_mask") {
    return PipelineEditChangeKind::RemoveMask;
  }
  if (text == "replace_mask_source") {
    return PipelineEditChangeKind::ReplaceMaskSource;
  }
  if (text == "replace_mask_asset") {
    return PipelineEditChangeKind::ReplaceMaskAsset;
  }
  if (text == "set_mask_field") {
    return PipelineEditChangeKind::SetMaskField;
  }
  if (text == "append_brush_stroke") {
    return PipelineEditChangeKind::AppendBrushStroke;
  }
  if (text == "remove_brush_stroke") {
    return PipelineEditChangeKind::RemoveBrushStroke;
  }
  if (text == "insert_brush_stroke") {
    return PipelineEditChangeKind::InsertBrushStroke;
  }
  if (text == "set_brush_translation") {
    return PipelineEditChangeKind::SetBrushTranslation;
  }
  Fail("PipelineEditChange: unknown kind '" + std::string{text} + "'");
}

auto PipelineEditChangeCompatible(PipelineEditOperationKind operation,
                                  PipelineEditChangeKind    change) -> bool {
  if (operation == PipelineEditOperationKind::Paste) {
    return change != PipelineEditChangeKind::NodeGraphTopologyChange;
  }
  switch (operation) {
    case PipelineEditOperationKind::SetParameter:
      return change == PipelineEditChangeKind::SetParameter;
    case PipelineEditOperationKind::SetNodeEnabled:
      return change == PipelineEditChangeKind::SetNodeEnabled;
    case PipelineEditOperationKind::SetNodeMix:
      return change == PipelineEditChangeKind::SetNodeMix;
    case PipelineEditOperationKind::RenameColorGrade:
      return change == PipelineEditChangeKind::RenameColorGrade;
    case PipelineEditOperationKind::AddColorGrade:
      return change == PipelineEditChangeKind::AddColorGrade;
    case PipelineEditOperationKind::RemoveColorGrade:
      return change == PipelineEditChangeKind::RemoveColorGrade;
    case PipelineEditOperationKind::ReconnectColorGrade:
      return change == PipelineEditChangeKind::ReconnectColorGrade;
    case PipelineEditOperationKind::AddMask:
      return change == PipelineEditChangeKind::AddMask;
    case PipelineEditOperationKind::RemoveMask:
      return change == PipelineEditChangeKind::RemoveMask;
    case PipelineEditOperationKind::ReplaceMaskSource:
      return change == PipelineEditChangeKind::ReplaceMaskSource;
    case PipelineEditOperationKind::ReplaceMaskAsset:
      return change == PipelineEditChangeKind::ReplaceMaskAsset;
    case PipelineEditOperationKind::SetMaskField:
      return change == PipelineEditChangeKind::SetMaskField;
    case PipelineEditOperationKind::AppendBrushStroke:
      return change == PipelineEditChangeKind::AppendBrushStroke;
    case PipelineEditOperationKind::RemoveBrushStroke:
      return change == PipelineEditChangeKind::RemoveBrushStroke;
    case PipelineEditOperationKind::InsertBrushStroke:
      return change == PipelineEditChangeKind::InsertBrushStroke;
    case PipelineEditOperationKind::SetBrushTranslation:
      return change == PipelineEditChangeKind::SetBrushTranslation;
    case PipelineEditOperationKind::Paste:
      return change != PipelineEditChangeKind::NodeGraphTopologyChange;
    case PipelineEditOperationKind::EditNodeGraph:
      return change == PipelineEditChangeKind::NodeGraphTopologyChange;
  }
  return false;
}

auto EncodePipelineEditChange(const PipelineEditChange& change) -> nlohmann::json {
  return std::visit(
      [](const auto& typed) -> nlohmann::json {
        using Typed = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Typed, SetParameterChange>) {
          return {{"after_enabled", typed.after_enabled},
                  {"after_value", typed.after_value},
                  {"before_enabled", typed.before_enabled},
                  {"before_value", typed.before_value},
                  {"kind", "set_parameter"},
                  {"target", TargetToJson(typed.target)}};
        } else if constexpr (std::is_same_v<Typed, SetNodeEnabledChange>) {
          return {{"after_enabled", typed.after_enabled},
                  {"before_enabled", typed.before_enabled},
                  {"kind", "set_node_enabled"},
                  {"node_id", std::string{typed.node_id.Value()}},
                  {"node_kind", std::string{NodeKindText(typed.node_kind)}}};
        } else if constexpr (std::is_same_v<Typed, SetNodeMixChange>) {
          return {{"after_mix", typed.after_mix},
                  {"before_mix", typed.before_mix},
                  {"kind", "set_node_mix"},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, RenameColorGradeChange>) {
          return {{"after_display_name", typed.after_display_name},
                  {"before_display_name", typed.before_display_name},
                  {"kind", "rename_color_grade"},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, AddColorGradeChange>) {
          return {
              {"after_next_color_grade_name_number", typed.after_next_color_grade_name_number},
              {"before_next_color_grade_name_number", typed.before_next_color_grade_name_number},
              {"incoming_edge", EdgeToJson(typed.incoming_edge)},
              {"kind", "add_color_grade"},
              {"node", typed.node},
              {"node_id", std::string{typed.node_id.Value()}},
              {"outgoing_edge", EdgeToJson(typed.outgoing_edge)},
              {"predecessor_id", std::string{typed.predecessor_id.Value()}},
              {"successor_id", std::string{typed.successor_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, RemoveColorGradeChange>) {
          return {{"bridge_edge", EdgeToJson(typed.bridge_edge)},
                  {"kind", "remove_color_grade"},
                  {"node", typed.node},
                  {"node_id", std::string{typed.node_id.Value()}},
                  {"predecessor_id", std::string{typed.predecessor_id.Value()}},
                  {"removed_incoming_edge", EdgeToJson(typed.removed_incoming_edge)},
                  {"removed_outgoing_edge", EdgeToJson(typed.removed_outgoing_edge)},
                  {"successor_id", std::string{typed.successor_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, ReconnectColorGradeChange>) {
          return {{"after_incoming_edge", EdgeToJson(typed.after_incoming_edge)},
                  {"after_outgoing_edge", EdgeToJson(typed.after_outgoing_edge)},
                  {"after_predecessor_id", std::string{typed.after_predecessor_id.Value()}},
                  {"after_successor_id", std::string{typed.after_successor_id.Value()}},
                  {"before_incoming_edge", EdgeToJson(typed.before_incoming_edge)},
                  {"before_outgoing_edge", EdgeToJson(typed.before_outgoing_edge)},
                  {"before_predecessor_id", std::string{typed.before_predecessor_id.Value()}},
                  {"before_successor_id", std::string{typed.before_successor_id.Value()}},
                  {"kind", "reconnect_color_grade"},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, AddMaskChange>) {
          return {{"display_index", typed.display_index},
                  {"kind", "add_mask"},
                  {"mask", typed.mask},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, RemoveMaskChange>) {
          return {{"display_index", typed.display_index},
                  {"kind", "remove_mask"},
                  {"mask", typed.mask},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskSourceChange>) {
          return {{"after_source", typed.after_source},
                  {"before_source", typed.before_source},
                  {"kind", "replace_mask_source"},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskAssetChange>) {
          return {{"after_source", typed.after_source},
                  {"before_source", typed.before_source},
                  {"kind", "replace_mask_asset"},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, SetMaskFieldChange>) {
          return {{"after_value", typed.after_value},
                  {"before_value", typed.before_value},
                  {"field_key", typed.field_key},
                  {"kind", "set_mask_field"},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, AppendBrushStrokeChange>) {
          return {{"kind", "append_brush_stroke"},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}},
                  {"stroke", BrushStrokeToJson(typed.stroke)}};
        } else if constexpr (std::is_same_v<Typed, RemoveBrushStrokeChange>) {
          return {{"index", typed.index},
                  {"kind", "remove_brush_stroke"},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}},
                  {"stroke", BrushStrokeToJson(typed.stroke)},
                  {"stroke_id", std::string{typed.stroke_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, InsertBrushStrokeChange>) {
          return {{"index", typed.index},
                  {"kind", "insert_brush_stroke"},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}},
                  {"stroke", BrushStrokeToJson(typed.stroke)}};
        } else if constexpr (std::is_same_v<Typed, SetBrushTranslationChange>) {
          return {{"after", TranslationVectorToJson(typed.after)},
                  {"before", TranslationVectorToJson(typed.before)},
                  {"kind", "set_brush_translation"},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}}};
        } else if constexpr (std::is_same_v<Typed, NodeGraphTopologyChange>) {
          nlohmann::json inserted = nlohmann::json::array();
          for (const auto& item : typed.inserted_nodes) {
            inserted.push_back({{"final_node_index", item.final_node_index}, {"node", item.node}});
          }
          nlohmann::json removed = nlohmann::json::array();
          for (const auto& item : typed.removed_nodes) {
            removed.push_back(
                {{"node", item.node}, {"original_node_index", item.original_node_index}});
          }
          nlohmann::json disconnected = nlohmann::json::array();
          for (const auto& item : typed.disconnected_edges) {
            disconnected.push_back({{"edge", EdgeToJson(item.edge)},
                                    {"original_edge_index", item.original_edge_index}});
          }
          nlohmann::json connected = nlohmann::json::array();
          for (const auto& item : typed.connected_edges) {
            connected.push_back(
                {{"edge", EdgeToJson(item.edge)}, {"final_edge_index", item.final_edge_index}});
          }
          return {
              {"after_next_color_grade_name_number", typed.after_next_color_grade_name_number},
              {"before_next_color_grade_name_number", typed.before_next_color_grade_name_number},
              {"connected_edges", std::move(connected)},
              {"disconnected_edges", std::move(disconnected)},
              {"inserted_nodes", std::move(inserted)},
              {"kind", "node_graph_topology_change"},
              {"removed_nodes", std::move(removed)}};
        } else {
          Fail("PipelineEditChange: unhandled typed change");
        }
      },
      change);
}

auto DecodePipelineEditChange(const nlohmann::json& json) -> PipelineEditChange {
  RequireObject(json, "PipelineEditChange");
  if (!json.contains("kind") || !json.at("kind").is_string()) {
    Fail("PipelineEditChange: kind must be a string");
  }
  const auto kind = PipelineEditChangeKindFromText(json.at("kind").get<std::string>());
  switch (kind) {
    case PipelineEditChangeKind::SetParameter: {
      RequireExactObjectKeys(
          json,
          {"after_enabled", "after_value", "before_enabled", "before_value", "kind", "target"},
          "SetParameter");
      SetParameterChange change;
      change.target         = TargetFromJson(json.at("target"));
      change.before_value   = json.at("before_value");
      change.after_value    = json.at("after_value");
      change.before_enabled = RequireBool(json, "before_enabled", "SetParameter");
      change.after_enabled  = RequireBool(json, "after_enabled", "SetParameter");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::SetNodeEnabled: {
      RequireExactObjectKeys(json,
                             {"after_enabled", "before_enabled", "kind", "node_id", "node_kind"},
                             "SetNodeEnabled");
      SetNodeEnabledChange change;
      change.node_id        = NodeId{RequiredIdFromJson(json, "node_id", "SetNodeEnabled")};
      change.node_kind      = NodeKindFromText(RequireString(json, "node_kind", "SetNodeEnabled"));
      change.before_enabled = RequireBool(json, "before_enabled", "SetNodeEnabled");
      change.after_enabled  = RequireBool(json, "after_enabled", "SetNodeEnabled");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::SetNodeMix: {
      RequireExactObjectKeys(json, {"after_mix", "before_mix", "kind", "node_id"}, "SetNodeMix");
      SetNodeMixChange change;
      change.node_id    = NodeId{RequiredIdFromJson(json, "node_id", "SetNodeMix")};
      change.before_mix = RequireNormalizedMix(json, "before_mix", "SetNodeMix");
      change.after_mix  = RequireNormalizedMix(json, "after_mix", "SetNodeMix");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::RenameColorGrade: {
      RequireExactObjectKeys(json, {"after_display_name", "before_display_name", "kind", "node_id"},
                             "RenameColorGrade");
      RenameColorGradeChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "RenameColorGrade")};
      change.before_display_name =
          RequireNonEmptyString(json, "before_display_name", "RenameColorGrade");
      change.after_display_name =
          RequireNonEmptyString(json, "after_display_name", "RenameColorGrade");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::AddColorGrade: {
      RequireExactObjectKeys(json,
                             {"after_next_color_grade_name_number",
                              "before_next_color_grade_name_number", "incoming_edge", "kind",
                              "node", "node_id", "outgoing_edge", "predecessor_id", "successor_id"},
                             "AddColorGrade");
      AddColorGradeChange change;
      change.node_id        = NodeId{RequiredIdFromJson(json, "node_id", "AddColorGrade")};
      change.predecessor_id = NodeId{RequiredIdFromJson(json, "predecessor_id", "AddColorGrade")};
      change.successor_id   = NodeId{RequiredIdFromJson(json, "successor_id", "AddColorGrade")};
      change.before_next_color_grade_name_number =
          RequirePositiveUint64(json, "before_next_color_grade_name_number", "AddColorGrade");
      change.after_next_color_grade_name_number =
          RequirePositiveUint64(json, "after_next_color_grade_name_number", "AddColorGrade");
      change.node          = CanonicalColorGradeNodeJson(json.at("node"), "AddColorGrade node");
      change.incoming_edge = EdgeFromJson(json.at("incoming_edge"), "AddColorGrade incoming_edge");
      change.outgoing_edge = EdgeFromJson(json.at("outgoing_edge"), "AddColorGrade outgoing_edge");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::RemoveColorGrade: {
      RequireExactObjectKeys(json,
                             {"bridge_edge", "kind", "node", "node_id", "predecessor_id",
                              "removed_incoming_edge", "removed_outgoing_edge", "successor_id"},
                             "RemoveColorGrade");
      RemoveColorGradeChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "RemoveColorGrade")};
      change.predecessor_id =
          NodeId{RequiredIdFromJson(json, "predecessor_id", "RemoveColorGrade")};
      change.successor_id = NodeId{RequiredIdFromJson(json, "successor_id", "RemoveColorGrade")};
      change.node         = CanonicalColorGradeNodeJson(json.at("node"), "RemoveColorGrade node");
      change.removed_incoming_edge =
          EdgeFromJson(json.at("removed_incoming_edge"), "RemoveColorGrade removed_incoming_edge");
      change.removed_outgoing_edge =
          EdgeFromJson(json.at("removed_outgoing_edge"), "RemoveColorGrade removed_outgoing_edge");
      change.bridge_edge = EdgeFromJson(json.at("bridge_edge"), "RemoveColorGrade bridge_edge");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::ReconnectColorGrade: {
      RequireExactObjectKeys(json,
                             {"after_incoming_edge", "after_outgoing_edge", "after_predecessor_id",
                              "after_successor_id", "before_incoming_edge", "before_outgoing_edge",
                              "before_predecessor_id", "before_successor_id", "kind", "node_id"},
                             "ReconnectColorGrade");
      ReconnectColorGradeChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "ReconnectColorGrade")};
      change.before_predecessor_id =
          NodeId{RequiredIdFromJson(json, "before_predecessor_id", "ReconnectColorGrade")};
      change.before_successor_id =
          NodeId{RequiredIdFromJson(json, "before_successor_id", "ReconnectColorGrade")};
      change.after_predecessor_id =
          NodeId{RequiredIdFromJson(json, "after_predecessor_id", "ReconnectColorGrade")};
      change.after_successor_id =
          NodeId{RequiredIdFromJson(json, "after_successor_id", "ReconnectColorGrade")};
      change.before_incoming_edge =
          EdgeFromJson(json.at("before_incoming_edge"), "ReconnectColorGrade before_incoming_edge");
      change.before_outgoing_edge =
          EdgeFromJson(json.at("before_outgoing_edge"), "ReconnectColorGrade before_outgoing_edge");
      change.after_incoming_edge =
          EdgeFromJson(json.at("after_incoming_edge"), "ReconnectColorGrade after_incoming_edge");
      change.after_outgoing_edge =
          EdgeFromJson(json.at("after_outgoing_edge"), "ReconnectColorGrade after_outgoing_edge");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::AddMask: {
      RequireExactObjectKeys(json, {"display_index", "kind", "mask", "mask_id", "node_id"},
                             "AddMask");
      if (!json.at("display_index").is_number_integer() ||
          json.at("display_index").get<std::int64_t>() < 0) {
        Fail("AddMask: display_index must be a non-negative integer");
      }
      AddMaskChange change;
      change.node_id       = NodeId{RequiredIdFromJson(json, "node_id", "AddMask")};
      change.mask_id       = MaskId{RequiredIdFromJson(json, "mask_id", "AddMask")};
      change.mask          = CanonicalMaskJson(json.at("mask"), "AddMask mask");
      change.display_index = json.at("display_index").get<std::uint32_t>();
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::RemoveMask: {
      RequireExactObjectKeys(json, {"display_index", "kind", "mask", "mask_id", "node_id"},
                             "RemoveMask");
      if (!json.at("display_index").is_number_integer() ||
          json.at("display_index").get<std::int64_t>() < 0) {
        Fail("RemoveMask: display_index must be a non-negative integer");
      }
      RemoveMaskChange change;
      change.node_id       = NodeId{RequiredIdFromJson(json, "node_id", "RemoveMask")};
      change.mask_id       = MaskId{RequiredIdFromJson(json, "mask_id", "RemoveMask")};
      change.mask          = CanonicalMaskJson(json.at("mask"), "RemoveMask mask");
      change.display_index = json.at("display_index").get<std::uint32_t>();
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::ReplaceMaskSource: {
      RequireExactObjectKeys(json, {"after_source", "before_source", "kind", "mask_id", "node_id"},
                             "ReplaceMaskSource");
      ReplaceMaskSourceChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "ReplaceMaskSource")};
      change.mask_id = MaskId{RequiredIdFromJson(json, "mask_id", "ReplaceMaskSource")};
      change.before_source =
          CanonicalMaskSourceJson(json.at("before_source"), "ReplaceMaskSource before_source");
      change.after_source =
          CanonicalMaskSourceJson(json.at("after_source"), "ReplaceMaskSource after_source");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::ReplaceMaskAsset: {
      RequireExactObjectKeys(json, {"after_source", "before_source", "kind", "mask_id", "node_id"},
                             "ReplaceMaskAsset");
      ReplaceMaskAssetChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "ReplaceMaskAsset")};
      change.mask_id = MaskId{RequiredIdFromJson(json, "mask_id", "ReplaceMaskAsset")};
      change.before_source =
          CanonicalMaskSourceJson(json.at("before_source"), "ReplaceMaskAsset before_source");
      change.after_source =
          CanonicalMaskSourceJson(json.at("after_source"), "ReplaceMaskAsset after_source");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::SetMaskField: {
      RequireExactObjectKeys(
          json, {"after_value", "before_value", "field_key", "kind", "mask_id", "node_id"},
          "SetMaskField");
      SetMaskFieldChange change;
      change.node_id      = NodeId{RequiredIdFromJson(json, "node_id", "SetMaskField")};
      change.mask_id      = MaskId{RequiredIdFromJson(json, "mask_id", "SetMaskField")};
      change.field_key    = RequireNonEmptyString(json, "field_key", "SetMaskField");
      change.before_value = json.at("before_value");
      change.after_value  = json.at("after_value");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::AppendBrushStroke: {
      RequireExactObjectKeys(json, {"kind", "mask_id", "node_id", "stroke"}, "AppendBrushStroke");
      AppendBrushStrokeChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "AppendBrushStroke")};
      change.mask_id = MaskId{RequiredIdFromJson(json, "mask_id", "AppendBrushStroke")};
      change.stroke  = StrokeFromCanonicalJson(json.at("stroke"), "AppendBrushStroke stroke");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::RemoveBrushStroke: {
      RequireExactObjectKeys(json, {"index", "kind", "mask_id", "node_id", "stroke", "stroke_id"},
                             "RemoveBrushStroke");
      RemoveBrushStrokeChange change;
      change.node_id   = NodeId{RequiredIdFromJson(json, "node_id", "RemoveBrushStroke")};
      change.mask_id   = MaskId{RequiredIdFromJson(json, "mask_id", "RemoveBrushStroke")};
      change.stroke_id = StrokeId{RequiredIdFromJson(json, "stroke_id", "RemoveBrushStroke")};
      change.index     = RequireNonNegativeUint32(json, "index", "RemoveBrushStroke");
      change.stroke    = StrokeFromCanonicalJson(json.at("stroke"), "RemoveBrushStroke stroke");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::InsertBrushStroke: {
      RequireExactObjectKeys(json, {"index", "kind", "mask_id", "node_id", "stroke"},
                             "InsertBrushStroke");
      InsertBrushStrokeChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "InsertBrushStroke")};
      change.mask_id = MaskId{RequiredIdFromJson(json, "mask_id", "InsertBrushStroke")};
      change.index   = RequireNonNegativeUint32(json, "index", "InsertBrushStroke");
      change.stroke  = StrokeFromCanonicalJson(json.at("stroke"), "InsertBrushStroke stroke");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::SetBrushTranslation: {
      RequireExactObjectKeys(json, {"after", "before", "kind", "mask_id", "node_id"},
                             "SetBrushTranslation");
      SetBrushTranslationChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "SetBrushTranslation")};
      change.mask_id = MaskId{RequiredIdFromJson(json, "mask_id", "SetBrushTranslation")};
      change.before  = TranslationVectorFromJson(json.at("before"), "SetBrushTranslation before");
      change.after   = TranslationVectorFromJson(json.at("after"), "SetBrushTranslation after");
      ValidatePipelineEditChange(change);
      return change;
    }
    case PipelineEditChangeKind::NodeGraphTopologyChange: {
      RequireExactObjectKeys(
          json,
          {"after_next_color_grade_name_number", "before_next_color_grade_name_number",
           "connected_edges", "disconnected_edges", "inserted_nodes", "kind", "removed_nodes"},
          "NodeGraphTopologyChange");
      if (!json.at("inserted_nodes").is_array() || !json.at("removed_nodes").is_array() ||
          !json.at("disconnected_edges").is_array() || !json.at("connected_edges").is_array()) {
        Fail("NodeGraphTopologyChange: node and edge lists must be arrays");
      }
      NodeGraphTopologyChange change;
      change.before_next_color_grade_name_number = RequirePositiveUint64(
          json, "before_next_color_grade_name_number", "NodeGraphTopologyChange");
      change.after_next_color_grade_name_number = RequirePositiveUint64(
          json, "after_next_color_grade_name_number", "NodeGraphTopologyChange");
      for (const auto& item : json.at("inserted_nodes")) {
        RequireExactObjectKeys(item, {"final_node_index", "node"},
                               "NodeGraphTopologyChange inserted node");
        NodeGraphInsertedNode inserted;
        inserted.final_node_index =
            RequireNonNegativeUint32(item, "final_node_index", "NodeGraphTopologyChange");
        inserted.node =
            CanonicalColorGradeNodeJson(item.at("node"), "NodeGraphTopologyChange inserted node");
        change.inserted_nodes.push_back(std::move(inserted));
      }
      for (const auto& item : json.at("removed_nodes")) {
        RequireExactObjectKeys(item, {"node", "original_node_index"},
                               "NodeGraphTopologyChange removed node");
        NodeGraphRemovedNode removed;
        removed.original_node_index =
            RequireNonNegativeUint32(item, "original_node_index", "NodeGraphTopologyChange");
        removed.node =
            CanonicalColorGradeNodeJson(item.at("node"), "NodeGraphTopologyChange removed node");
        change.removed_nodes.push_back(std::move(removed));
      }
      for (const auto& item : json.at("disconnected_edges")) {
        RequireExactObjectKeys(item, {"edge", "original_edge_index"},
                               "NodeGraphTopologyChange disconnected edge");
        NodeGraphDisconnectedEdge disconnected;
        disconnected.original_edge_index =
            RequireNonNegativeUint32(item, "original_edge_index", "NodeGraphTopologyChange");
        disconnected.edge =
            EdgeFromJson(item.at("edge"), "NodeGraphTopologyChange disconnected edge");
        change.disconnected_edges.push_back(std::move(disconnected));
      }
      for (const auto& item : json.at("connected_edges")) {
        RequireExactObjectKeys(item, {"edge", "final_edge_index"},
                               "NodeGraphTopologyChange connected edge");
        NodeGraphConnectedEdge connected;
        connected.final_edge_index =
            RequireNonNegativeUint32(item, "final_edge_index", "NodeGraphTopologyChange");
        connected.edge = EdgeFromJson(item.at("edge"), "NodeGraphTopologyChange connected edge");
        change.connected_edges.push_back(std::move(connected));
      }
      ValidatePipelineEditChange(change);
      return change;
    }
  }
  Fail("PipelineEditChange: unhandled kind");
}
}  // namespace alcedo
