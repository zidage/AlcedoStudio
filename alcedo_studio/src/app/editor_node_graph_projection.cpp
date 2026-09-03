//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_node_graph_projection.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {
namespace {

auto NodeKindOf(const INodeModel& node) -> EditorNodeKind {
  if (node.Type() == type_ids::DevelopNode()) {
    return EditorNodeKind::Develop;
  }
  if (node.Type() == type_ids::ColorGradeNode()) {
    return EditorNodeKind::ColorGrade;
  }
  if (node.Type() == type_ids::DrtNode()) {
    return EditorNodeKind::Drt;
  }
  throw std::invalid_argument("EditorNodeGraphProjection encountered an unsupported node type");
}

auto ContainsNode(const std::vector<NodeId>& ids, const NodeId& id) -> bool {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

auto EditorNodeGraphProjection::Build(const PipelineDocument& document,
                                      std::uint64_t           session_generation,
                                      std::uint64_t           projection_revision,
                                      std::uint64_t topology_revision) -> EditorNodeGraphSnapshot {
  const auto backbone = document.Graph().ImageBackboneNodeIds();
  if (backbone.empty()) {
    throw std::invalid_argument("EditorNodeGraphProjection requires a valid image backbone");
  }

  EditorNodeGraphSnapshot snapshot;
  snapshot.session_generation  = session_generation;
  snapshot.projection_revision = projection_revision;
  snapshot.topology_revision   = topology_revision;
  snapshot.nodes.reserve(backbone.size());

  for (const auto& node_id : backbone) {
    const auto* node = document.Graph().FindNode(node_id);
    if (node == nullptr) {
      throw std::invalid_argument(
          "EditorNodeGraphProjection image backbone contains an unknown node");
    }

    EditorNodeProjection projected;
    projected.node_id      = node->Id();
    projected.node_kind    = NodeKindOf(*node);
    projected.display_name = std::string{node->DisplayName()};

    if (projected.node_kind == EditorNodeKind::ColorGrade) {
      const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node);
      if (grade == nullptr) {
        throw std::invalid_argument(
            "EditorNodeGraphProjection Color Grade type has an invalid model");
      }
      projected.masks.reserve(grade->MaskCount());
      for (const auto& mask : grade->Masks()) {
        projected.masks.push_back({mask.id, GetMaskSourceKind(mask.source)});
      }
    }
    snapshot.nodes.push_back(std::move(projected));
  }

  snapshot.edges.reserve(document.Graph().Edges().size());
  for (const auto& edge : document.Graph().Edges()) {
    if (!ContainsNode(backbone, edge.from_node) || !ContainsNode(backbone, edge.to_node)) {
      continue;
    }
    snapshot.edges.push_back({edge.from_node, edge.from_port, edge.to_node, edge.to_port});
  }
  return snapshot;
}

auto EditorNodeGraphProjection::AcceptsGeneration(const EditorNodeGraphSnapshot& snapshot,
                                                  std::uint64_t session_generation) -> bool {
  return snapshot.session_generation == session_generation;
}

}  // namespace alcedo
