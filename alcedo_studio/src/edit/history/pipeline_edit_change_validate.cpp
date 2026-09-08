//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <cmath>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>

#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_edit_change.hpp"
#include "edit/history/pipeline_edit_json.hpp"

namespace alcedo {
namespace {

using pipeline_edit_json::AssetKeyFromBrushSource;
using pipeline_edit_json::CanonicalBrushStrokeJson;
using pipeline_edit_json::CanonicalColorGradeNodeJson;
using pipeline_edit_json::CanonicalMaskJson;
using pipeline_edit_json::CanonicalMaskSourceJson;
using pipeline_edit_json::Fail;
using pipeline_edit_json::RequireEdgeEndpoints;
using pipeline_edit_json::RequireModelObject;
using pipeline_edit_json::ValidateParameterTarget;

void ValidateSetParameter(const SetParameterChange& change) {
  ValidateParameterTarget(change.target);
  RequireModelObject(change.before_value, "SetParameter before_value");
  RequireModelObject(change.after_value, "SetParameter after_value");
}

void ValidateSetNodeEnabled(const SetNodeEnabledChange& change) {
  if (change.node_id.Empty()) {
    Fail("SetNodeEnabled: node_id must not be empty");
  }
}

void ValidateSetNodeMix(const SetNodeMixChange& change) {
  if (change.node_id.Empty()) {
    Fail("SetNodeMix: node_id must not be empty");
  }
  if (!std::isfinite(change.before_mix) || change.before_mix < 0.0f || change.before_mix > 1.0f) {
    Fail("SetNodeMix: before_mix must be a finite value in [0, 1]");
  }
  if (!std::isfinite(change.after_mix) || change.after_mix < 0.0f || change.after_mix > 1.0f) {
    Fail("SetNodeMix: after_mix must be a finite value in [0, 1]");
  }
}

void ValidateRename(const RenameColorGradeChange& change) {
  if (change.node_id.Empty()) {
    Fail("RenameColorGrade: node_id must not be empty");
  }
  if (change.before_display_name.empty() || change.after_display_name.empty()) {
    Fail("RenameColorGrade: display names must not be empty");
  }
}

void ValidateAddColorGrade(const AddColorGradeChange& change) {
  if (change.node_id.Empty() || change.predecessor_id.Empty() || change.successor_id.Empty()) {
    Fail("AddColorGrade: node_id, predecessor_id, and successor_id are required");
  }
  if (change.node_id == change.predecessor_id || change.node_id == change.successor_id ||
      change.predecessor_id == change.successor_id) {
    Fail("AddColorGrade: node, predecessor, and successor must be distinct");
  }
  const auto node = CanonicalColorGradeNodeJson(change.node, "AddColorGrade node");
  if (node.at("id").get<std::string>() != std::string{change.node_id.Value()}) {
    Fail("AddColorGrade: node.id must match node_id");
  }
  if (change.before_next_color_grade_name_number == 0 ||
      change.after_next_color_grade_name_number == 0) {
    Fail("AddColorGrade: name-counter values must be positive");
  }
  const bool counter_unchanged =
      change.before_next_color_grade_name_number == change.after_next_color_grade_name_number;
  const bool counter_advanced =
      change.before_next_color_grade_name_number != std::numeric_limits<std::uint64_t>::max() &&
      change.after_next_color_grade_name_number == change.before_next_color_grade_name_number + 1;
  if (!counter_unchanged && !counter_advanced) {
    Fail("AddColorGrade: name-counter state must be unchanged or advance by one");
  }
  if (counter_advanced &&
      node.at("display_name").get<std::string>() !=
          DefaultColorGradeDisplayName(change.before_next_color_grade_name_number)) {
    Fail("AddColorGrade: generated display name does not match the stored counter");
  }
  RequireEdgeEndpoints(change.incoming_edge, change.predecessor_id, change.node_id,
                       "AddColorGrade incoming_edge");
  RequireEdgeEndpoints(change.outgoing_edge, change.node_id, change.successor_id,
                       "AddColorGrade outgoing_edge");
}

void ValidateRemoveColorGrade(const RemoveColorGradeChange& change) {
  if (change.node_id.Empty() || change.predecessor_id.Empty() || change.successor_id.Empty()) {
    Fail("RemoveColorGrade: node_id, predecessor_id, and successor_id are required");
  }
  const auto node = CanonicalColorGradeNodeJson(change.node, "RemoveColorGrade node");
  if (node.at("id").get<std::string>() != std::string{change.node_id.Value()}) {
    Fail("RemoveColorGrade: node.id must match node_id");
  }
  RequireEdgeEndpoints(change.removed_incoming_edge, change.predecessor_id, change.node_id,
                       "RemoveColorGrade removed_incoming_edge");
  RequireEdgeEndpoints(change.removed_outgoing_edge, change.node_id, change.successor_id,
                       "RemoveColorGrade removed_outgoing_edge");
  RequireEdgeEndpoints(change.bridge_edge, change.predecessor_id, change.successor_id,
                       "RemoveColorGrade bridge_edge");
}

void ValidateReconnect(const ReconnectColorGradeChange& change) {
  if (change.node_id.Empty() || change.before_predecessor_id.Empty() ||
      change.before_successor_id.Empty() || change.after_predecessor_id.Empty() ||
      change.after_successor_id.Empty()) {
    Fail("ReconnectColorGrade: node and neighbor IDs are required");
  }
  RequireEdgeEndpoints(change.before_incoming_edge, change.before_predecessor_id, change.node_id,
                       "ReconnectColorGrade before_incoming_edge");
  RequireEdgeEndpoints(change.before_outgoing_edge, change.node_id, change.before_successor_id,
                       "ReconnectColorGrade before_outgoing_edge");
  RequireEdgeEndpoints(change.after_incoming_edge, change.after_predecessor_id, change.node_id,
                       "ReconnectColorGrade after_incoming_edge");
  RequireEdgeEndpoints(change.after_outgoing_edge, change.node_id, change.after_successor_id,
                       "ReconnectColorGrade after_outgoing_edge");
}
auto EdgeIdentityKey(const PipelineSceneEdge& edge) -> std::string {
  return std::string{edge.from_node.Value()} + "/" + std::string{edge.from_port.Value()} + "->" +
         std::string{edge.to_node.Value()} + "/" + std::string{edge.to_port.Value()};
}

void ValidateNodeGraphTopology(const NodeGraphTopologyChange& change) {
  if (change.before_next_color_grade_name_number == 0 ||
      change.after_next_color_grade_name_number == 0) {
    Fail("NodeGraphTopologyChange: name-counter values must be positive");
  }
  if (change.after_next_color_grade_name_number < change.before_next_color_grade_name_number) {
    Fail("NodeGraphTopologyChange: after name-counter cannot be smaller than before");
  }
  if (change.inserted_nodes.empty() && change.removed_nodes.empty() &&
      change.disconnected_edges.empty() && change.connected_edges.empty() &&
      change.before_next_color_grade_name_number == change.after_next_color_grade_name_number) {
    Fail("NodeGraphTopologyChange: net delta must not be empty");
  }

  std::set<std::string>   inserted_ids;
  std::set<std::uint32_t> inserted_indexes;
  for (const auto& item : change.inserted_nodes) {
    const auto node =
        CanonicalColorGradeNodeJson(item.node, "NodeGraphTopologyChange inserted node");
    const auto id = node.at("id").get<std::string>();
    if (!inserted_ids.insert(id).second) {
      Fail("NodeGraphTopologyChange: duplicate inserted NodeId");
    }
    if (!inserted_indexes.insert(item.final_node_index).second) {
      Fail("NodeGraphTopologyChange: duplicate inserted node index");
    }
  }
  std::set<std::string>   removed_ids;
  std::set<std::uint32_t> removed_indexes;
  for (const auto& item : change.removed_nodes) {
    const auto node =
        CanonicalColorGradeNodeJson(item.node, "NodeGraphTopologyChange removed node");
    const auto id = node.at("id").get<std::string>();
    if (!removed_ids.insert(id).second) {
      Fail("NodeGraphTopologyChange: duplicate removed NodeId");
    }
    if (inserted_ids.contains(id)) {
      Fail("NodeGraphTopologyChange: NodeId is both inserted and removed");
    }
    if (!removed_indexes.insert(item.original_node_index).second) {
      Fail("NodeGraphTopologyChange: duplicate removed node index");
    }
  }
  std::set<std::string>   disconnected_keys;
  std::set<std::uint32_t> disconnected_indexes;
  for (const auto& item : change.disconnected_edges) {
    if (item.edge.from_node.Empty() || item.edge.to_node.Empty()) {
      Fail("NodeGraphTopologyChange: disconnected edge endpoints are required");
    }
    const auto key = EdgeIdentityKey(item.edge);
    if (!disconnected_keys.insert(key).second) {
      Fail("NodeGraphTopologyChange: duplicate disconnected edge");
    }
    if (!disconnected_indexes.insert(item.original_edge_index).second) {
      Fail("NodeGraphTopologyChange: duplicate disconnected edge index");
    }
  }
  std::set<std::string>   connected_keys;
  std::set<std::uint32_t> connected_indexes;
  for (const auto& item : change.connected_edges) {
    if (item.edge.from_node.Empty() || item.edge.to_node.Empty()) {
      Fail("NodeGraphTopologyChange: connected edge endpoints are required");
    }
    const auto key = EdgeIdentityKey(item.edge);
    if (!connected_keys.insert(key).second) {
      Fail("NodeGraphTopologyChange: duplicate connected edge");
    }
    if (!connected_indexes.insert(item.final_edge_index).second) {
      Fail("NodeGraphTopologyChange: duplicate connected edge index");
    }
  }
}

void ValidateMaskOwner(const NodeId& node_id, const MaskId& mask_id, std::string_view context) {
  if (node_id.Empty() || mask_id.Empty()) {
    Fail(std::string{context} + ": node_id and mask_id are required");
  }
}

void ValidateAddOrRemoveMask(const NodeId& node_id, const MaskId& mask_id,
                             const nlohmann::json& mask, std::string_view context) {
  ValidateMaskOwner(node_id, mask_id, context);
  const auto canonical = CanonicalMaskJson(mask, context);
  if (canonical.at("id").get<std::string>() != std::string{mask_id.Value()}) {
    Fail(std::string{context} + ": mask.id must match mask_id");
  }
}

void ValidateReplaceMaskSource(const ReplaceMaskSourceChange& change) {
  ValidateMaskOwner(change.node_id, change.mask_id, "ReplaceMaskSource");
  (void)CanonicalMaskSourceJson(change.before_source, "ReplaceMaskSource before_source");
  (void)CanonicalMaskSourceJson(change.after_source, "ReplaceMaskSource after_source");
}

void ValidateReplaceMaskAsset(const ReplaceMaskAssetChange& change) {
  ValidateMaskOwner(change.node_id, change.mask_id, "ReplaceMaskAsset");
  const auto before =
      CanonicalMaskSourceJson(change.before_source, "ReplaceMaskAsset before_source");
  const auto after = CanonicalMaskSourceJson(change.after_source, "ReplaceMaskAsset after_source");
  (void)AssetKeyFromBrushSource(before, "ReplaceMaskAsset before_source");
  (void)AssetKeyFromBrushSource(after, "ReplaceMaskAsset after_source");
}

void ValidateMaskFieldValue(const nlohmann::json& value, const std::string& field_key,
                            std::string_view context) {
  if (field_key == "enabled" || field_key == "invert") {
    if (!value.is_boolean()) {
      Fail(std::string{context} + ": '" + field_key + "' must be a boolean");
    }
    return;
  }
  if (field_key == "opacity") {
    if (!value.is_number() || !std::isfinite(value.get<double>())) {
      Fail(std::string{context} + ": opacity must be a finite number");
    }
    const double opacity = value.get<double>();
    if (opacity < 0.0 || opacity > 1.0) {
      Fail(std::string{context} + ": opacity must stay in [0, 1]");
    }
    return;
  }
  if (field_key == "display_name") {
    if (!value.is_string()) {
      Fail(std::string{context} + ": display_name must be a string");
    }
    return;
  }
  Fail(std::string{context} + ": unsupported field_key '" + field_key + "'");
}
void ValidateSetMaskField(const SetMaskFieldChange& change) {
  ValidateMaskOwner(change.node_id, change.mask_id, "SetMaskField");
  if (change.field_key.empty()) {
    Fail("SetMaskField: field_key must not be empty");
  }
  ValidateMaskFieldValue(change.before_value, change.field_key, "SetMaskField before_value");
  ValidateMaskFieldValue(change.after_value, change.field_key, "SetMaskField after_value");
}

void ValidateAppendBrushStroke(const AppendBrushStrokeChange& change) {
  ValidateMaskOwner(change.node_id, change.mask_id, "AppendBrushStroke");
  (void)CanonicalBrushStrokeJson(change.stroke, "AppendBrushStroke stroke");
}

void ValidateRemoveBrushStroke(const RemoveBrushStrokeChange& change) {
  ValidateMaskOwner(change.node_id, change.mask_id, "RemoveBrushStroke");
  if (change.stroke_id.Empty()) {
    Fail("RemoveBrushStroke: stroke_id must not be empty");
  }
  (void)CanonicalBrushStrokeJson(change.stroke, "RemoveBrushStroke stroke");
  if (change.stroke.id != change.stroke_id) {
    Fail("RemoveBrushStroke: stroke.id must match stroke_id");
  }
}

void ValidateInsertBrushStroke(const InsertBrushStrokeChange& change) {
  ValidateMaskOwner(change.node_id, change.mask_id, "InsertBrushStroke");
  (void)CanonicalBrushStrokeJson(change.stroke, "InsertBrushStroke stroke");
}

void ValidateSetBrushTranslation(const SetBrushTranslationChange& change) {
  ValidateMaskOwner(change.node_id, change.mask_id, "SetBrushTranslation");
  if (!std::isfinite(change.before.x) || !std::isfinite(change.before.y) ||
      !std::isfinite(change.after.x) || !std::isfinite(change.after.y)) {
    Fail("SetBrushTranslation: before and after must be finite");
  }
  if (change.before == change.after) {
    Fail("SetBrushTranslation: before and after must differ");
  }
}

void ValidateChange(const PipelineEditChange& change) {
  std::visit(
      [](const auto& typed) {
        using Typed = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Typed, SetParameterChange>) {
          ValidateSetParameter(typed);
        } else if constexpr (std::is_same_v<Typed, SetNodeEnabledChange>) {
          ValidateSetNodeEnabled(typed);
        } else if constexpr (std::is_same_v<Typed, SetNodeMixChange>) {
          ValidateSetNodeMix(typed);
        } else if constexpr (std::is_same_v<Typed, RenameColorGradeChange>) {
          ValidateRename(typed);
        } else if constexpr (std::is_same_v<Typed, AddColorGradeChange>) {
          ValidateAddColorGrade(typed);
        } else if constexpr (std::is_same_v<Typed, RemoveColorGradeChange>) {
          ValidateRemoveColorGrade(typed);
        } else if constexpr (std::is_same_v<Typed, ReconnectColorGradeChange>) {
          ValidateReconnect(typed);
        } else if constexpr (std::is_same_v<Typed, AddMaskChange>) {
          ValidateAddOrRemoveMask(typed.node_id, typed.mask_id, typed.mask, "AddMask");
        } else if constexpr (std::is_same_v<Typed, RemoveMaskChange>) {
          ValidateAddOrRemoveMask(typed.node_id, typed.mask_id, typed.mask, "RemoveMask");
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskSourceChange>) {
          ValidateReplaceMaskSource(typed);
        } else if constexpr (std::is_same_v<Typed, ReplaceMaskAssetChange>) {
          ValidateReplaceMaskAsset(typed);
        } else if constexpr (std::is_same_v<Typed, SetMaskFieldChange>) {
          ValidateSetMaskField(typed);
        } else if constexpr (std::is_same_v<Typed, AppendBrushStrokeChange>) {
          ValidateAppendBrushStroke(typed);
        } else if constexpr (std::is_same_v<Typed, RemoveBrushStrokeChange>) {
          ValidateRemoveBrushStroke(typed);
        } else if constexpr (std::is_same_v<Typed, InsertBrushStrokeChange>) {
          ValidateInsertBrushStroke(typed);
        } else if constexpr (std::is_same_v<Typed, SetBrushTranslationChange>) {
          ValidateSetBrushTranslation(typed);
        } else if constexpr (std::is_same_v<Typed, NodeGraphTopologyChange>) {
          ValidateNodeGraphTopology(typed);
        } else {
          Fail("PipelineEditChange: unhandled typed change");
        }
      },
      change);
}

}  // namespace

void ValidatePipelineEditChange(const PipelineEditChange& change) { ValidateChange(change); }

}  // namespace alcedo
