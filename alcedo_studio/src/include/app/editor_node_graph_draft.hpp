//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "app/editor_node_graph_projection.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_edit_batch.hpp"

namespace alcedo {

/// Bound product identity copied when a node-graph draft is created.
struct EditorNodeGraphDraftIdentity {
  std::uint64_t element_id          = 0;
  std::uint64_t image_id            = 0;
  std::string   version_id;
  std::uint64_t session_generation  = 0;
  std::uint64_t projection_revision = 0;
  std::uint64_t topology_revision   = 0;

  auto operator==(const EditorNodeGraphDraftIdentity&) const -> bool = default;
};

/// Incremental Qan operations produced by one admitted draft mutation.
struct EditorNodeGraphDraftMutation {
  bool                                  succeeded        = false;
  bool                                  no_op            = false;
  bool                                  submission_valid = false;
  bool                                  delta_empty      = false;
  std::string                           error;
  std::vector<EditorNodeProjection>     inserted_nodes;
  std::vector<NodeId>                   removed_node_ids;
  std::vector<EditorNodeEdgeProjection> removed_edges;
  std::vector<EditorNodeEdgeProjection> inserted_edges;
};

/**
 * @brief Copy and index work recorded by one draft object.
 *
 * Tests reset these counters after construction. Validity traversal is counted
 * separately from mutation copies. A whole-draft checkpoint copies every node,
 * edge, JSON map, index, and net-delta map in one step.
 */
struct EditorNodeGraphDraftWorkStats {
  int complete_state_copies   = 0;
  int complete_index_rebuilds = 0;
  int node_entry_copies       = 0;
  int edge_entry_copies       = 0;
  int json_entry_copies       = 0;
  int index_entry_updates     = 0;
  int validity_traversals     = 0;
};

/**
 * @brief Incremental Nodes-page topology draft. Not product data and not a render input.
 *
 * Construction copies the live document once. Later Add, Delete, and Connect
 * mutate this object in place. A rejected operation leaves values, indexes,
 * cached submission validity, and the accumulated delta unchanged. An admitted
 * operation stores a bounded reversal record of affected entries only.
 *
 * Threading: GUI thread only. Ownership: value type, no QObject or Qan pointers.
 */
class EditorNodeGraphDraft {
 public:
  /**
   * @brief Copy the complete live document into a new draft bound to @p identity.
   * @pre @p document is the product graph for @p identity.
   */
  static auto FromDocument(const PipelineDocument& document, EditorNodeGraphDraftIdentity identity)
      -> EditorNodeGraphDraft;

  [[nodiscard]] auto identity() const -> const EditorNodeGraphDraftIdentity& { return identity_; }
  [[nodiscard]] auto MatchesIdentity(const EditorNodeGraphDraftIdentity& identity) const -> bool {
    return identity_ == identity;
  }

  /**
   * @brief True when the live graph still matches the bound base topology.
   */
  [[nodiscard]] auto MatchesBase(const PipelineDocument& document) const -> bool;

  /**
   * @brief Insert one disconnected clean Color Grade. Does not create an edge.
   */
  auto AddColorGrade(NodeId node_id) -> EditorNodeGraphDraftMutation;

  /**
   * @brief Remove one Color Grade and its incident edges. Does not bridge neighbors.
   */
  auto RemoveColorGrade(const NodeId& node_id) -> EditorNodeGraphDraftMutation;

  /**
   * @brief Exclusive-port connect from @p source_id image output to @p destination_id image input.
   *
   * Replaces the current outgoing edge of the source and the current incoming
   * edge of the destination. Does not infer a backbone move.
   */
  auto Connect(const NodeId& source_id, const NodeId& destination_id)
      -> EditorNodeGraphDraftMutation;

  /**
   * @brief Restore the exact state captured before the last admitted mutation.
   *
   * No-op when HasLastMutation is false. After restore there is no further
   * reversal until another mutation is admitted.
   */
  void RestoreLastMutation();

  [[nodiscard]] auto HasLastMutation() const -> bool { return reversal_.valid; }
  [[nodiscard]] auto DeltaEmpty() const -> bool;
  /**
   * @brief Cached result of the last admitted mutation or construction.
   *
   * Does not walk the graph. Invalid requests leave this value unchanged.
   */
  [[nodiscard]] auto SubmissionValid() const -> bool { return submission_valid_; }
  [[nodiscard]] auto NextColorGradeNameNumber() const -> std::uint64_t {
    return draft_next_name_number_;
  }
  [[nodiscard]] auto BaseNextColorGradeNameNumber() const -> std::uint64_t {
    return base_next_name_number_;
  }
  [[nodiscard]] auto work_stats() const -> EditorNodeGraphDraftWorkStats { return work_stats_; }
  void               ResetWorkStats() { work_stats_ = {}; }

  /**
   * @brief Build the stored net delta from current indexes. Empty when DeltaEmpty.
   *
   * Materializes ordered serialized node and edge indexes with a full traversal.
   */
  [[nodiscard]] auto MakeChange() const -> NodeGraphTopologyChange;

  [[nodiscard]] auto Nodes() const -> const std::vector<EditorNodeProjection>& { return nodes_; }
  [[nodiscard]] auto Edges() const -> const std::vector<EditorNodeEdgeProjection>& { return edges_; }
  [[nodiscard]] auto FindNode(const NodeId& node_id) const -> const EditorNodeProjection*;
  [[nodiscard]] auto NodeJson(const NodeId& node_id) const -> const nlohmann::json*;

  /**
   * @brief Value snapshot of the current draft graph, including detached nodes.
   *
   * Copies the live node and edge vectors. Call only at an explicit projection
   * boundary such as page recreation.
   */
  [[nodiscard]] auto CurrentSnapshot(std::uint64_t session_generation,
                                     std::uint64_t projection_revision,
                                     std::uint64_t topology_revision) const
      -> EditorNodeGraphSnapshot;

 private:
  struct RemovedNodeDelta {
    std::size_t    original_index = 0;
    nlohmann::json json;
  };

  struct DisconnectedEdgeDelta {
    std::size_t        original_index = 0;
    PipelineSceneEdge  edge;
  };

  struct ReversalRecord {
    bool            valid = false;
    std::uint64_t   prior_next_name_number = 0;
    bool            prior_submission_valid = false;

    std::vector<NodeId>                   added_node_ids;
    std::vector<std::string>              added_edge_keys;
    std::vector<std::size_t>              removed_node_indexes;
    std::vector<EditorNodeProjection>     removed_nodes;
    std::vector<std::size_t>              removed_edge_indexes;
    std::vector<EditorNodeEdgeProjection> removed_edges;

    std::map<NodeId, std::optional<nlohmann::json>>      prior_node_json;
    std::map<NodeId, std::optional<nlohmann::json>>      prior_inserted_json;
    std::map<NodeId, std::optional<RemovedNodeDelta>>    prior_removed;
    std::map<std::string, std::optional<DisconnectedEdgeDelta>> prior_disconnected;
    std::map<std::string, std::optional<PipelineSceneEdge>>    prior_connected;
  };

  void BeginReversal();
  void RebuildIndexes();
  auto AdmitConnect(const NodeId& source_id, const NodeId& destination_id, std::string* error) const
      -> bool;
  auto WouldCreateCycle(const NodeId& source_id, const NodeId& destination_id,
                        const std::optional<std::string>& skip_outgoing,
                        const std::optional<std::string>& skip_incoming) const -> bool;
  auto RemoveEdgeByKey(const std::string& key) -> EditorNodeEdgeProjection;
  void InsertEdge(EditorNodeEdgeProjection edge);
  void InsertEdgeAt(std::size_t index, EditorNodeEdgeProjection edge);
  void InsertNodeAt(std::size_t index, EditorNodeProjection node);
  void EraseNodeAt(std::size_t index);
  void DisconnectDraftKeys(const std::vector<std::string>& keys,
                           EditorNodeGraphDraftMutation*   mutation);
  auto FinishMutation(EditorNodeGraphDraftMutation mutation) -> EditorNodeGraphDraftMutation;
  [[nodiscard]] auto ComputeSubmissionValid() -> bool;
  [[nodiscard]] auto FindEdgeByKey(const std::string& key) const -> const EditorNodeEdgeProjection*;
  [[nodiscard]] auto FindEdgeIndex(const std::string& key) const -> std::optional<std::size_t>;

  template <class Map, class Key>
  void Remember(std::map<Key, std::optional<typename Map::mapped_type>>* prior, const Map& current,
                const Key& key);
  template <class Map, class Key>
  static void RestoreMap(Map* current,
                         const std::map<Key, std::optional<typename Map::mapped_type>>& prior);

  [[nodiscard]] static auto ImagePort() -> PortId;
  [[nodiscard]] static auto EdgeKey(const EditorNodeEdgeProjection& edge) -> std::string;
  [[nodiscard]] static auto EdgeKey(const PipelineSceneEdge& edge) -> std::string;
  [[nodiscard]] static auto ToSceneEdge(const EditorNodeEdgeProjection& edge) -> PipelineSceneEdge;
  [[nodiscard]] static auto ToProjection(const PipelineSceneEdge& edge) -> EditorNodeEdgeProjection;

  EditorNodeGraphDraftIdentity          identity_{};
  std::vector<EditorNodeProjection>     nodes_;
  std::vector<EditorNodeEdgeProjection> edges_;
  std::map<NodeId, nlohmann::json>      node_json_;
  std::map<NodeId, std::size_t>         node_index_;
  std::map<NodeId, std::optional<std::string>> outgoing_;
  std::map<NodeId, std::optional<std::string>> incoming_;
  std::map<NodeId, std::size_t>         base_node_index_;
  std::map<std::string, std::size_t>    base_edge_index_;
  std::map<NodeId, nlohmann::json>      base_node_json_;
  std::vector<PipelineSceneEdge>        base_edges_;
  std::map<NodeId, nlohmann::json>      inserted_json_;
  std::map<NodeId, RemovedNodeDelta>    removed_;
  std::map<std::string, DisconnectedEdgeDelta> disconnected_;
  std::map<std::string, PipelineSceneEdge>     connected_edge_;
  std::uint64_t                         base_next_name_number_  = kInitialNextColorGradeNameNumber;
  std::uint64_t                         draft_next_name_number_ = kInitialNextColorGradeNameNumber;
  bool                                  submission_valid_       = false;
  ReversalRecord                        reversal_{};
  EditorNodeGraphDraftWorkStats         work_stats_{};
};

}  // namespace alcedo
