//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/history/pipeline_edit_json.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/mask/brush_stroke.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo::pipeline_edit_json {

[[noreturn]] void Fail(const std::string& message) { throw std::runtime_error(message); }

void              RequireObject(const nlohmann::json& json, std::string_view context) {
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

auto EdgeToJson(const PipelineSceneEdge& edge) -> nlohmann::json {
  return nlohmann::json{{"from", nlohmann::json::array({std::string{edge.from_node.Value()},
                                                        std::string{edge.from_port.Value()}})},
                        {"to", nlohmann::json::array({std::string{edge.to_node.Value()},
                                                      std::string{edge.to_port.Value()}})}};
}

auto EdgeFromJson(const nlohmann::json& json, std::string_view context) -> PipelineSceneEdge {
  RequireExactObjectKeys(json, {"from", "to"}, context);
  if (!json.at("from").is_array() || json.at("from").size() != 2 ||
      !json.at("from")[0].is_string() || !json.at("from")[1].is_string()) {
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
  nlohmann::json probe{{"id", "source.probe"},   {"display_name", ""},        {"enabled", true},
                       {"opacity", 1.0},         {"invert", false},           {"source", source},
                       {"color_range", nullptr}, {"luminance_range", nullptr}};
  try {
    const auto parsed           = MaskModelFromJson(probe);
    const auto canonical_source = MaskModelToJson(parsed).at("source");
    RequireCanonicalDump(source, canonical_source, context);
    return canonical_source;
  } catch (const std::exception& ex) {
    Fail(std::string{context} + ": " + ex.what());
  }
}

auto AssetKeyFromBrushSource(const nlohmann::json& source, std::string_view context)
    -> std::string {
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
        Fail(
            "SetParameter: document target must leave node_id, adjustment_instance_id, and "
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
  target.node_id = NodeId{OptionalIdFromJson(json.at("node_id"), "SetParameter target", "node_id")};
  target.adjustment_instance_id = AdjustmentInstanceId{OptionalIdFromJson(
      json.at("adjustment_instance_id"), "SetParameter target", "adjustment_instance_id")};
  target.mask_id = MaskId{OptionalIdFromJson(json.at("mask_id"), "SetParameter target", "mask_id")};
  ValidateParameterTarget(target);
  return target;
}

void RequireModelObject(const nlohmann::json& value, std::string_view context) {
  RequireObject(value, context);
  RejectNonFiniteNumbers(value, context);
}

auto RequireNonNegativeUint32(const nlohmann::json& json, const char* key, std::string_view context)
    -> std::uint32_t {
  const auto& value = json.at(key);
  if (value.is_number_unsigned()) {
    return value.get<std::uint32_t>();
  }
  if (value.is_number_integer() && value.get<std::int64_t>() >= 0) {
    return static_cast<std::uint32_t>(value.get<std::int64_t>());
  }
  Fail(std::string{context} + ": '" + key + "' must be a non-negative integer");
}

auto CanonicalBrushStrokeJson(const BrushStroke& stroke, std::string_view context)
    -> nlohmann::json {
  try {
    const auto json      = BrushStrokeToJson(stroke);
    const auto canonical = BrushStrokeToJson(BrushStrokeFromJson(json));
    RequireCanonicalDump(json, canonical, context);
    return canonical;
  } catch (const std::exception& ex) {
    Fail(std::string{context} + ": " + ex.what());
  }
}

auto StrokeFromCanonicalJson(const nlohmann::json& json, std::string_view context) -> BrushStroke {
  RequireObject(json, context);
  RejectNonFiniteNumbers(json, context);
  try {
    auto       stroke    = BrushStrokeFromJson(json);
    const auto canonical = BrushStrokeToJson(stroke);
    RequireCanonicalDump(json, canonical, context);
    return stroke;
  } catch (const std::exception& ex) {
    Fail(std::string{context} + ": " + ex.what());
  }
}

auto TranslationVectorFromJson(const nlohmann::json& json, std::string_view context) -> Vector2 {
  if (!json.is_array() || json.size() != 2 || !json[0].is_number() || !json[1].is_number()) {
    Fail(std::string{context} + ": translation must be an array of two numbers");
  }
  Vector2 value;
  value.x = json[0].get<float>();
  value.y = json[1].get<float>();
  if (!std::isfinite(value.x) || !std::isfinite(value.y)) {
    Fail(std::string{context} + ": translation components must be finite");
  }
  return value;
}

auto TranslationVectorToJson(Vector2 value) -> nlohmann::json {
  return nlohmann::json::array({value.x, value.y});
}

}  // namespace alcedo::pipeline_edit_json
