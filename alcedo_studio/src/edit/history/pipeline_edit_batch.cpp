//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/pipeline_edit_batch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {
namespace {

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

void RequireObject(const nlohmann::json& json, std::string_view context) {
  if (!json.is_object()) {
    Fail(std::string{context} + ": expected object");
  }
}

void RequireExactObjectKeys(const nlohmann::json& json, std::initializer_list<const char*> keys,
                            std::string_view context) {
  RequireObject(json, context);
  if (json.size() != keys.size()) {
    Fail(std::string{context} + ": unexpected field count");
  }
  for (const char* key : keys) {
    if (!json.contains(key)) {
      Fail(std::string{context} + ": missing required field '" + key + "'");
    }
  }
}

void RequireCanonicalDump(const nlohmann::json& stored, const nlohmann::json& canonical,
                          std::string_view context) {
  if (stored.dump() != canonical.dump()) {
    Fail(std::string{context} + ": stored payload is not in canonical form");
  }
}

void RejectNonFiniteNumbers(const nlohmann::json& value, std::string_view context) {
  if (value.is_number()) {
    if (!std::isfinite(value.get<double>())) {
      Fail(std::string{context} + ": numbers must be finite");
    }
    return;
  }
  if (value.is_array()) {
    for (const auto& item : value) {
      RejectNonFiniteNumbers(item, context);
    }
    return;
  }
  if (value.is_object()) {
    for (const auto& item : value.items()) {
      RejectNonFiniteNumbers(item.value(), context);
    }
  }
}

auto RequireString(const nlohmann::json& json, const char* key, std::string_view context)
    -> std::string {
  if (!json.at(key).is_string()) {
    Fail(std::string{context} + ": '" + key + "' must be a string");
  }
  return json.at(key).get<std::string>();
}

auto RequireNonEmptyString(const nlohmann::json& json, const char* key, std::string_view context)
    -> std::string {
  auto value = RequireString(json, key, context);
  if (value.empty()) {
    Fail(std::string{context} + ": '" + key + "' must not be empty");
  }
  return value;
}

auto RequireBool(const nlohmann::json& json, const char* key, std::string_view context) -> bool {
  if (!json.at(key).is_boolean()) {
    Fail(std::string{context} + ": '" + key + "' must be a boolean");
  }
  return json.at(key).get<bool>();
}

auto RequireFiniteFloat(const nlohmann::json& json, const char* key, std::string_view context)
    -> float {
  if (!json.at(key).is_number()) {
    Fail(std::string{context} + ": '" + key + "' must be a number");
  }
  const double value = json.at(key).get<double>();
  if (!std::isfinite(value)) {
    Fail(std::string{context} + ": '" + key + "' must be finite");
  }
  return static_cast<float>(value);
}

auto RequireNormalizedMix(const nlohmann::json& json, const char* key, std::string_view context)
    -> float {
  const float value = RequireFiniteFloat(json, key, context);
  if (value < 0.0f || value > 1.0f) {
    Fail(std::string{context} + ": '" + key + "' must stay in [0, 1]");
  }
  return value;
}

auto OptionalIdFromJson(const nlohmann::json& value, std::string_view context, const char* key)
    -> std::string {
  if (value.is_null()) {
    return {};
  }
  if (!value.is_string()) {
    Fail(std::string{context} + ": '" + key + "' must be a string or null");
  }
  return value.get<std::string>();
}

auto RequiredIdFromJson(const nlohmann::json& json, const char* key, std::string_view context)
    -> std::string {
  return RequireNonEmptyString(json, key, context);
}

auto RequirePositiveUint64(const nlohmann::json& json, const char* key, std::string_view context)
    -> std::uint64_t {
  const auto& value = json.at(key);
  if (value.is_number_unsigned()) {
    const auto number = value.get<std::uint64_t>();
    if (number != 0) {
      return number;
    }
  } else if (value.is_number_integer()) {
    const auto number = value.get<std::int64_t>();
    if (number > 0) {
      return static_cast<std::uint64_t>(number);
    }
  }
  Fail(std::string{context} + ": '" + key + "' must be a positive uint64");
}

auto IdToJson(std::string_view value) -> nlohmann::json {
  if (value.empty()) {
    return nullptr;
  }
  return std::string{value};
}

auto OwnerKindText(PipelineParameterOwnerKind kind) -> std::string_view {
  switch (kind) {
    case PipelineParameterOwnerKind::Document:
      return "document";
    case PipelineParameterOwnerKind::Develop:
      return "develop";
    case PipelineParameterOwnerKind::ColorGrade:
      return "color_grade";
    case PipelineParameterOwnerKind::ColorGradeMask:
      return "color_grade_mask";
    case PipelineParameterOwnerKind::DrtPost:
      return "drt_post";
  }
  Fail("PipelineParameterOwnerKind: unknown enum value");
}

auto OwnerKindFromText(std::string_view text) -> PipelineParameterOwnerKind {
  if (text == "document") {
    return PipelineParameterOwnerKind::Document;
  }
  if (text == "develop") {
    return PipelineParameterOwnerKind::Develop;
  }
  if (text == "color_grade") {
    return PipelineParameterOwnerKind::ColorGrade;
  }
  if (text == "color_grade_mask") {
    return PipelineParameterOwnerKind::ColorGradeMask;
  }
  if (text == "drt_post") {
    return PipelineParameterOwnerKind::DrtPost;
  }
  Fail("PipelineParameterOwnerKind: unknown owner_kind '" + std::string{text} + "'");
}

auto NodeKindText(PipelineEditNodeKind kind) -> std::string_view {
  switch (kind) {
    case PipelineEditNodeKind::Develop:
      return "develop";
    case PipelineEditNodeKind::ColorGrade:
      return "color_grade";
    case PipelineEditNodeKind::Drt:
      return "drt";
  }
  Fail("PipelineEditNodeKind: unknown enum value");
}

auto NodeKindFromText(std::string_view text) -> PipelineEditNodeKind {
  if (text == "develop") {
    return PipelineEditNodeKind::Develop;
  }
  if (text == "color_grade") {
    return PipelineEditNodeKind::ColorGrade;
  }
  if (text == "drt") {
    return PipelineEditNodeKind::Drt;
  }
  Fail("PipelineEditNodeKind: unknown node_kind '" + std::string{text} + "'");
}

auto ChangeKindFromText(std::string_view text) -> PipelineEditChangeKind {
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
  Fail("PipelineEditChange: unknown kind '" + std::string{text} + "'");
}

auto EdgeToJson(const PipelineSceneEdge& edge) -> nlohmann::json {
  return nlohmann::json{{"from", nlohmann::json::array({std::string{edge.from_node.Value()},
                                                        std::string{edge.from_port.Value()}})},
                        {"to", nlohmann::json::array({std::string{edge.to_node.Value()},
                                                      std::string{edge.to_port.Value()}})}};
}

auto EdgeFromJson(const nlohmann::json& json, std::string_view context) -> PipelineSceneEdge {
  RequireExactObjectKeys(json, {"from", "to"}, context);
  if (!json.at("from").is_array() || json.at("from").size() != 2 || !json.at("from")[0].is_string() ||
      !json.at("from")[1].is_string()) {
    Fail(std::string{context} + ": 'from' must be [node_id, port_id]");
  }
  if (!json.at("to").is_array() || json.at("to").size() != 2 || !json.at("to")[0].is_string() ||
      !json.at("to")[1].is_string()) {
    Fail(std::string{context} + ": 'to' must be [node_id, port_id]");
  }
  PipelineSceneEdge edge;
  edge.from_node = NodeId{json.at("from")[0].get<std::string>()};
  edge.from_port = PortId{json.at("from")[1].get<std::string>()};
  edge.to_node   = NodeId{json.at("to")[0].get<std::string>()};
  edge.to_port   = PortId{json.at("to")[1].get<std::string>()};
  if (edge.from_node.Empty() || edge.from_port.Empty() || edge.to_node.Empty() ||
      edge.to_port.Empty()) {
    Fail(std::string{context} + ": edge endpoints must not be empty");
  }
  return edge;
}

void RequireEdgeEndpoints(const PipelineSceneEdge& edge, const NodeId& from, const NodeId& to,
                          std::string_view context) {
  if (edge.from_node != from || edge.to_node != to) {
    Fail(std::string{context} + ": edge endpoints do not match stored neighbors");
  }
}

auto CanonicalColorGradeNodeJson(const nlohmann::json& node, std::string_view context)
    -> nlohmann::json {
  RequireObject(node, context);
  RejectNonFiniteNumbers(node, context);
  try {
    const auto parsed    = ColorGradeNodeModel::FromJson(node);
    const auto canonical = parsed->ToJson();
    RequireCanonicalDump(node, canonical, context);
    return canonical;
  } catch (const std::exception& ex) {
    Fail(std::string{context} + ": " + ex.what());
  }
}

auto CanonicalMaskJson(const nlohmann::json& mask, std::string_view context) -> nlohmann::json {
  RequireObject(mask, context);
  RejectNonFiniteNumbers(mask, context);
  try {
    const auto canonical = MaskModelToJson(MaskModelFromJson(mask));
    RequireCanonicalDump(mask, canonical, context);
    return canonical;
  } catch (const std::exception& ex) {
    Fail(std::string{context} + ": " + ex.what());
  }
}

auto CanonicalMaskSourceJson(const nlohmann::json& source, std::string_view context)
    -> nlohmann::json {
  RequireObject(source, context);
  RejectNonFiniteNumbers(source, context);
  nlohmann::json probe{{"id", "source.probe"},
                       {"display_name", ""},
                       {"enabled", true},
                       {"opacity", 1.0},
                       {"invert", false},
                       {"source", source},
                       {"color_range", nullptr},
                       {"luminance_range", nullptr}};
  try {
    const auto parsed           = MaskModelFromJson(probe);
    const auto canonical_source = MaskModelToJson(parsed).at("source");
    RequireCanonicalDump(source, canonical_source, context);
    return canonical_source;
  } catch (const std::exception& ex) {
    Fail(std::string{context} + ": " + ex.what());
  }
}

auto AssetKeyFromBrushSource(const nlohmann::json& source, std::string_view context) -> std::string {
  if (!source.contains("kind") || !source.at("kind").is_string() ||
      source.at("kind").get<std::string>() != "brush") {
    Fail(std::string{context} + ": ReplaceMaskAsset requires a brush source");
  }
  if (!source.contains("asset_key") || source.at("asset_key").is_null()) {
    Fail(std::string{context} + ": ReplaceMaskAsset requires a non-null asset_key");
  }
  if (!source.at("asset_key").is_string()) {
    Fail(std::string{context} + ": asset_key must be a string");
  }
  const auto key = source.at("asset_key").get<std::string>();
  if (key.size() != 32) {
    Fail(std::string{context} + ": asset_key must be 32 lowercase hex digits");
  }
  for (char ch : key) {
    const bool hex = (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
    if (!hex) {
      Fail(std::string{context} + ": asset_key must be 32 lowercase hex digits");
    }
  }
  return key;
}

void ValidateParameterTarget(const PipelineParameterTarget& target) {
  if (target.field_key.empty()) {
    Fail("SetParameter: field_key must not be empty");
  }
  switch (target.owner_kind) {
    case PipelineParameterOwnerKind::Document:
      if (!target.node_id.Empty() || !target.adjustment_instance_id.Empty() ||
          !target.mask_id.Empty()) {
        Fail("SetParameter: document target must leave node_id, adjustment_instance_id, and "
             "mask_id empty");
      }
      return;
    case PipelineParameterOwnerKind::Develop:
      if (target.node_id.Empty()) {
        Fail("SetParameter: develop target requires node_id");
      }
      if (!target.adjustment_instance_id.Empty() || !target.mask_id.Empty()) {
        Fail("SetParameter: develop target must leave adjustment_instance_id and mask_id empty");
      }
      return;
    case PipelineParameterOwnerKind::ColorGrade:
      if (target.node_id.Empty() || target.adjustment_instance_id.Empty()) {
        Fail("SetParameter: color_grade target requires node_id and adjustment_instance_id");
      }
      if (!target.mask_id.Empty()) {
        Fail("SetParameter: color_grade target must leave mask_id empty");
      }
      return;
    case PipelineParameterOwnerKind::ColorGradeMask:
      if (target.node_id.Empty() || target.mask_id.Empty()) {
        Fail("SetParameter: color_grade_mask target requires node_id and mask_id");
      }
      if (!target.adjustment_instance_id.Empty()) {
        Fail("SetParameter: color_grade_mask target must leave adjustment_instance_id empty");
      }
      return;
    case PipelineParameterOwnerKind::DrtPost:
      if (target.node_id.Empty()) {
        Fail("SetParameter: drt_post target requires node_id");
      }
      if (!target.mask_id.Empty()) {
        Fail("SetParameter: drt_post target must leave mask_id empty");
      }
      if ((target.field_key == "clarity" || target.field_key == "sharpen" ||
           target.field_key == "halation" || target.field_key == "film_grain") &&
          target.adjustment_instance_id.Empty()) {
        Fail("SetParameter: drt_post target requires adjustment_instance_id");
      }
      return;
  }
  Fail("SetParameter: unknown owner_kind");
}

auto TargetToJson(const PipelineParameterTarget& target) -> nlohmann::json {
  return nlohmann::json{{"adjustment_instance_id", IdToJson(target.adjustment_instance_id.Value())},
                        {"field_key", target.field_key},
                        {"mask_id", IdToJson(target.mask_id.Value())},
                        {"node_id", IdToJson(target.node_id.Value())},
                        {"owner_kind", std::string{OwnerKindText(target.owner_kind)}}};
}

auto TargetFromJson(const nlohmann::json& json) -> PipelineParameterTarget {
  RequireExactObjectKeys(
      json, {"adjustment_instance_id", "field_key", "mask_id", "node_id", "owner_kind"},
      "SetParameter target");
  PipelineParameterTarget target;
  target.owner_kind = OwnerKindFromText(RequireString(json, "owner_kind", "SetParameter target"));
  target.field_key  = RequireNonEmptyString(json, "field_key", "SetParameter target");
  target.node_id    = NodeId{OptionalIdFromJson(json.at("node_id"), "SetParameter target", "node_id")};
  target.adjustment_instance_id = AdjustmentInstanceId{
      OptionalIdFromJson(json.at("adjustment_instance_id"), "SetParameter target",
                         "adjustment_instance_id")};
  target.mask_id =
      MaskId{OptionalIdFromJson(json.at("mask_id"), "SetParameter target", "mask_id")};
  ValidateParameterTarget(target);
  return target;
}

void RequireModelObject(const nlohmann::json& value, std::string_view context) {
  RequireObject(value, context);
  RejectNonFiniteNumbers(value, context);
}

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
  const auto before = CanonicalMaskSourceJson(change.before_source, "ReplaceMaskAsset before_source");
  const auto after  = CanonicalMaskSourceJson(change.after_source, "ReplaceMaskAsset after_source");
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
        } else {
          ValidateSetMaskField(typed);
        }
      },
      change);
}

auto ChangeCompatible(PipelineEditOperationKind operation, PipelineEditChangeKind change) -> bool {
  if (operation == PipelineEditOperationKind::Paste) {
    return true;
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
    case PipelineEditOperationKind::Paste:
      return true;
  }
  return false;
}

auto ChangeToJson(const PipelineEditChange& change) -> nlohmann::json {
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
          return {{"after_next_color_grade_name_number", typed.after_next_color_grade_name_number},
                  {"before_next_color_grade_name_number",
                   typed.before_next_color_grade_name_number},
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
        } else {
          return {{"after_value", typed.after_value},
                  {"before_value", typed.before_value},
                  {"field_key", typed.field_key},
                  {"kind", "set_mask_field"},
                  {"mask_id", std::string{typed.mask_id.Value()}},
                  {"node_id", std::string{typed.node_id.Value()}}};
        }
      },
      change);
}

auto ChangeFromJson(const nlohmann::json& json) -> PipelineEditChange {
  RequireObject(json, "PipelineEditChange");
  if (!json.contains("kind") || !json.at("kind").is_string()) {
    Fail("PipelineEditChange: kind must be a string");
  }
  const auto kind = ChangeKindFromText(json.at("kind").get<std::string>());
  switch (kind) {
    case PipelineEditChangeKind::SetParameter: {
      RequireExactObjectKeys(json,
                             {"after_enabled", "after_value", "before_enabled", "before_value",
                              "kind", "target"},
                             "SetParameter");
      SetParameterChange change;
      change.target         = TargetFromJson(json.at("target"));
      change.before_value   = json.at("before_value");
      change.after_value    = json.at("after_value");
      change.before_enabled = RequireBool(json, "before_enabled", "SetParameter");
      change.after_enabled  = RequireBool(json, "after_enabled", "SetParameter");
      ValidateSetParameter(change);
      return change;
    }
    case PipelineEditChangeKind::SetNodeEnabled: {
      RequireExactObjectKeys(json, {"after_enabled", "before_enabled", "kind", "node_id", "node_kind"},
                             "SetNodeEnabled");
      SetNodeEnabledChange change;
      change.node_id        = NodeId{RequiredIdFromJson(json, "node_id", "SetNodeEnabled")};
      change.node_kind      = NodeKindFromText(RequireString(json, "node_kind", "SetNodeEnabled"));
      change.before_enabled = RequireBool(json, "before_enabled", "SetNodeEnabled");
      change.after_enabled  = RequireBool(json, "after_enabled", "SetNodeEnabled");
      ValidateSetNodeEnabled(change);
      return change;
    }
    case PipelineEditChangeKind::SetNodeMix: {
      RequireExactObjectKeys(json, {"after_mix", "before_mix", "kind", "node_id"}, "SetNodeMix");
      SetNodeMixChange change;
      change.node_id    = NodeId{RequiredIdFromJson(json, "node_id", "SetNodeMix")};
      change.before_mix = RequireNormalizedMix(json, "before_mix", "SetNodeMix");
      change.after_mix  = RequireNormalizedMix(json, "after_mix", "SetNodeMix");
      ValidateSetNodeMix(change);
      return change;
    }
    case PipelineEditChangeKind::RenameColorGrade: {
      RequireExactObjectKeys(
          json, {"after_display_name", "before_display_name", "kind", "node_id"},
          "RenameColorGrade");
      RenameColorGradeChange change;
      change.node_id             = NodeId{RequiredIdFromJson(json, "node_id", "RenameColorGrade")};
      change.before_display_name = RequireNonEmptyString(json, "before_display_name",
                                                         "RenameColorGrade");
      change.after_display_name =
          RequireNonEmptyString(json, "after_display_name", "RenameColorGrade");
      ValidateRename(change);
      return change;
    }
    case PipelineEditChangeKind::AddColorGrade: {
      RequireExactObjectKeys(json,
                             {"after_next_color_grade_name_number",
                              "before_next_color_grade_name_number", "incoming_edge", "kind",
                              "node", "node_id", "outgoing_edge", "predecessor_id", "successor_id"},
                             "AddColorGrade");
      AddColorGradeChange change;
      change.node_id         = NodeId{RequiredIdFromJson(json, "node_id", "AddColorGrade")};
      change.predecessor_id  = NodeId{RequiredIdFromJson(json, "predecessor_id", "AddColorGrade")};
      change.successor_id    = NodeId{RequiredIdFromJson(json, "successor_id", "AddColorGrade")};
      change.before_next_color_grade_name_number =
          RequirePositiveUint64(json, "before_next_color_grade_name_number", "AddColorGrade");
      change.after_next_color_grade_name_number =
          RequirePositiveUint64(json, "after_next_color_grade_name_number", "AddColorGrade");
      change.node            = CanonicalColorGradeNodeJson(json.at("node"), "AddColorGrade node");
      change.incoming_edge   = EdgeFromJson(json.at("incoming_edge"), "AddColorGrade incoming_edge");
      change.outgoing_edge   = EdgeFromJson(json.at("outgoing_edge"), "AddColorGrade outgoing_edge");
      ValidateAddColorGrade(change);
      return change;
    }
    case PipelineEditChangeKind::RemoveColorGrade: {
      RequireExactObjectKeys(json,
                             {"bridge_edge", "kind", "node", "node_id", "predecessor_id",
                              "removed_incoming_edge", "removed_outgoing_edge", "successor_id"},
                             "RemoveColorGrade");
      RemoveColorGradeChange change;
      change.node_id        = NodeId{RequiredIdFromJson(json, "node_id", "RemoveColorGrade")};
      change.predecessor_id = NodeId{RequiredIdFromJson(json, "predecessor_id", "RemoveColorGrade")};
      change.successor_id   = NodeId{RequiredIdFromJson(json, "successor_id", "RemoveColorGrade")};
      change.node = CanonicalColorGradeNodeJson(json.at("node"), "RemoveColorGrade node");
      change.removed_incoming_edge =
          EdgeFromJson(json.at("removed_incoming_edge"), "RemoveColorGrade removed_incoming_edge");
      change.removed_outgoing_edge =
          EdgeFromJson(json.at("removed_outgoing_edge"), "RemoveColorGrade removed_outgoing_edge");
      change.bridge_edge = EdgeFromJson(json.at("bridge_edge"), "RemoveColorGrade bridge_edge");
      ValidateRemoveColorGrade(change);
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
      ValidateReconnect(change);
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
      ValidateAddOrRemoveMask(change.node_id, change.mask_id, change.mask, "AddMask");
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
      ValidateAddOrRemoveMask(change.node_id, change.mask_id, change.mask, "RemoveMask");
      return change;
    }
    case PipelineEditChangeKind::ReplaceMaskSource: {
      RequireExactObjectKeys(
          json, {"after_source", "before_source", "kind", "mask_id", "node_id"},
          "ReplaceMaskSource");
      ReplaceMaskSourceChange change;
      change.node_id       = NodeId{RequiredIdFromJson(json, "node_id", "ReplaceMaskSource")};
      change.mask_id       = MaskId{RequiredIdFromJson(json, "mask_id", "ReplaceMaskSource")};
      change.before_source =
          CanonicalMaskSourceJson(json.at("before_source"), "ReplaceMaskSource before_source");
      change.after_source =
          CanonicalMaskSourceJson(json.at("after_source"), "ReplaceMaskSource after_source");
      ValidateReplaceMaskSource(change);
      return change;
    }
    case PipelineEditChangeKind::ReplaceMaskAsset: {
      RequireExactObjectKeys(
          json, {"after_source", "before_source", "kind", "mask_id", "node_id"},
          "ReplaceMaskAsset");
      ReplaceMaskAssetChange change;
      change.node_id = NodeId{RequiredIdFromJson(json, "node_id", "ReplaceMaskAsset")};
      change.mask_id = MaskId{RequiredIdFromJson(json, "mask_id", "ReplaceMaskAsset")};
      change.before_source =
          CanonicalMaskSourceJson(json.at("before_source"), "ReplaceMaskAsset before_source");
      change.after_source =
          CanonicalMaskSourceJson(json.at("after_source"), "ReplaceMaskAsset after_source");
      ValidateReplaceMaskAsset(change);
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
      ValidateSetMaskField(change);
      return change;
    }
  }
  Fail("PipelineEditChange: unhandled kind");
}

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
    case PipelineEditOperationKind::Paste:
      return "paste";
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
  if (text == "paste") {
    return PipelineEditOperationKind::Paste;
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
        } else {
          return PipelineEditChangeKind::SetMaskField;
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
    ValidateChange(change);
    if (!ChangeCompatible(operation_kind, PipelineEditChangeKindOf(change))) {
      Fail("PipelineEditBatch: change kind is incompatible with operation_kind");
    }
  }
}

auto PipelineEditBatch::Make(PipelineEditOperationKind operation_kind,
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
    change_array.push_back(ChangeToJson(change));
  }
  return nlohmann::json{{"batch_format_version", batch_format_version},
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
  batch.presentation_key =
      RequireNonEmptyString(json, "presentation_key", "PipelineEditBatch");
  batch.presentation_args = json.at("presentation_args");
  batch.changes.reserve(json.at("changes").size());
  for (const auto& change_json : json.at("changes")) {
    batch.changes.push_back(ChangeFromJson(change_json));
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
  row.operation_kind     = batch.operation_kind;
  row.presentation_key   = batch.presentation_key;
  row.presentation_args  = batch.presentation_args;
  row.node_display_name  = StringArg(batch.presentation_args, "node_display_name");
  row.mask_display_name  = StringArg(batch.presentation_args, "mask_display_name");
  const auto& change     = batch.changes.front();
  std::visit(
      [&row](const auto& typed) {
        using Typed = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Typed, SetParameterChange>) {
          row.node_id = std::string{typed.target.node_id.Value()};
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
        } else {
          row.node_id              = std::string{typed.node_id.Value()};
          row.mask_id              = std::string{typed.mask_id.Value()};
          row.field_key            = typed.field_key;
          row.before_display_value = typed.before_value;
          row.after_display_value  = typed.after_value;
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
