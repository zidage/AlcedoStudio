//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/pipeline_document.hpp"

#include <stdexcept>
#include <string>

#include "edit/graph/analytic_mask_node_model.hpp"
#include "edit/graph/raster_mask_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/scalar_operator_model.hpp"

namespace alcedo {

namespace {

template <class T>
auto Downcast(INodeModel* node) -> T* {
  return dynamic_cast<T*>(node);
}

template <class T>
auto Downcast(const INodeModel* node) -> const T* {
  return dynamic_cast<const T*>(node);
}

auto NodeFromJson(const nlohmann::json& json) -> std::unique_ptr<INodeModel> {
  const auto type = json.at("type").get<std::string>();
  if (type == type_ids::DevelopNode().Text()) {
    return DevelopNodeModel::FromJson(json);
  }
  if (type == type_ids::ColorGradeNode().Text()) {
    return ColorGradeNodeModel::FromJson(json);
  }
  if (type == type_ids::DrtNode().Text()) {
    return DrtNodeModel::FromJson(json);
  }
  if (type == type_ids::AnalyticMaskNode().Text()) {
    return AnalyticMaskNodeModel::FromJson(json);
  }
  if (type == type_ids::RasterMaskNode().Text()) {
    return RasterMaskNodeModel::FromJson(json);
  }
  throw std::runtime_error("Unknown node type: " + type);
}

}  // namespace

auto PipelineDocument::Develop() -> DevelopNodeModel* {
  return Downcast<DevelopNodeModel>(graph_.FindNode("develop"));
}

auto PipelineDocument::Develop() const -> const DevelopNodeModel* {
  return Downcast<DevelopNodeModel>(graph_.FindNode("develop"));
}

auto PipelineDocument::PrimaryGrade() -> ColorGradeNodeModel* {
  return Downcast<ColorGradeNodeModel>(graph_.FindNode("grade.primary"));
}

auto PipelineDocument::PrimaryGrade() const -> const ColorGradeNodeModel* {
  return Downcast<ColorGradeNodeModel>(graph_.FindNode("grade.primary"));
}

auto PipelineDocument::Drt() -> DrtNodeModel* {
  return Downcast<DrtNodeModel>(graph_.FindNode("drt"));
}

auto PipelineDocument::Drt() const -> const DrtNodeModel* {
  return Downcast<DrtNodeModel>(graph_.FindNode("drt"));
}

void PipelineDocument::InsertAdjustment(const NodeId& grade_id, std::size_t index,
                                        AdjustmentInstanceId instance_id,
                                        std::unique_ptr<IOperatorModel> model) {
  auto* grade = Downcast<ColorGradeNodeModel>(graph_.FindNode(grade_id));
  if (grade == nullptr) {
    throw std::invalid_argument("InsertAdjustment: node is not a ColorGrade");
  }
  grade->InsertAdjustment(index, std::move(instance_id), std::move(model));
  MarkTopologyDirty();
}

auto PipelineDocument::ToJson() const -> nlohmann::json {
  nlohmann::json nodes = nlohmann::json::array();
  for (const auto& node : graph_.Nodes()) {
    nodes.push_back(node->ToJson());
  }
  nlohmann::json edges = nlohmann::json::array();
  for (const auto& edge : graph_.Edges()) {
    edges.push_back({{"from", nlohmann::json::array({std::string{edge.from_node.Value()},
                                                     std::string{edge.from_port.Value()}})},
                     {"to", nlohmann::json::array({std::string{edge.to_node.Value()},
                                                   std::string{edge.to_port.Value()}})}});
  }
  return {{"format_version", format_version_},
          {"geometry", geometry_.ToJson()},
          {"nodes", std::move(nodes)},
          {"edges", std::move(edges)}};
}

auto PipelineDocument::FromJson(const nlohmann::json& json) -> PipelineDocument {
  PipelineDocument document;
  if (!json.contains("format_version") || json["format_version"].get<std::uint32_t>() !=
                                              kPipelineDocumentFormatVersion) {
    throw std::runtime_error("Unsupported pipeline document format_version");
  }
  document.format_version_ = kPipelineDocumentFormatVersion;
  if (json.contains("geometry")) {
    document.geometry_ = ImageGeometryModel::FromJson(json["geometry"]);
  }
  if (json.contains("nodes") && json["nodes"].is_array()) {
    for (const auto& node_json : json["nodes"]) {
      document.graph_.AddNode(NodeFromJson(node_json));
    }
  }
  if (json.contains("edges") && json["edges"].is_array()) {
    for (const auto& edge_json : json["edges"]) {
      const auto from = edge_json.at("from");
      const auto to   = edge_json.at("to");
      document.graph_.Connect(NodeId{from.at(0).get<std::string>()},
                              PortId{from.at(1).get<std::string>()},
                              NodeId{to.at(0).get<std::string>()}, PortId{to.at(1).get<std::string>()});
    }
  }
  document.topology_dirty_ = true;
  return document;
}

auto CreateDefaultPipelineDocument() -> PipelineDocument {
  PipelineDocument document;
  document.Graph().AddNode(std::make_unique<DevelopNodeModel>(NodeId{"develop"}));
  document.Graph().AddNode(ColorGradeNodeModel::MakeDefault(NodeId{"grade.primary"}));
  document.Graph().AddNode(std::make_unique<DrtNodeModel>(NodeId{"drt"}));
  document.Graph().Connect(NodeId{"develop"}, PortId{"image"}, NodeId{"grade.primary"},
                           PortId{"image"});
  document.Graph().Connect(NodeId{"grade.primary"}, PortId{"image"}, NodeId{"drt"}, PortId{"image"});
  document.MarkTopologyDirty();
  return document;
}

void AttachTemporaryPrimaryGradeOvalMask(PipelineDocument& document) {
  auto* grade = document.PrimaryGrade();
  if (grade == nullptr) {
    throw std::invalid_argument(
        "AttachTemporaryPrimaryGradeOvalMask: missing primary color grade");
  }

  const NodeId mask_id{"mask.ui_test.radial"};
  if (document.Graph().FindNode(mask_id) == nullptr) {
    auto node = std::make_unique<AnalyticMaskNodeModel>(mask_id, AnalyticMaskKind::Radial);
    RadialMaskParams radial;
    radial.center_x      = 0.5f;
    radial.center_y      = 0.5f;
    radial.major_radius  = 0.32f;
    radial.minor_radius  = 0.20f;
    radial.rotation      = 0.0f;
    radial.inner_feather = 0.0f;
    radial.outer_feather = 0.12f;
    radial.invert        = false;
    node->SetRadial(radial);
    document.Graph().AddNode(std::move(node));
    document.Graph().Connect(mask_id, PortId{"mask"}, NodeId{"grade.primary"}, PortId{"mask"});
    document.MarkTopologyDirty();
  }

  auto* exposure =
      dynamic_cast<ExposureModel*>(grade->FindAdjustmentByType(type_ids::Exposure()));
  if (exposure == nullptr) {
    throw std::invalid_argument("AttachTemporaryPrimaryGradeOvalMask: missing exposure adjustment");
  }
  if (exposure->Value() == ExposureTraits::kDefault) {
    exposure->SetValue(1.0f);
  }
}

auto AllowsLegacyStageAdapterRemirror(const PipelineDocument& document) -> bool {
  const auto count = document.Graph().NodeCount();
  if (count == 3U) {
    return true;
  }
  return kTemporaryPrimaryGradeOvalMask && count == 4U &&
         document.Graph().FindNode(NodeId{"mask.ui_test.radial"}) != nullptr;
}

}  // namespace alcedo
