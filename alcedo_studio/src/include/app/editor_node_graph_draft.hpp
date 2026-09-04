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
  std::uint64_t element_id           = 0;
  std::uint64_t image_id             = 0;
  std::string   version_id;
  std::uint64_t session_generation   = 0;
  std::uint64_t projection_revision  = 0;
  std::uint64_t topology_revision    = 0;

  auto operator==(const EditorNodeGraphDraftIdentity&) const -> bool = default;
};

/// Incremental Qan operations produced by one admitted draft mutation.
struct EditorNodeGraphDraftMutation {
  bool                                  succeeded         = false;
  bool                                  no_op             = false;
  bool                                  submission_valid  = false;
  bool                                  delta_empty       = false;
  std::string                           error;
  std::vector<EditorNodeProjection>     inserted_nodes;
  std::vector<NodeId>                   removed_node_ids;
  std::vector<EditorNodeEdgeProjection> removed_edges;
  std::vector<EditorNodeEdgeProjection> inserted_edges;
};

/**
 * @brief Incremental Nodes-page topology draft. Not product data and not a render input.
 *
 * Construction copies the live document once. Later Add, Delete, and Connect
 * mutate this object in place. A rejected operation restores the exact prior
 * values, indexes, and accumulated delta.
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
   */
  void RestoreLastMutation();

  [[nodiscard]] auto HasLastMutation() const -> bool { return has_checkpoint_; }
  [[nodiscard]] auto DeltaEmpty() const -> bool;
  [[nodiscard]] auto SubmissionValid() const -> bool;
  [[nodiscard]] auto NextColorGradeNameNumber() const -> std::uint64_t {
    return draft_next_name_number_;
  }
  [[nodiscard]] auto BaseNextColorGradeNameNumber() const -> std::uint64_t {
    return base_next_name_number_;
  }

  /**
   * @brief Build the stored net delta from current indexes. Empty when DeltaEmpty.
   */
  [[nodiscard]] auto MakeChange() const -> NodeGraphTopologyChange;

  [[nodiscard]] auto Nodes() const -> const std::vector<EditorNodeProjection>& { return nodes_; }
  [[nodiscard]] auto Edges() const -> const std::vector<EditorNodeEdgeProjection>& { return edges_; }
  [[nodiscard]] auto FindNode(const NodeId& node_id) const -> const EditorNodeProjection*;
  [[nodiscard]] auto NodeJson(const NodeId& node_id) const -> const nlohmann::json*;

  /**
   * @brief Value snapshot of the current draft graph, including detached nodes.
   */
  [[nodiscard]] auto CurrentSnapshot(std::uint64_t session_generation,
                                     std::uint64_t projection_revision,
                                     std::uint64_t topology_revision) const
      -> EditorNodeGraphSnapshot;

 private:
  struct EdgeRecord {
    EditorNodeEdgeProjection edge;
  };

  struct Checkpoint {
    std::vector<EditorNodeProjection>     nodes;
    std::vector<EditorNodeEdgeProjection> edges;
    std::map<NodeId, nlohmann::json>      node_json;
    std::map<NodeId, std::size_t>         node_index;
    std::map<std::string, std::size_t>    edge_index;
    std::map<NodeId, std::optional<std::size_t>> outgoing;
    std::map<NodeId, std::optional<std::size_t>> incoming;
    std::map<NodeId, nlohmann::json>      inserted_json;
    std::map<NodeId, std::size_t>         removed_original_index;
    std::map<NodeId, nlohmann::json>      removed_json;
    std::map<std::string, std::size_t>    disconnected_original_index;
    std::map<std::string, PipelineSceneEdge> disconnected_edge;
    std::map<std::string, PipelineSceneEdge> connected_edge;
    std::uint64_t                         draft_next_name_number = 0;
  };

  void CaptureCheckpoint();
  void RebuildIndexes();
  auto AdmitConnect(const NodeId& source_id, const NodeId& destination_id, std::string* error) const
      -> bool;
  auto WouldCreateCycle(const NodeId& source_id, const NodeId& destination_id,
                        const std::optional<std::size_t>& skip_outgoing,
                        const std::optional<std::size_t>& skip_incoming) const -> bool;
  auto RemoveEdgeAt(std::size_t index) -> EditorNodeEdgeProjection;
  void InsertEdge(EditorNodeEdgeProjection edge);
  auto FinishMutation(EditorNodeGraphDraftMutation mutation) -> EditorNodeGraphDraftMutation;
  [[nodiscard]] static auto ImagePort() -> PortId;
  [[nodiscard]] static auto EdgeKey(const EditorNodeEdgeProjection& edge) -> std::string;
  [[nodiscard]] static auto EdgeKey(const PipelineSceneEdge& edge) -> std::string;
  [[nodiscard]] static auto ToSceneEdge(const EditorNodeEdgeProjection& edge) -> PipelineSceneEdge;
  [[nodiscard]] static auto ToProjection(const PipelineSceneEdge& edge) -> EditorNodeEdgeProjection;
  [[nodiscard]] static auto KindOf(const INodeModel& node) -> EditorNodeKind;
  [[nodiscard]] auto        ProjectNode(const INodeModel& node) const -> EditorNodeProjection;

  EditorNodeGraphDraftIdentity          identity_{};
  std::vector<EditorNodeProjection>     nodes_;
  std::vector<EditorNodeEdgeProjection> edges_;
  std::map<NodeId, nlohmann::json>      node_json_;
  std::map<NodeId, std::size_t>         node_index_;
  std::map<std::string, std::size_t>    edge_index_;
  std::map<NodeId, std::optional<std::size_t>> outgoing_;
  std::map<NodeId, std::optional<std::size_t>> incoming_;
  std::map<NodeId, std::size_t>         base_node_index_;
  std::map<std::string, std::size_t>    base_edge_index_;
  std::map<NodeId, nlohmann::json>      base_node_json_;
  std::vector<PipelineSceneEdge>        base_edges_;
  std::map<NodeId, nlohmann::json>      inserted_json_;
  std::map<NodeId, std::size_t>         removed_original_index_;
  std::map<NodeId, nlohmann::json>      removed_json_;
  std::map<std::string, std::size_t>    disconnected_original_index_;
  std::map<std::string, PipelineSceneEdge> disconnected_edge_;
  std::map<std::string, PipelineSceneEdge> connected_edge_;
  std::uint64_t                         base_next_name_number_  = kInitialNextColorGradeNameNumber;
  std::uint64_t                         draft_next_name_number_ = kInitialNextColorGradeNameNumber;
  Checkpoint                            checkpoint_{};
  bool                                  has_checkpoint_ = false;
};

}  // namespace alcedo
