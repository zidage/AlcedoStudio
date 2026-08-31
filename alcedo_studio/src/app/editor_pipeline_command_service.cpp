//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pipeline_command_service.hpp"

#include <cmath>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/graph_validation.hpp"

namespace alcedo {
namespace {

/// Validate supplied values before any Model setter runs. Arrays replace whole fields;
/// curve points are variable length, other parameter arrays have fixed dimensions.
void ValidatePatch(const nlohmann::json& current, const nlohmann::json& patch,
                   const std::string& path = {}) {
  if (current.is_number()) {
    if (!patch.is_number() || !std::isfinite(patch.get<double>()) ||
        !std::isfinite(patch.get<float>())) {
      throw std::invalid_argument("Expected finite number: " + path);
    }
    return;
  }
  if (current.type() != patch.type()) {
    throw std::invalid_argument("Invalid parameter type: " + path);
  }
  if (current.is_object()) {
    for (const auto& [key, value] : patch.items()) {
      if (!current.contains(key)) {
        throw std::invalid_argument("Unknown parameter: " + path + key);
      }
      ValidatePatch(current.at(key), value, path + key + ".");
    }
  } else if (current.is_array()) {
    const bool points = path == "points.";
    if ((points && patch.size() < 2) || (!points && current.size() != patch.size())) {
      throw std::invalid_argument("Invalid parameter array length: " + path);
    }
    for (std::size_t i = 0; i < patch.size(); ++i) {
      ValidatePatch(current.at(points ? 0 : i), patch.at(i), path + std::to_string(i) + ".");
    }
  }
}

/// Merge an already validated partial parameter object, without touching a Model.
void MergeJsonPatch(nlohmann::json& target, const nlohmann::json& patch) {
  for (const auto& [key, value] : patch.items()) {
    if (target.contains(key) && target[key].is_object() && value.is_object()) {
      MergeJsonPatch(target[key], value);
    } else {
      target[key] = value;
    }
  }
}

/// Apply to the existing Model; a throwing setter restores only this Model's values.
/// Restoration failure escapes as a fatal error instead of reporting a usable document.
void ApplyModelPatch(IOperatorModel& model, const nlohmann::json& patch) {
  const auto before = model.ToJson();
  ValidatePatch(before, patch);
  auto merged = before;
  MergeJsonPatch(merged, patch);
  try {
    model.LoadJson(merged);
  } catch (...) {
    const auto failure = std::current_exception();
    try {
      model.LoadJson(before);
    } catch (const std::exception& ex) {
      throw std::runtime_error(std::string{"Model parameter restoration failed: "} + ex.what());
    }
    std::rethrow_exception(failure);
  }
}

auto SetError(std::string* error, std::string message) -> bool {
  if (error != nullptr) {
    *error = std::move(message);
  }
  return false;
}

auto JoinValidation(const std::vector<GraphValidationError>& errors) -> std::string {
  std::string text;
  for (const auto& item : errors) {
    if (!text.empty()) {
      text += "; ";
    }
    text += item.message;
  }
  return text;
}

auto ColorGradeOf(PipelineDocument& document, const NodeId& id, std::string* error)
    -> ColorGradeNodeModel* {
  auto* grade = dynamic_cast<ColorGradeNodeModel*>(document.Graph().FindNode(id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string(id.Value()));
    return nullptr;
  }
  return grade;
}

auto ColorGradeOf(const PipelineDocument& document, const NodeId& id, std::string* error)
    -> const ColorGradeNodeModel* {
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(document.Graph().FindNode(id));
  if (grade == nullptr) {
    SetError(error, "Color Grade node is missing: " + std::string(id.Value()));
    return nullptr;
  }
  return grade;
}

}  // namespace

auto ApplyEditorParameterPatch(PipelineDocument& document, const EditorParameterTarget& target,
                               const nlohmann::json& params, std::string* error) -> bool {
  const auto target_error = DescribeEditorParameterTargetError(target, target.field_key);
  if (!target_error.empty()) {
    return SetError(error, target_error);
  }
  if (!params.is_object() && !params.is_null()) {
    return SetError(error, "Editor parameter params must be a JSON object");
  }
  const nlohmann::json patch = params.is_null() ? nlohmann::json::object() : params;

  try {
    switch (target.owner_kind) {
      case EditorParameterOwnerKind::ColorGrade: {
        auto* grade = ColorGradeOf(document, target.node_id, error);
        if (grade == nullptr) {
          return false;
        }
        auto* model = grade->FindAdjustment(target.adjustment_instance_id);
        if (model == nullptr) {
          return SetError(error, "Adjustment instance is missing: " +
                                      std::string(target.adjustment_instance_id.Value()));
        }
        ApplyModelPatch(*model, patch);
        return true;
      }
      case EditorParameterOwnerKind::Document: {
        auto merged = document.Geometry().ToJson();
        ValidatePatch(merged, patch);
        MergeJsonPatch(merged, patch);
        document.Geometry() = ImageGeometryModel::FromJson(merged);
        return true;
      }
      case EditorParameterOwnerKind::Develop: {
        auto* develop = document.Develop();
        if (develop == nullptr || develop->Id() != target.node_id) {
          return SetError(error, "Develop node is missing: " + std::string(target.node_id.Value()));
        }
        ApplyModelPatch(develop->Params(), patch);
        return true;
      }
      case EditorParameterOwnerKind::DrtPost: {
        auto* drt = document.Drt();
        if (drt == nullptr || drt->Id() != target.node_id) {
          return SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
        }
        ApplyModelPatch(drt->Params(), patch);
        return true;
      }
      default:
        return SetError(error, "Editor parameter target owner_kind is not supported");
    }
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto ReadEditorParameterJson(const PipelineDocument& document, const EditorParameterTarget& target,
                             nlohmann::json* json, std::string* error) -> bool {
  if (json == nullptr) {
    return SetError(error, "Editor parameter JSON storage is required");
  }
  const auto target_error = DescribeEditorParameterTargetError(target, target.field_key);
  if (!target_error.empty()) {
    return SetError(error, target_error);
  }
  try {
    switch (target.owner_kind) {
      case EditorParameterOwnerKind::ColorGrade: {
        const auto* grade = ColorGradeOf(document, target.node_id, error);
        if (grade == nullptr) {
          return false;
        }
        const auto* model = grade->FindAdjustment(target.adjustment_instance_id);
        if (model == nullptr) {
          return SetError(error, "Adjustment instance is missing: " +
                                      std::string(target.adjustment_instance_id.Value()));
        }
        *json = model->ToJson();
        return true;
      }
      case EditorParameterOwnerKind::Document:
        *json = document.Geometry().ToJson();
        return true;
      case EditorParameterOwnerKind::Develop: {
        const auto* develop = document.Develop();
        if (develop == nullptr || develop->Id() != target.node_id) {
          return SetError(error, "Develop node is missing: " + std::string(target.node_id.Value()));
        }
        *json = develop->Params().ToJson();
        return true;
      }
      case EditorParameterOwnerKind::DrtPost: {
        const auto* drt = document.Drt();
        if (drt == nullptr || drt->Id() != target.node_id) {
          return SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
        }
        *json = drt->Params().ToJson();
        return true;
      }
      default:
        return SetError(error, "Editor parameter target owner_kind is not supported");
    }
  } catch (const std::exception& ex) {
    return SetError(error, ex.what());
  }
}

auto CanonicalPipelineDocumentJson(const PipelineDocument& document) -> std::string {
  return document.ToJson().dump();
}

auto PipelineDocumentPassesValidation(const PipelineDocument& document, std::string* error)
    -> bool {
  auto errors = document.Graph().Validate();
  const auto backbone = document.Graph().ValidateImageBackbone();
  errors.insert(errors.end(), backbone.begin(), backbone.end());
  if (errors.empty()) {
    return true;
  }
  return SetError(error, JoinValidation(errors));
}

auto PublishEditorParameterPatch(PipelineDocument& live, const EditorParameterTarget& target,
                                 const nlohmann::json& params, std::string* error) -> bool {
  return ApplyEditorParameterPatch(live, target, params, error);
}

}  // namespace alcedo
