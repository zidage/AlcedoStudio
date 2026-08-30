//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_pipeline_command_service.hpp"

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/graph_validation.hpp"

namespace alcedo {
namespace {

void MergeJsonPatch(nlohmann::json& target, const nlohmann::json& patch) {
  if (!target.is_object() || !patch.is_object()) {
    target = patch;
    return;
  }
  for (const auto& [key, value] : patch.items()) {
    if (target.contains(key) && target[key].is_object() && value.is_object()) {
      MergeJsonPatch(target[key], value);
    } else {
      target[key] = value;
    }
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

auto ApplyEditorParameterPatch(PipelineDocument& candidate, const EditorParameterTarget& target,
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
        auto* grade = ColorGradeOf(candidate, target.node_id, error);
        if (grade == nullptr) {
          return false;
        }
        auto* model = grade->FindAdjustment(target.adjustment_instance_id);
        if (model == nullptr) {
          return SetError(error, "Adjustment instance is missing: " +
                                      std::string(target.adjustment_instance_id.Value()));
        }
        auto merged = model->ToJson();
        MergeJsonPatch(merged, patch);
        model->LoadJson(merged);
        return true;
      }
      case EditorParameterOwnerKind::Document: {
        auto merged = candidate.Geometry().ToJson();
        MergeJsonPatch(merged, patch);
        candidate.Geometry() = ImageGeometryModel::FromJson(merged);
        return true;
      }
      case EditorParameterOwnerKind::Develop: {
        auto* develop = candidate.Develop();
        if (develop == nullptr || develop->Id() != target.node_id) {
          return SetError(error, "Develop node is missing: " + std::string(target.node_id.Value()));
        }
        auto merged = develop->Params().ToJson();
        MergeJsonPatch(merged, patch);
        develop->Params().LoadJson(merged);
        return true;
      }
      case EditorParameterOwnerKind::DrtPost: {
        auto* drt = candidate.Drt();
        if (drt == nullptr || drt->Id() != target.node_id) {
          return SetError(error, "DRT node is missing: " + std::string(target.node_id.Value()));
        }
        auto merged = drt->Params().ToJson();
        MergeJsonPatch(merged, patch);
        drt->Params().LoadJson(merged);
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
  auto candidate = ClonePipelineDocument(live);
  if (!ApplyEditorParameterPatch(candidate, target, params, error)) {
    return false;
  }
  if (!PipelineDocumentPassesValidation(candidate, error)) {
    return false;
  }
  live = std::move(candidate);
  return true;
}

}  // namespace alcedo
