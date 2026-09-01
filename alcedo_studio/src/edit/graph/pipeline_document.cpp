//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "edit/graph/pipeline_document.hpp"

#include <cmath>
#include <stdexcept>
#include <string>

#include "edit/operators/models/builtin_type_ids.hpp"

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
  if (type == type_ids::AnalyticMaskNode().Text() || type == type_ids::RasterMaskNode().Text()) {
    throw std::runtime_error(
        "Unsupported pipeline document: top-level Mask nodes are not allowed");
  }
  throw std::runtime_error("Unknown node type: " + type);
}

void ValidateFiniteNumbers(const nlohmann::json& value, const std::string& path) {
  if (value.is_number_float() && !std::isfinite(value.get<double>())) {
    throw std::runtime_error("Pipeline document contains a non-finite number at " + path);
  }
  if (value.is_array()) {
    for (std::size_t index = 0; index < value.size(); ++index) {
      ValidateFiniteNumbers(value[index], path + "[" + std::to_string(index) + "]");
    }
    return;
  }
  if (value.is_object()) {
    for (const auto& [key, child] : value.items()) {
      ValidateFiniteNumbers(child, path + "." + key);
    }
  }
}

void ValidateDocumentShape(const nlohmann::json& json) {
  if (!json.is_object()) {
    throw std::runtime_error("Pipeline document must be an object");
  }
  if (!json.contains("format_version") || !json["format_version"].is_number_integer() ||
      json["format_version"].get<std::uint32_t>() != kPipelineDocumentFormatVersion) {
    throw std::runtime_error("Unsupported pipeline document format_version");
  }
  if (!json.contains("geometry") || !json["geometry"].is_object()) {
    throw std::runtime_error("Pipeline document is missing an object geometry");
  }
  if (!json.contains("nodes") || !json["nodes"].is_array()) {
    throw std::runtime_error("Pipeline document is missing a nodes array");
  }
  if (!json.contains("edges") || !json["edges"].is_array()) {
    throw std::runtime_error("Pipeline document is missing an edges array");
  }

  for (std::size_t index = 0; index < json["nodes"].size(); ++index) {
    const auto& node = json["nodes"][index];
    const auto  path = "nodes[" + std::to_string(index) + "]";
    if (!node.is_object() || !node.contains("id") || !node["id"].is_string() ||
        node["id"].get<std::string>().empty() || !node.contains("type") ||
        !node["type"].is_string() || node["type"].get<std::string>().empty()) {
      throw std::runtime_error("Pipeline document has an invalid node at " + path);
    }

    const auto type = node["type"].get<std::string>();
    if (type == type_ids::AnalyticMaskNode().Text() || type == type_ids::RasterMaskNode().Text()) {
      throw std::runtime_error(
          "Unsupported pipeline document: top-level Mask nodes are not allowed");
    }
    if (type == type_ids::ColorGradeNode().Text() || type == type_ids::DrtNode().Text()) {
      if (type == type_ids::DrtNode().Text() &&
          (!node.contains("params") || !node["params"].is_object())) {
        throw std::runtime_error("Pipeline document DRT is missing object params at " + path);
      }
      if (!node.contains("adjustments") || !node["adjustments"].is_array()) {
        throw std::runtime_error("Pipeline document " +
                                 (type == type_ids::DrtNode().Text() ? std::string{"DRT"}
                                                                     : std::string{"ColorGrade"}) +
                                 " is missing adjustments at " + path);
      }
      for (std::size_t adjustment_index = 0; adjustment_index < node["adjustments"].size();
           ++adjustment_index) {
        const auto& adjustment = node["adjustments"][adjustment_index];
        const auto adjustment_path = path + ".adjustments[" +
                                     std::to_string(adjustment_index) + "]";
        if (!adjustment.is_object() || !adjustment.contains("id") ||
            !adjustment["id"].is_string() || adjustment["id"].get<std::string>().empty() ||
            !adjustment.contains("type") || !adjustment["type"].is_string() ||
            adjustment["type"].get<std::string>().empty() || !adjustment.contains("params") ||
            !adjustment["params"].is_object()) {
          throw std::runtime_error("Pipeline document has an invalid adjustment at " +
                                   adjustment_path);
        }
      }
      if (type == type_ids::ColorGradeNode().Text() &&
          (!node.contains("masks") || !node["masks"].is_array())) {
        throw std::runtime_error("Pipeline document ColorGrade is missing a masks array at " +
                                 path);
      }
    } else if (!node.contains("params") || !node["params"].is_object()) {
      throw std::runtime_error("Pipeline document node is missing object params at " + path);
    }
  }

  for (std::size_t index = 0; index < json["edges"].size(); ++index) {
    const auto& edge = json["edges"][index];
    const auto  path = "edges[" + std::to_string(index) + "]";
    if (!edge.is_object() || !edge.contains("from") || !edge.contains("to") ||
        !edge["from"].is_array() || edge["from"].size() != 2 ||
        !edge["to"].is_array() || edge["to"].size() != 2 ||
        !edge["from"][0].is_string() || !edge["from"][1].is_string() ||
        !edge["to"][0].is_string() || !edge["to"][1].is_string()) {
      throw std::runtime_error("Pipeline document has an invalid edge at " + path);
    }
    const auto from_port = edge["from"][1].get<std::string>();
    const auto to_port   = edge["to"][1].get<std::string>();
    if (from_port == "mask" || to_port == "mask") {
      throw std::runtime_error("Unsupported pipeline document: Mask edges are not allowed");
    }
  }

  ValidateFiniteNumbers(json, "document");
}

void ApplyDefaultPipelineLook(ColorGradeNodeModel& grade) {
  auto* exposure   = grade.FindAdjustmentByType(type_ids::Exposure());
  auto* saturation = grade.FindAdjustmentByType(type_ids::Saturation());
  if (exposure == nullptr || saturation == nullptr) {
    throw std::logic_error("Default Color Grade is missing exposure or saturation");
  }
  exposure->LoadJson({{"exposure_ev", kDefaultPipelineExposureEv}});
  saturation->LoadJson({{"saturation", kDefaultPipelineSaturation}});
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
  ValidateDocumentShape(json);
  PipelineDocument document;
  document.format_version_ = kPipelineDocumentFormatVersion;
  document.geometry_ = ImageGeometryModel::FromJson(json["geometry"]);
  for (const auto& node_json : json["nodes"]) {
    document.graph_.AddNode(NodeFromJson(node_json));
  }
  for (const auto& edge_json : json["edges"]) {
    const auto from = edge_json.at("from");
    const auto to   = edge_json.at("to");
    document.graph_.Connect(NodeId{from.at(0).get<std::string>()},
                            PortId{from.at(1).get<std::string>()},
                            NodeId{to.at(0).get<std::string>()}, PortId{to.at(1).get<std::string>()});
  }
  document.topology_dirty_ = true;
  return document;
}

auto CreateDefaultPipelineDocument() -> PipelineDocument {
  PipelineDocument document;
  document.Graph().AddNode(std::make_unique<DevelopNodeModel>(NodeId{"develop"}));
  auto grade = ColorGradeNodeModel::MakeDefault(NodeId{"grade.primary"});
  ApplyDefaultPipelineLook(*grade);
  document.Graph().AddNode(std::move(grade));
  document.Graph().AddNode(DrtNodeModel::MakeDefault(NodeId{"drt"}));
  document.Graph().Connect(NodeId{"develop"}, PortId{"image"}, NodeId{"grade.primary"},
                           PortId{"image"});
  document.Graph().Connect(NodeId{"grade.primary"}, PortId{"image"}, NodeId{"drt"}, PortId{"image"});
  document.MarkTopologyDirty();
  return document;
}

auto ClonePipelineDocument(const PipelineDocument& src) -> PipelineDocument {
  return PipelineDocument::FromJson(src.ToJson());
}

auto ColorGradesOnImageBackbone(const PipelineDocument& document)
    -> std::vector<const ColorGradeNodeModel*> {
  std::vector<const ColorGradeNodeModel*> grades;
  for (const auto& id : document.Graph().ImageBackboneNodeIds()) {
    const auto* grade = Downcast<ColorGradeNodeModel>(document.Graph().FindNode(id));
    if (grade != nullptr) {
      grades.push_back(grade);
    }
  }
  return grades;
}

auto AllowsLegacyStageAdapterRemirror(const PipelineDocument& document) -> bool {
  return document.Develop() != nullptr && document.PrimaryGrade() != nullptr &&
         document.Drt() != nullptr;
}

}  // namespace alcedo
