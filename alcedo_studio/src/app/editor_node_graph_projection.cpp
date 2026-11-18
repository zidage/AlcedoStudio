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
  projected.deletion_protected = grade->DeletionProtected();
  projected.hidden             = !grade->Enabled();
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

  const auto detached = document.Graph().DetachedColorGradeNodeIds();

  EditorNodeGraphSnapshot snapshot;
  snapshot.session_generation = session_generation;
  snapshot.nodes.reserve(backbone.size() + detached.size());

  for (const auto& node_id : backbone) {
    const auto* node = document.Graph().FindNode(node_id);
    if (node == nullptr) {
      throw std::invalid_argument(
          "EditorNodeGraphProjection image backbone contains an unknown node");
    }

    snapshot.nodes.push_back(ProjectNode(*node));
  }
  for (const auto& node_id : detached) {
    auto projected     = ProjectNode(*document.Graph().FindNode(node_id));
    projected.detached = true;
    snapshot.nodes.push_back(std::move(projected));
  }

  // A detached Color Grade has no edge, so the backbone holds every edge endpoint.
  snapshot.edges.reserve(document.Graph().Edges().size());
  for (const auto& edge : document.Graph().Edges()) {
    if (!ContainsNode(backbone, edge.from_node) || !ContainsNode(backbone, edge.to_node)) {
      continue;
    }
    snapshot.edges.push_back({edge.from_node, edge.from_port, edge.to_node, edge.to_port});
  }
  snapshot.default_grade_id = document.DefaultGradeId();
  return snapshot;
}

auto EditorNodeGraphProjection::BuildMaskGroups(const PipelineDocument& document)
    -> EditorMaskGroupSnapshot {
  const auto backbone = document.Graph().ImageBackboneNodeIds();
  if (backbone.empty()) {
    throw std::invalid_argument("EditorMaskGroupProjection requires a valid image backbone");
  }

  // Downstream-first backbone order, then detached groups in storage order.
  std::vector<NodeId> group_ids(backbone.rbegin(), backbone.rend());
  const auto          detached = document.Graph().DetachedColorGradeNodeIds();
  group_ids.insert(group_ids.end(), detached.begin(), detached.end());

  EditorMaskGroupSnapshot snapshot;
  snapshot.groups.reserve(group_ids.size());

  for (const auto& node_id : group_ids) {
    const auto* node = document.Graph().FindNode(node_id);
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
    group.detached           = !ContainsNode(backbone, node_id);
    group.deletion_protected = grade->DeletionProtected();
    group.masks.reserve(grade->MaskCount());
    for (const auto& mask : grade->Masks()) {
      EditorMaskGroupMaskRow row;
      row.node_id            = node_id;
      row.mask_id            = mask.id;
      row.source_kind        = GetMaskSourceKind(mask.source);
      row.display_name       = mask.display_name;
      row.enabled            = mask.enabled;
      row.invert             = mask.invert;
      row.opacity            = mask.opacity;
      group.masks.push_back(std::move(row));
    }
    snapshot.groups.push_back(std::move(group));
  }
  return snapshot;
}

}  // namespace alcedo
