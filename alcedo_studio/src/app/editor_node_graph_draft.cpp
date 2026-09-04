//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_node_graph_draft.hpp"

#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_set>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"

namespace alcedo {

auto EditorNodeGraphDraft::ImagePort() -> PortId { return PortId{"image"}; }

auto EditorNodeGraphDraft::EdgeKey(const EditorNodeEdgeProjection& edge) -> std::string {
  return std::string{edge.source_node_id.Value()} + "/" + std::string{edge.source_port_id.Value()} +
         "->" + std::string{edge.destination_node_id.Value()} + "/" +
         std::string{edge.destination_port_id.Value()};
}

auto EditorNodeGraphDraft::EdgeKey(const PipelineSceneEdge& edge) -> std::string {
  return std::string{edge.from_node.Value()} + "/" + std::string{edge.from_port.Value()} + "->" +
         std::string{edge.to_node.Value()} + "/" + std::string{edge.to_port.Value()};
}

auto EditorNodeGraphDraft::ToSceneEdge(const EditorNodeEdgeProjection& edge) -> PipelineSceneEdge {
  return PipelineSceneEdge{edge.source_node_id, edge.source_port_id, edge.destination_node_id,
                           edge.destination_port_id};
}

auto EditorNodeGraphDraft::ToProjection(const PipelineSceneEdge& edge) -> EditorNodeEdgeProjection {
  return EditorNodeEdgeProjection{edge.from_node, edge.from_port, edge.to_node, edge.to_port};
}

auto EditorNodeGraphDraft::KindOf(const INodeModel& node) -> EditorNodeKind {
  if (node.Type() == type_ids::DevelopNode()) {
    return EditorNodeKind::Develop;
  }
  if (node.Type() == type_ids::DrtNode()) {
    return EditorNodeKind::Drt;
  }
  if (node.Type() == type_ids::ColorGradeNode()) {
    return EditorNodeKind::ColorGrade;
  }
  throw std::invalid_argument("Unsupported node type: " + std::string{node.Type().Text()});
}

auto EditorNodeGraphDraft::ProjectNode(const INodeModel& node) const -> EditorNodeProjection {
  EditorNodeProjection projected;
  projected.node_id      = node.Id();
  projected.node_kind    = KindOf(node);
  projected.display_name = std::string{node.DisplayName()};
  if (projected.node_kind == EditorNodeKind::ColorGrade) {
    const auto* grade = dynamic_cast<const ColorGradeNodeModel*>(&node);
    if (grade == nullptr) {
      throw std::invalid_argument("Color Grade type has an invalid model");
    }
    projected.masks.reserve(grade->MaskCount());
    for (const auto& mask : grade->Masks()) {
      projected.masks.push_back({mask.id, GetMaskSourceKind(mask.source)});
    }
  }
  return projected;
}

auto EditorNodeGraphDraft::FromDocument(const PipelineDocument& document,
                                        EditorNodeGraphDraftIdentity identity)
    -> EditorNodeGraphDraft {
  EditorNodeGraphDraft draft;
  draft.identity_                = std::move(identity);
  draft.base_next_name_number_   = document.NextColorGradeNameNumber();
  draft.draft_next_name_number_  = draft.base_next_name_number_;
  const auto& graph              = document.Graph();
  draft.nodes_.reserve(graph.Nodes().size());
  for (std::size_t index = 0; index < graph.Nodes().size(); ++index) {
    const auto* node = graph.Nodes()[index].get();
    if (node == nullptr) {
      throw std::invalid_argument("Pipeline graph contains a null node");
    }
    auto projected = draft.ProjectNode(*node);
    draft.node_json_[projected.node_id]      = node->ToJson();
    draft.base_node_json_[projected.node_id] = draft.node_json_[projected.node_id];
    draft.base_node_index_[projected.node_id] = index;
    draft.nodes_.push_back(std::move(projected));
  }
  draft.edges_.reserve(graph.Edges().size());
  draft.base_edges_.reserve(graph.Edges().size());
  for (std::size_t index = 0; index < graph.Edges().size(); ++index) {
    const auto& edge = graph.Edges()[index];
    const auto  scene = PipelineSceneEdge{edge.from_node, edge.from_port, edge.to_node, edge.to_port};
    draft.base_edges_.push_back(scene);
    draft.base_edge_index_[EdgeKey(scene)] = index;
    draft.edges_.push_back(ToProjection(scene));
  }
  draft.RebuildIndexes();
  return draft;
}

void EditorNodeGraphDraft::RebuildIndexes() {
  node_index_.clear();
  edge_index_.clear();
  outgoing_.clear();
  incoming_.clear();
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    node_index_[nodes_[index].node_id] = index;
    outgoing_[nodes_[index].node_id]   = std::nullopt;
    incoming_[nodes_[index].node_id]   = std::nullopt;
  }
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    const auto& edge = edges_[index];
    edge_index_[EdgeKey(edge)]              = index;
    outgoing_[edge.source_node_id]          = index;
    incoming_[edge.destination_node_id]     = index;
  }
}

auto EditorNodeGraphDraft::FindNode(const NodeId& node_id) const -> const EditorNodeProjection* {
  const auto it = node_index_.find(node_id);
  if (it == node_index_.end()) {
    return nullptr;
  }
  return &nodes_[it->second];
}

auto EditorNodeGraphDraft::NodeJson(const NodeId& node_id) const -> const nlohmann::json* {
  const auto it = node_json_.find(node_id);
  if (it == node_json_.end()) {
    return nullptr;
  }
  return &it->second;
}

auto EditorNodeGraphDraft::MatchesBase(const PipelineDocument& document) const -> bool {
  const auto& graph = document.Graph();
  if (graph.Nodes().size() != base_node_index_.size() ||
      graph.Edges().size() != base_edges_.size() ||
      document.NextColorGradeNameNumber() != base_next_name_number_) {
    return false;
  }
  for (std::size_t index = 0; index < graph.Nodes().size(); ++index) {
    const auto* node = graph.Nodes()[index].get();
    if (node == nullptr) {
      return false;
    }
    const auto found = base_node_index_.find(node->Id());
    if (found == base_node_index_.end() || found->second != index) {
      return false;
    }
  }
  for (std::size_t index = 0; index < graph.Edges().size(); ++index) {
    const auto& edge = graph.Edges()[index];
    if (!(base_edges_[index] ==
          PipelineSceneEdge{edge.from_node, edge.from_port, edge.to_node, edge.to_port})) {
      return false;
    }
  }
  return true;
}

void EditorNodeGraphDraft::CaptureCheckpoint() {
  checkpoint_.nodes                        = nodes_;
  checkpoint_.edges                        = edges_;
  checkpoint_.node_json                    = node_json_;
  checkpoint_.node_index                   = node_index_;
  checkpoint_.edge_index                   = edge_index_;
  checkpoint_.outgoing                     = outgoing_;
  checkpoint_.incoming                     = incoming_;
  checkpoint_.inserted_json                = inserted_json_;
  checkpoint_.removed_original_index       = removed_original_index_;
  checkpoint_.removed_json                 = removed_json_;
  checkpoint_.disconnected_original_index  = disconnected_original_index_;
  checkpoint_.disconnected_edge            = disconnected_edge_;
  checkpoint_.connected_edge               = connected_edge_;
  checkpoint_.draft_next_name_number       = draft_next_name_number_;
  has_checkpoint_                          = true;
}

void EditorNodeGraphDraft::RestoreLastMutation() {
  if (!has_checkpoint_) {
    return;
  }
  nodes_                       = checkpoint_.nodes;
  edges_                       = checkpoint_.edges;
  node_json_                   = checkpoint_.node_json;
  node_index_                  = checkpoint_.node_index;
  edge_index_                  = checkpoint_.edge_index;
  outgoing_                    = checkpoint_.outgoing;
  incoming_                    = checkpoint_.incoming;
  inserted_json_               = checkpoint_.inserted_json;
  removed_original_index_      = checkpoint_.removed_original_index;
  removed_json_                = checkpoint_.removed_json;
  disconnected_original_index_ = checkpoint_.disconnected_original_index;
  disconnected_edge_           = checkpoint_.disconnected_edge;
  connected_edge_              = checkpoint_.connected_edge;
  draft_next_name_number_      = checkpoint_.draft_next_name_number;
  has_checkpoint_              = false;
}

auto EditorNodeGraphDraft::DeltaEmpty() const -> bool {
  return inserted_json_.empty() && removed_json_.empty() && disconnected_edge_.empty() &&
         connected_edge_.empty() && draft_next_name_number_ == base_next_name_number_;
}

auto EditorNodeGraphDraft::WouldCreateCycle(const NodeId& source_id, const NodeId& destination_id,
                                            const std::optional<std::size_t>& skip_outgoing,
                                            const std::optional<std::size_t>& skip_incoming) const
    -> bool {
  std::unordered_set<std::string> visited;
  std::vector<NodeId>             stack{destination_id};
  while (!stack.empty()) {
    const auto current = stack.back();
    stack.pop_back();
    const auto key = std::string{current.Value()};
    if (!visited.insert(key).second) {
      continue;
    }
    if (current == source_id) {
      return true;
    }
    const auto out = outgoing_.find(current);
    if (out == outgoing_.end() || !out->second.has_value()) {
      continue;
    }
    const auto index = *out->second;
    if (skip_outgoing.has_value() && index == *skip_outgoing) {
      continue;
    }
    if (skip_incoming.has_value() && index == *skip_incoming) {
      continue;
    }
    stack.push_back(edges_[index].destination_node_id);
  }
  return false;
}

auto EditorNodeGraphDraft::AdmitConnect(const NodeId& source_id, const NodeId& destination_id,
                                        std::string* error) const -> bool {
  auto fail = [&](std::string message) {
    if (error != nullptr) {
      *error = std::move(message);
    }
    return false;
  };
  const auto* source      = FindNode(source_id);
  const auto* destination = FindNode(destination_id);
  if (source == nullptr || destination == nullptr) {
    return fail("That node is not in the current graph");
  }
  if (source_id == destination_id) {
    return fail("A node cannot connect to itself");
  }
  if (destination->node_kind == EditorNodeKind::Develop) {
    return fail("Develop has no incoming image port");
  }
  if (source->node_kind == EditorNodeKind::Drt) {
    return fail("DRT/Post has no outgoing image port");
  }
  if (source->node_kind != EditorNodeKind::Develop &&
      source->node_kind != EditorNodeKind::ColorGrade) {
    return fail("Unsupported source node type: " + std::string{source->node_id.Value()});
  }
  if (destination->node_kind != EditorNodeKind::Drt &&
      destination->node_kind != EditorNodeKind::ColorGrade) {
    return fail("Unsupported destination node type: " + std::string{destination->node_id.Value()});
  }
  const auto skip_out = outgoing_.at(source_id);
  const auto skip_in  = incoming_.at(destination_id);
  if (WouldCreateCycle(source_id, destination_id, skip_out, skip_in)) {
    return fail("That connection would create a cycle");
  }
  return true;
}

auto EditorNodeGraphDraft::RemoveEdgeAt(std::size_t index) -> EditorNodeEdgeProjection {
  auto edge = edges_[index];
  edges_.erase(edges_.begin() + static_cast<std::ptrdiff_t>(index));
  RebuildIndexes();
  return edge;
}

void EditorNodeGraphDraft::InsertEdge(EditorNodeEdgeProjection edge) {
  edges_.push_back(std::move(edge));
  RebuildIndexes();
}

auto EditorNodeGraphDraft::FinishMutation(EditorNodeGraphDraftMutation mutation)
    -> EditorNodeGraphDraftMutation {
  mutation.delta_empty      = DeltaEmpty();
  mutation.submission_valid = SubmissionValid();
  mutation.succeeded        = true;
  return mutation;
}

auto EditorNodeGraphDraft::AddColorGrade(NodeId node_id) -> EditorNodeGraphDraftMutation {
  EditorNodeGraphDraftMutation result;
  if (node_id.Empty() || FindNode(node_id) != nullptr) {
    result.error = "A Color Grade with that identity already exists";
    return result;
  }
  CaptureCheckpoint();
  auto model = CreateCleanColorGradeNode(node_id);
  model->SetDisplayName(DefaultColorGradeDisplayName(draft_next_name_number_));
  auto json      = model->ToJson();
  auto projected = ProjectNode(*model);
  nodes_.push_back(projected);
  node_json_[node_id]      = json;
  inserted_json_[node_id]  = json;
  ++draft_next_name_number_;
  RebuildIndexes();
  result.inserted_nodes.push_back(std::move(projected));
  return FinishMutation(std::move(result));
}

auto EditorNodeGraphDraft::RemoveColorGrade(const NodeId& node_id)
    -> EditorNodeGraphDraftMutation {
  EditorNodeGraphDraftMutation result;
  const auto* node = FindNode(node_id);
  if (node == nullptr) {
    result.error = "That node is not in the current graph";
    return result;
  }
  if (node->node_kind != EditorNodeKind::ColorGrade) {
    result.error = "Only a Color Grade can be deleted";
    return result;
  }
  CaptureCheckpoint();
  const auto out = outgoing_[node_id];
  const auto in  = incoming_[node_id];
  std::vector<std::size_t> remove_indexes;
  if (out.has_value()) {
    remove_indexes.push_back(*out);
  }
  if (in.has_value() && (!out.has_value() || *in != *out)) {
    remove_indexes.push_back(*in);
  }
  std::sort(remove_indexes.begin(), remove_indexes.end(), std::greater<>());
  for (const auto index : remove_indexes) {
    const auto edge = RemoveEdgeAt(index);
    result.removed_edges.push_back(edge);
    const auto key = EdgeKey(edge);
    const auto connected = connected_edge_.find(key);
    if (connected != connected_edge_.end()) {
      connected_edge_.erase(connected);
    } else if (base_edge_index_.contains(key)) {
      disconnected_original_index_[key] = base_edge_index_.at(key);
      disconnected_edge_[key]           = ToSceneEdge(edge);
    }
  }

  const auto node_pos = node_index_.at(node_id);
  const auto json     = node_json_.at(node_id);
  nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(node_pos));
  node_json_.erase(node_id);
  result.removed_node_ids.push_back(node_id);
  const auto inserted = inserted_json_.find(node_id);
  if (inserted != inserted_json_.end()) {
    inserted_json_.erase(inserted);
    std::uint64_t max_inserted = base_next_name_number_;
    for (const auto& [id, stored] : inserted_json_) {
      (void)id;
      if (stored.contains("display_name") && stored.at("display_name").is_string()) {
        const auto name = stored.at("display_name").get<std::string>();
        const auto prefix = std::string{"Color Grade "};
        if (name.rfind(prefix, 0) == 0) {
          try {
            const auto number = std::stoull(name.substr(prefix.size()));
            max_inserted      = std::max(max_inserted, number + 1);
          } catch (...) {
          }
        }
      }
    }
    draft_next_name_number_ = inserted_json_.empty() ? base_next_name_number_ : max_inserted;
  } else {
    removed_original_index_[node_id] = base_node_index_.at(node_id);
    removed_json_[node_id]           = json;
  }
  RebuildIndexes();
  return FinishMutation(std::move(result));
}

auto EditorNodeGraphDraft::Connect(const NodeId& source_id, const NodeId& destination_id)
    -> EditorNodeGraphDraftMutation {
  EditorNodeGraphDraftMutation result;
  std::string                  error;
  if (!AdmitConnect(source_id, destination_id, &error)) {
    result.error = std::move(error);
    return result;
  }
  const auto skip_out = outgoing_.at(source_id);
  const auto skip_in  = incoming_.at(destination_id);
  EditorNodeEdgeProjection proposed{source_id, ImagePort(), destination_id, ImagePort()};
  if (skip_out.has_value() && edges_[*skip_out].destination_node_id == destination_id &&
      edges_[*skip_out].source_node_id == source_id) {
    result.no_op      = true;
    result.succeeded  = true;
    result.delta_empty = DeltaEmpty();
    result.submission_valid = SubmissionValid();
    return result;
  }

  CaptureCheckpoint();
  std::vector<std::size_t> remove_indexes;
  if (skip_out.has_value()) {
    remove_indexes.push_back(*skip_out);
  }
  if (skip_in.has_value() && (!skip_out.has_value() || *skip_in != *skip_out)) {
    remove_indexes.push_back(*skip_in);
  }
  std::sort(remove_indexes.begin(), remove_indexes.end(), std::greater<>());
  for (const auto index : remove_indexes) {
    const auto edge = RemoveEdgeAt(index);
    result.removed_edges.push_back(edge);
    const auto key = EdgeKey(edge);
    const auto connected = connected_edge_.find(key);
    if (connected != connected_edge_.end()) {
      connected_edge_.erase(connected);
    } else if (base_edge_index_.contains(key)) {
      disconnected_original_index_[key] = base_edge_index_.at(key);
      disconnected_edge_[key]           = ToSceneEdge(edge);
    }
  }

  const auto key = EdgeKey(proposed);
  InsertEdge(proposed);
  result.inserted_edges.push_back(proposed);
  const auto disconnected = disconnected_edge_.find(key);
  if (disconnected != disconnected_edge_.end()) {
    disconnected_edge_.erase(disconnected);
    disconnected_original_index_.erase(key);
  } else {
    connected_edge_[key] = ToSceneEdge(proposed);
  }
  return FinishMutation(std::move(result));
}

auto EditorNodeGraphDraft::SubmissionValid() const -> bool {
  int develop_count = 0;
  int drt_count     = 0;
  const EditorNodeProjection* develop = nullptr;
  const EditorNodeProjection* drt     = nullptr;
  for (const auto& node : nodes_) {
    if (node.node_kind == EditorNodeKind::Develop) {
      ++develop_count;
      develop = &node;
    } else if (node.node_kind == EditorNodeKind::Drt) {
      ++drt_count;
      drt = &node;
    } else if (node.node_kind != EditorNodeKind::ColorGrade) {
      return false;
    }
  }
  if (develop_count != 1 || drt_count != 1 || develop == nullptr || drt == nullptr) {
    return false;
  }

  std::map<NodeId, int> outgoing_count;
  std::map<NodeId, int> incoming_count;
  for (const auto& edge : edges_) {
    if (edge.source_port_id != ImagePort() || edge.destination_port_id != ImagePort()) {
      return false;
    }
    ++outgoing_count[edge.source_node_id];
    ++incoming_count[edge.destination_node_id];
    if (outgoing_count[edge.source_node_id] > 1 || incoming_count[edge.destination_node_id] > 1) {
      return false;
    }
  }
  if (incoming_count[develop->node_id] != 0 || outgoing_count[drt->node_id] != 0) {
    return false;
  }

  std::unordered_set<std::string> on_path;
  NodeId                          current = develop->node_id;
  while (true) {
    const auto key = std::string{current.Value()};
    if (!on_path.insert(key).second) {
      return false;
    }
    if (current == drt->node_id) {
      break;
    }
    const auto out = outgoing_.find(current);
    if (out == outgoing_.end() || !out->second.has_value()) {
      return false;
    }
    current = edges_[*out->second].destination_node_id;
  }
  if (!on_path.contains(std::string{drt->node_id.Value()})) {
    return false;
  }
  for (const auto& node : nodes_) {
    if (node.node_kind == EditorNodeKind::ColorGrade &&
        !on_path.contains(std::string{node.node_id.Value()})) {
      return false;
    }
    if (node.node_kind != EditorNodeKind::Develop && incoming_count[node.node_id] != 1) {
      return false;
    }
    if (node.node_kind != EditorNodeKind::Drt && outgoing_count[node.node_id] != 1) {
      return false;
    }
  }
  return true;
}

auto EditorNodeGraphDraft::MakeChange() const -> NodeGraphTopologyChange {
  NodeGraphTopologyChange change;
  change.before_next_color_grade_name_number = base_next_name_number_;
  change.after_next_color_grade_name_number  = draft_next_name_number_;
  for (const auto& [id, json] : inserted_json_) {
    NodeGraphInsertedNode item;
    item.node              = json;
    item.final_node_index  = static_cast<std::uint32_t>(node_index_.at(id));
    change.inserted_nodes.push_back(std::move(item));
  }
  for (const auto& [id, json] : removed_json_) {
    NodeGraphRemovedNode item;
    item.node                 = json;
    item.original_node_index  = static_cast<std::uint32_t>(removed_original_index_.at(id));
    change.removed_nodes.push_back(std::move(item));
  }
  for (const auto& [key, edge] : disconnected_edge_) {
    NodeGraphDisconnectedEdge item;
    item.edge                 = edge;
    item.original_edge_index  = static_cast<std::uint32_t>(disconnected_original_index_.at(key));
    change.disconnected_edges.push_back(std::move(item));
  }
  for (const auto& [key, edge] : connected_edge_) {
    NodeGraphConnectedEdge item;
    item.edge             = edge;
    item.final_edge_index = static_cast<std::uint32_t>(edge_index_.at(key));
    change.connected_edges.push_back(std::move(item));
  }
  return change;
}

auto EditorNodeGraphDraft::CurrentSnapshot(std::uint64_t session_generation,
                                           std::uint64_t projection_revision,
                                           std::uint64_t topology_revision) const
    -> EditorNodeGraphSnapshot {
  EditorNodeGraphSnapshot snapshot;
  snapshot.session_generation  = session_generation;
  snapshot.projection_revision = projection_revision;
  snapshot.topology_revision   = topology_revision;
  snapshot.nodes               = nodes_;
  snapshot.edges               = edges_;
  return snapshot;
}

}  // namespace alcedo
