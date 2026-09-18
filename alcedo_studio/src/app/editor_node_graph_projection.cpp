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

auto ContainsNode(const std::vector<NodeId>& ids, const NodeId& id) -> bool {
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

}  // namespace

auto EditorNodeGraphProjection::KindOf(const INodeModel& node) -> EditorNodeKind {
  if (node.Type() == type_ids::DevelopNode()) {
    return EditorNodeKind::Develop;
  }
  if (node.Type() == type_ids::ColorGradeNode()) {
    return EditorNodeKind::ColorGrade;
  }
  if (node.Type() == type_ids::DrtNode()) {
    return EditorNodeKind::Drt;
  }
  throw std::invalid_argument("Unsupported Nodes-page node type: " +
                              std::string{node.Type().Text()});
}

auto EditorNodeGraphProjection::ProjectNode(const INodeModel& node) -> EditorNodeProjection {
  EditorNodeProjection projected;
  projected.node_id      = node.Id();
  projected.node_kind    = KindOf(node);
  projected.display_name = std::string{node.DisplayName()};
  if (projected.node_kind != EditorNodeKind::ColorGrade) {
    return projected;
  }
  const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(&node);
  if (grade == nullptr) {
    throw std::invalid_argument("Color Grade type has an invalid model");
  }
  projected.masks.reserve(grade->MaskCount());
  for (const auto& mask : grade->Masks()) {
    projected.masks.push_back({mask.id, GetMaskSourceKind(mask.source)});
  }
  return projected;
}

auto EditorNodeGraphProjection::Build(const PipelineDocument& document,
                                      std::uint64_t session_generation) -> EditorNodeGraphSnapshot {
  const auto backbone = document.Graph().ImageBackboneNodeIds();
  if (backbone.empty()) {
    throw std::invalid_argument("EditorNodeGraphProjection requires a valid image backbone");
  }

  EditorNodeGraphSnapshot snapshot;
  snapshot.session_generation = session_generation;
  snapshot.nodes.reserve(backbone.size());

  for (const auto& node_id : backbone) {
    const auto* node = document.Graph().FindNode(node_id);
    if (node == nullptr) {
      throw std::invalid_argument(
          "EditorNodeGraphProjection image backbone contains an unknown node");
    }

    snapshot.nodes.push_back(ProjectNode(*node));
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

auto EditorNodeGraphProjection::BuildMaskGroups(const PipelineDocument& document)
    -> EditorMaskGroupSnapshot {
  const auto backbone = document.Graph().ImageBackboneNodeIds();
  if (backbone.empty()) {
    throw std::invalid_argument("EditorMaskGroupProjection requires a valid image backbone");
  }

  EditorMaskGroupSnapshot snapshot;
  snapshot.groups.reserve(backbone.size());

  for (auto it = backbone.rbegin(); it != backbone.rend(); ++it) {
    const auto& node_id = *it;
    const auto* node    = document.Graph().FindNode(node_id);
    if (node == nullptr) {
      throw std::invalid_argument(
          "EditorMaskGroupProjection image backbone contains an unknown node");
    }
    if (KindOf(*node) != EditorNodeKind::ColorGrade) {
      continue;
    }
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(node);
    if (grade == nullptr) {
      throw std::invalid_argument("Color Grade type has an invalid model");
    }
    EditorMaskGroupRow group;
    group.node_id            = node_id;
    group.display_name       = std::string{node->DisplayName()};
    group.enabled            = grade->Enabled();
    group.deletion_protected = grade->DeletionProtected();
    group.masks.reserve(grade->MaskCount());
    for (const auto& mask : grade->Masks()) {
      EditorMaskGroupMaskRow row;
      row.node_id            = node_id;
      row.mask_id            = mask.id;
      row.source_kind        = GetMaskSourceKind(mask.source);
      row.display_name       = mask.display_name;
      row.enabled            = mask.enabled;
      row.deletion_protected = mask.deletion_protected;
      row.opacity            = mask.opacity;
      group.masks.push_back(std::move(row));
    }
    snapshot.groups.push_back(std::move(group));
  }
  return snapshot;
}

}  // namespace alcedo
