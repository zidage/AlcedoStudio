//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_node_graph_draft.hpp"

#include <algorithm>
#include <stdexcept>
#include <type_traits>
#include <unordered_set>
#include <utility>

#include "edit/graph/color_grade_node_model.hpp"

namespace alcedo {
namespace {

auto MaxInsertedNameNumber(const std::map<NodeId, nlohmann::json>& inserted,
                           std::uint64_t                           base_number) -> std::uint64_t {
  std::uint64_t max_inserted = base_number;
  const auto    prefix       = std::string{"Color Grade "};
  for (const auto& [id, stored] : inserted) {
    (void)id;
    if (!stored.contains("display_name") || !stored.at("display_name").is_string()) {
      continue;
    }
    const auto name = stored.at("display_name").get<std::string>();
    if (name.rfind(prefix, 0) != 0) {
      continue;
    }
    try {
      const auto number = std::stoull(name.substr(prefix.size()));
      max_inserted      = std::max(max_inserted, number + 1);
    } catch (...) {
    }
  }
  return max_inserted;
}

}  // namespace

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

template <class Map, class Key>
void EditorNodeGraphDraft::Remember(
    std::map<Key, std::optional<typename Map::mapped_type>>* prior, const Map& current,
    const Key& key) {
  if (prior->contains(key)) {
    return;
  }
  const auto it = current.find(key);
  if (it == current.end()) {
    prior->emplace(key, std::nullopt);
    return;
  }
  prior->emplace(key, it->second);
  if constexpr (std::is_same_v<typename Map::mapped_type, nlohmann::json>) {
    ++work_stats_.json_entry_copies;
  }
}

template <class Map, class Key>
void EditorNodeGraphDraft::RestoreMap(
    Map* current, const std::map<Key, std::optional<typename Map::mapped_type>>& prior) {
  for (const auto& [key, value] : prior) {
    if (!value.has_value()) {
      current->erase(key);
    } else {
      (*current)[key] = *value;
    }
  }
}

auto EditorNodeGraphDraft::FromDocument(const PipelineDocument& document,
                                        EditorNodeGraphDraftIdentity identity)
    -> EditorNodeGraphDraft {
  EditorNodeGraphDraft draft;
  draft.identity_               = std::move(identity);
  draft.base_next_name_number_  = document.NextColorGradeNameNumber();
  draft.draft_next_name_number_ = draft.base_next_name_number_;
  const auto& graph             = document.Graph();
  draft.nodes_.reserve(graph.Nodes().size());
  for (std::size_t index = 0; index < graph.Nodes().size(); ++index) {
    const auto* node = graph.Nodes()[index].get();
    if (node == nullptr) {
      throw std::invalid_argument("Pipeline graph contains a null node");
    }
    auto projected = EditorNodeGraphProjection::ProjectNode(*node);
    auto json      = node->ToJson();
    draft.node_json_[projected.node_id]      = json;
    draft.base_node_json_[projected.node_id] = std::move(json);
    draft.base_node_index_[projected.node_id] = index;
    draft.nodes_.push_back(std::move(projected));
  }
  draft.edges_.reserve(graph.Edges().size());
  draft.base_edges_.reserve(graph.Edges().size());
  for (std::size_t index = 0; index < graph.Edges().size(); ++index) {
    const auto& edge  = graph.Edges()[index];
    const auto  scene = PipelineSceneEdge{edge.from_node, edge.from_port, edge.to_node, edge.to_port};
    draft.base_edges_.push_back(scene);
    draft.base_edge_index_[EdgeKey(scene)] = index;
    draft.edges_.push_back(ToProjection(scene));
  }
  draft.RebuildIndexes();
  draft.submission_valid_ = draft.ComputeSubmissionValid();
  return draft;
}

void EditorNodeGraphDraft::RebuildIndexes() {
  ++work_stats_.complete_index_rebuilds;
  node_index_.clear();
  outgoing_.clear();
  incoming_.clear();
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    const auto& id         = nodes_[index].node_id;
    node_index_[id]        = index;
    outgoing_[id]          = std::nullopt;
    incoming_[id]          = std::nullopt;
    work_stats_.index_entry_updates += 3;
  }
  for (const auto& edge : edges_) {
    const auto key                 = EdgeKey(edge);
    outgoing_[edge.source_node_id] = key;
    incoming_[edge.destination_node_id] = key;
    work_stats_.index_entry_updates += 2;
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

auto EditorNodeGraphDraft::FindEdgeIndex(const std::string& key) const
    -> std::optional<std::size_t> {
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    if (EdgeKey(edges_[index]) == key) {
      return index;
    }
  }
  return std::nullopt;
}

auto EditorNodeGraphDraft::FindEdgeByKey(const std::string& key) const
    -> const EditorNodeEdgeProjection* {
  const auto index = FindEdgeIndex(key);
  if (!index.has_value()) {
    return nullptr;
  }
  return &edges_[*index];
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

void EditorNodeGraphDraft::BeginReversal() {
  reversal_                        = {};
  reversal_.valid                  = true;
  reversal_.prior_next_name_number = draft_next_name_number_;
  reversal_.prior_submission_valid = submission_valid_;
}

void EditorNodeGraphDraft::InsertNodeAt(std::size_t index, EditorNodeProjection node) {
  const auto id = node.node_id;
  nodes_.insert(nodes_.begin() + static_cast<std::ptrdiff_t>(index), std::move(node));
  ++work_stats_.node_entry_copies;
  for (auto& [nid, idx] : node_index_) {
    if (idx >= index) {
      ++idx;
      ++work_stats_.index_entry_updates;
    }
  }
  node_index_[id] = index;
  outgoing_[id]   = std::nullopt;
  incoming_[id]   = std::nullopt;
  work_stats_.index_entry_updates += 3;
}

void EditorNodeGraphDraft::EraseNodeAt(std::size_t index) {
  const auto id = nodes_[index].node_id;
  nodes_.erase(nodes_.begin() + static_cast<std::ptrdiff_t>(index));
  node_index_.erase(id);
  outgoing_.erase(id);
  incoming_.erase(id);
  for (auto& [nid, idx] : node_index_) {
    if (idx > index) {
      --idx;
      ++work_stats_.index_entry_updates;
    }
  }
}

void EditorNodeGraphDraft::InsertEdge(EditorNodeEdgeProjection edge) {
  InsertEdgeAt(edges_.size(), std::move(edge));
}

void EditorNodeGraphDraft::InsertEdgeAt(std::size_t index, EditorNodeEdgeProjection edge) {
  const auto key = EdgeKey(edge);
  outgoing_[edge.source_node_id]        = key;
  incoming_[edge.destination_node_id]   = key;
  work_stats_.index_entry_updates += 2;
  ++work_stats_.edge_entry_copies;
  edges_.insert(edges_.begin() + static_cast<std::ptrdiff_t>(index), std::move(edge));
}

auto EditorNodeGraphDraft::RemoveEdgeByKey(const std::string& key) -> EditorNodeEdgeProjection {
  const auto index = FindEdgeIndex(key);
  if (!index.has_value()) {
    throw std::logic_error("Draft edge key is missing: " + key);
  }
  auto edge = edges_[*index];
  outgoing_[edge.source_node_id]      = std::nullopt;
  incoming_[edge.destination_node_id] = std::nullopt;
  work_stats_.index_entry_updates += 2;
  ++work_stats_.edge_entry_copies;
  edges_.erase(edges_.begin() + static_cast<std::ptrdiff_t>(*index));
  return edge;
}

void EditorNodeGraphDraft::DisconnectDraftKeys(const std::vector<std::string>& keys,
                                               EditorNodeGraphDraftMutation*   mutation) {
  struct Pending {
    std::string key;
    std::size_t index = 0;
  };
  std::vector<Pending> pending;
  pending.reserve(keys.size());
  for (const auto& key : keys) {
    const auto index = FindEdgeIndex(key);
    if (!index.has_value()) {
      continue;
    }
    pending.push_back(Pending{key, *index});
  }
  std::sort(pending.begin(), pending.end(),
            [](const Pending& lhs, const Pending& rhs) { return lhs.index > rhs.index; });
  for (const auto& item : pending) {
    auto edge = RemoveEdgeByKey(item.key);
    if (mutation != nullptr) {
      mutation->removed_edges.push_back(edge);
    }
    reversal_.removed_edge_indexes.push_back(item.index);
    reversal_.removed_edges.push_back(edge);
    Remember(&reversal_.prior_connected, connected_edge_, item.key);
    Remember(&reversal_.prior_disconnected, disconnected_, item.key);
    const auto connected = connected_edge_.find(item.key);
    if (connected != connected_edge_.end()) {
      connected_edge_.erase(connected);
    } else if (base_edge_index_.contains(item.key)) {
      disconnected_[item.key] =
          DisconnectedEdgeDelta{base_edge_index_.at(item.key), ToSceneEdge(edge)};
    }
  }
}

void EditorNodeGraphDraft::RestoreLastMutation() {
  if (!reversal_.valid) {
    return;
  }
  for (auto it = reversal_.added_edge_keys.rbegin(); it != reversal_.added_edge_keys.rend(); ++it) {
    (void)RemoveEdgeByKey(*it);
  }
  for (auto it = reversal_.added_node_ids.rbegin(); it != reversal_.added_node_ids.rend(); ++it) {
    const auto index = node_index_.at(*it);
    EraseNodeAt(index);
  }

  std::vector<std::size_t> node_order(reversal_.removed_node_indexes.size());
  for (std::size_t i = 0; i < node_order.size(); ++i) {
    node_order[i] = i;
  }
  std::sort(node_order.begin(), node_order.end(), [&](std::size_t lhs, std::size_t rhs) {
    return reversal_.removed_node_indexes[lhs] < reversal_.removed_node_indexes[rhs];
  });
  for (const auto i : node_order) {
    InsertNodeAt(reversal_.removed_node_indexes[i], reversal_.removed_nodes[i]);
  }

  std::vector<std::size_t> edge_order(reversal_.removed_edge_indexes.size());
  for (std::size_t i = 0; i < edge_order.size(); ++i) {
    edge_order[i] = i;
  }
  std::sort(edge_order.begin(), edge_order.end(), [&](std::size_t lhs, std::size_t rhs) {
    return reversal_.removed_edge_indexes[lhs] < reversal_.removed_edge_indexes[rhs];
  });
  for (const auto i : edge_order) {
    InsertEdgeAt(reversal_.removed_edge_indexes[i], reversal_.removed_edges[i]);
  }

  RestoreMap(&node_json_, reversal_.prior_node_json);
  RestoreMap(&inserted_json_, reversal_.prior_inserted_json);
  RestoreMap(&removed_, reversal_.prior_removed);
  RestoreMap(&disconnected_, reversal_.prior_disconnected);
  RestoreMap(&connected_edge_, reversal_.prior_connected);
  draft_next_name_number_ = reversal_.prior_next_name_number;
  submission_valid_       = reversal_.prior_submission_valid;
  reversal_               = {};
}

auto EditorNodeGraphDraft::DeltaEmpty() const -> bool {
  return inserted_json_.empty() && removed_.empty() && disconnected_.empty() &&
         connected_edge_.empty() && draft_next_name_number_ == base_next_name_number_;
}

auto EditorNodeGraphDraft::WouldCreateCycle(const NodeId& source_id, const NodeId& destination_id,
                                            const std::optional<std::string>& skip_outgoing,
                                            const std::optional<std::string>& skip_incoming) const
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
    const auto& edge_key = *out->second;
    if (skip_outgoing.has_value() && edge_key == *skip_outgoing) {
      continue;
    }
    if (skip_incoming.has_value() && edge_key == *skip_incoming) {
      continue;
    }
    const auto* edge = FindEdgeByKey(edge_key);
    if (edge == nullptr) {
      continue;
    }
    stack.push_back(edge->destination_node_id);
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
    return fail("Unsupported destination node type: " +
                std::string{destination->node_id.Value()});
  }
  const auto skip_out = outgoing_.at(source_id);
  const auto skip_in  = incoming_.at(destination_id);
  if (WouldCreateCycle(source_id, destination_id, skip_out, skip_in)) {
    return fail("That connection would create a cycle");
  }
  return true;
}

auto EditorNodeGraphDraft::FinishMutation(EditorNodeGraphDraftMutation mutation)
    -> EditorNodeGraphDraftMutation {
  mutation.delta_empty      = DeltaEmpty();
  mutation.submission_valid = ComputeSubmissionValid();
  submission_valid_         = mutation.submission_valid;
  mutation.succeeded        = true;
  return mutation;
}

auto EditorNodeGraphDraft::AddColorGrade(NodeId node_id) -> EditorNodeGraphDraftMutation {
  EditorNodeGraphDraftMutation result;
  if (node_id.Empty() || FindNode(node_id) != nullptr) {
    result.error = "A Color Grade with that identity already exists";
    return result;
  }
  BeginReversal();
  auto model = CreateCleanColorGradeNode(node_id);
  model->SetDisplayName(DefaultColorGradeDisplayName(draft_next_name_number_));
  auto json      = model->ToJson();
  auto projected = EditorNodeGraphProjection::ProjectNode(*model);
  Remember(&reversal_.prior_node_json, node_json_, node_id);
  Remember(&reversal_.prior_inserted_json, inserted_json_, node_id);
  reversal_.added_node_ids.push_back(node_id);
  InsertNodeAt(nodes_.size(), projected);
  node_json_[node_id]     = json;
  inserted_json_[node_id] = json;
  work_stats_.json_entry_copies += 2;
  ++draft_next_name_number_;
  result.inserted_nodes.push_back(std::move(projected));
  return FinishMutation(std::move(result));
}

auto EditorNodeGraphDraft::RemoveColorGrade(const NodeId& node_id)
    -> EditorNodeGraphDraftMutation {
  EditorNodeGraphDraftMutation result;
  const auto*                  node = FindNode(node_id);
  if (node == nullptr) {
    result.error = "That node is not in the current graph";
    return result;
  }
  if (node->node_kind != EditorNodeKind::ColorGrade) {
    result.error = "Only a Color Grade can be deleted";
    return result;
  }
  BeginReversal();
  const auto out = outgoing_.at(node_id);
  const auto in  = incoming_.at(node_id);
  std::vector<std::string> remove_keys;
  if (out.has_value()) {
    remove_keys.push_back(*out);
  }
  if (in.has_value() && (!out.has_value() || *in != *out)) {
    remove_keys.push_back(*in);
  }
  DisconnectDraftKeys(remove_keys, &result);

  const auto node_pos = node_index_.at(node_id);
  const auto json     = node_json_.at(node_id);
  reversal_.removed_node_indexes.push_back(node_pos);
  reversal_.removed_nodes.push_back(*node);
  Remember(&reversal_.prior_node_json, node_json_, node_id);
  Remember(&reversal_.prior_inserted_json, inserted_json_, node_id);
  Remember(&reversal_.prior_removed, removed_, node_id);
  EraseNodeAt(node_pos);
  node_json_.erase(node_id);
  result.removed_node_ids.push_back(node_id);
  const auto inserted = inserted_json_.find(node_id);
  if (inserted != inserted_json_.end()) {
    inserted_json_.erase(inserted);
    draft_next_name_number_ =
        inserted_json_.empty() ? base_next_name_number_
                               : MaxInsertedNameNumber(inserted_json_, base_next_name_number_);
  } else {
    removed_[node_id] = RemovedNodeDelta{base_node_index_.at(node_id), json};
    ++work_stats_.json_entry_copies;
  }
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
  if (skip_out.has_value()) {
    const auto* current = FindEdgeByKey(*skip_out);
    if (current != nullptr && current->destination_node_id == destination_id &&
        current->source_node_id == source_id) {
      result.no_op            = true;
      result.succeeded        = true;
      result.delta_empty      = DeltaEmpty();
      result.submission_valid = submission_valid_;
      return result;
    }
  }

  BeginReversal();
  std::vector<std::string> remove_keys;
  if (skip_out.has_value()) {
    remove_keys.push_back(*skip_out);
  }
  if (skip_in.has_value() && (!skip_out.has_value() || *skip_in != *skip_out)) {
    remove_keys.push_back(*skip_in);
  }
  DisconnectDraftKeys(remove_keys, &result);

  const auto key = EdgeKey(proposed);
  Remember(&reversal_.prior_connected, connected_edge_, key);
  Remember(&reversal_.prior_disconnected, disconnected_, key);
  InsertEdge(proposed);
  reversal_.added_edge_keys.push_back(key);
  result.inserted_edges.push_back(proposed);
  const auto disconnected = disconnected_.find(key);
  if (disconnected != disconnected_.end()) {
    disconnected_.erase(disconnected);
  } else {
    connected_edge_[key] = ToSceneEdge(proposed);
  }
  return FinishMutation(std::move(result));
}

auto EditorNodeGraphDraft::ComputeSubmissionValid() -> bool {
  ++work_stats_.validity_traversals;
  int                             develop_count = 0;
  int                             drt_count     = 0;
  const EditorNodeProjection*     develop       = nullptr;
  const EditorNodeProjection*     drt           = nullptr;
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
    const auto* edge = FindEdgeByKey(*out->second);
    if (edge == nullptr) {
      return false;
    }
    current = edge->destination_node_id;
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
  std::map<NodeId, std::uint32_t> node_order;
  for (std::size_t index = 0; index < nodes_.size(); ++index) {
    node_order[nodes_[index].node_id] = static_cast<std::uint32_t>(index);
  }
  std::map<std::string, std::uint32_t> edge_order;
  for (std::size_t index = 0; index < edges_.size(); ++index) {
    edge_order[EdgeKey(edges_[index])] = static_cast<std::uint32_t>(index);
  }
  for (const auto& [id, json] : inserted_json_) {
    NodeGraphInsertedNode item;
    item.node             = json;
    item.final_node_index = node_order.at(id);
    change.inserted_nodes.push_back(std::move(item));
  }
  for (const auto& [id, record] : removed_) {
    NodeGraphRemovedNode item;
    item.node                = record.json;
    item.original_node_index = static_cast<std::uint32_t>(record.original_index);
    change.removed_nodes.push_back(std::move(item));
  }
  for (const auto& [key, record] : disconnected_) {
    NodeGraphDisconnectedEdge item;
    item.edge                = record.edge;
    item.original_edge_index = static_cast<std::uint32_t>(record.original_index);
    change.disconnected_edges.push_back(std::move(item));
  }
  for (const auto& [key, edge] : connected_edge_) {
    NodeGraphConnectedEdge item;
    item.edge             = edge;
    item.final_edge_index = edge_order.at(key);
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
