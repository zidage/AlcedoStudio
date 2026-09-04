//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "edit/mask/mask_id.hpp"
#include "json.hpp"

namespace alcedo {

/// User-action kind recorded on one batch. Distinct from the typed change list.
enum class PipelineEditOperationKind : std::uint8_t {
  SetParameter = 0,
  SetNodeEnabled,
  SetNodeMix,
  RenameColorGrade,
  AddColorGrade,
  RemoveColorGrade,
  ReconnectColorGrade,
  AddMask,
  RemoveMask,
  ReplaceMaskSource,
  ReplaceMaskAsset,
  SetMaskField,
  Paste,
  EditNodeGraph,
};

/// Discriminator for one stored typed change.
enum class PipelineEditChangeKind : std::uint8_t {
  SetParameter = 0,
  SetNodeEnabled,
  SetNodeMix,
  RenameColorGrade,
  AddColorGrade,
  RemoveColorGrade,
  ReconnectColorGrade,
  AddMask,
  RemoveMask,
  ReplaceMaskSource,
  ReplaceMaskAsset,
  SetMaskField,
  NodeGraphTopologyChange,
};

/// Owner of one stored parameter write. Unspecified is not a legal stored value.
enum class PipelineParameterOwnerKind : std::uint8_t {
  Document = 0,
  Develop,
  ColorGrade,
  ColorGradeMask,
  DrtPost,
};

/// Graph node kind required by enabled-state changes.
enum class PipelineEditNodeKind : std::uint8_t {
  Develop = 0,
  ColorGrade,
  Drt,
};

/// Apply direction for stored before/after data. Does not mutate a document.
enum class PipelineEditApplyDirection : std::uint8_t {
  Forward = 0,
  Inverse = 1,
};

/**
 * @brief Complete identity of one parameter write.
 *
 * Replay uses these stored IDs. It must not infer an owner from @p field_key or
 * from a live selection.
 *
 * Unused identity fields are empty and serialize as JSON null.
 */
struct PipelineParameterTarget {
  PipelineParameterOwnerKind owner_kind = PipelineParameterOwnerKind::Document;
  NodeId                     node_id;
  AdjustmentInstanceId       adjustment_instance_id;
  MaskId                     mask_id;
  std::string                field_key;

  auto operator==(const PipelineParameterTarget&) const -> bool = default;
};

/**
 * @brief Exact scene-image edge recorded with a graph change.
 *
 * Port values are stored explicitly. Replay must not invent a port from node kind.
 */
struct PipelineSceneEdge {
  NodeId from_node;
  PortId from_port;
  NodeId to_node;
  PortId to_port;

  auto operator==(const PipelineSceneEdge&) const -> bool = default;
};

struct SetParameterChange {
  PipelineParameterTarget target;
  nlohmann::json          before_value   = nlohmann::json::object();
  nlohmann::json          after_value    = nlohmann::json::object();
  bool                    before_enabled = false;
  bool                    after_enabled  = true;

  auto operator==(const SetParameterChange&) const -> bool = default;
};

struct SetNodeEnabledChange {
  NodeId               node_id;
  PipelineEditNodeKind node_kind      = PipelineEditNodeKind::ColorGrade;
  bool                 before_enabled = true;
  bool                 after_enabled  = true;

  auto operator==(const SetNodeEnabledChange&) const -> bool = default;
};

struct SetNodeMixChange {
  NodeId node_id;
  float  before_mix = 1.0f;
  float  after_mix  = 1.0f;

  auto operator==(const SetNodeMixChange&) const -> bool = default;
};

struct RenameColorGradeChange {
  NodeId      node_id;
  std::string before_display_name;
  std::string after_display_name;

  auto operator==(const RenameColorGradeChange&) const -> bool = default;
};

struct AddColorGradeChange {
  NodeId             node_id;
  nlohmann::json     node = nlohmann::json::object();
  NodeId             predecessor_id;
  NodeId             successor_id;
  PipelineSceneEdge  incoming_edge;
  PipelineSceneEdge  outgoing_edge;
  // Stored-node insertion uses equal values because it restores an existing display name.
  std::uint64_t      before_next_color_grade_name_number = kInitialNextColorGradeNameNumber;
  std::uint64_t      after_next_color_grade_name_number  = kInitialNextColorGradeNameNumber;

  auto operator==(const AddColorGradeChange&) const -> bool = default;
};

struct RemoveColorGradeChange {
  NodeId             node_id;
  nlohmann::json     node = nlohmann::json::object();
  NodeId             predecessor_id;
  NodeId             successor_id;
  PipelineSceneEdge  removed_incoming_edge;
  PipelineSceneEdge  removed_outgoing_edge;
  PipelineSceneEdge  bridge_edge;

  auto operator==(const RemoveColorGradeChange&) const -> bool = default;
};

struct ReconnectColorGradeChange {
  NodeId            node_id;
  NodeId            before_predecessor_id;
  NodeId            before_successor_id;
  NodeId            after_predecessor_id;
  NodeId            after_successor_id;
  PipelineSceneEdge before_incoming_edge;
  PipelineSceneEdge before_outgoing_edge;
  PipelineSceneEdge after_incoming_edge;
  PipelineSceneEdge after_outgoing_edge;

  auto operator==(const ReconnectColorGradeChange&) const -> bool = default;
};

/// One Color Grade inserted by a net topology delta.
struct NodeGraphInsertedNode {
  nlohmann::json node              = nlohmann::json::object();
  std::uint32_t  final_node_index = 0;

  auto operator==(const NodeGraphInsertedNode&) const -> bool = default;
};

/// One Color Grade removed by a net topology delta.
struct NodeGraphRemovedNode {
  nlohmann::json node                 = nlohmann::json::object();
  std::uint32_t  original_node_index = 0;

  auto operator==(const NodeGraphRemovedNode&) const -> bool = default;
};

/// One scene-image edge disconnected from the bound base graph.
struct NodeGraphDisconnectedEdge {
  PipelineSceneEdge edge;
  std::uint32_t     original_edge_index = 0;

  auto operator==(const NodeGraphDisconnectedEdge&) const -> bool = default;
};

/// One scene-image edge present in the final graph.
struct NodeGraphConnectedEdge {
  PipelineSceneEdge edge;
  std::uint32_t     final_edge_index = 0;

  auto operator==(const NodeGraphConnectedEdge&) const -> bool = default;
};

/**
 * @brief Net topology delta from one bound base graph to one accepted graph.
 *
 * One stored change applies in place. It is not a list of Add, Remove, or
 * Reconnect commands. Transient session generation and topology revision
 * guards are not replay data and are omitted.
 */
struct NodeGraphTopologyChange {
  std::vector<NodeGraphInsertedNode>     inserted_nodes;
  std::vector<NodeGraphRemovedNode>      removed_nodes;
  std::vector<NodeGraphDisconnectedEdge> disconnected_edges;
  std::vector<NodeGraphConnectedEdge>    connected_edges;
  std::uint64_t before_next_color_grade_name_number = kInitialNextColorGradeNameNumber;
  std::uint64_t after_next_color_grade_name_number  = kInitialNextColorGradeNameNumber;

  auto operator==(const NodeGraphTopologyChange&) const -> bool = default;
};

struct AddMaskChange {
  NodeId         node_id;
  MaskId         mask_id;
  nlohmann::json mask = nlohmann::json::object();
  std::uint32_t  display_index = 0;

  auto operator==(const AddMaskChange&) const -> bool = default;
};

struct RemoveMaskChange {
  NodeId         node_id;
  MaskId         mask_id;
  nlohmann::json mask = nlohmann::json::object();
  std::uint32_t  display_index = 0;

  auto operator==(const RemoveMaskChange&) const -> bool = default;
};

struct ReplaceMaskSourceChange {
  NodeId         node_id;
  MaskId         mask_id;
  nlohmann::json before_source = nlohmann::json::object();
  nlohmann::json after_source  = nlohmann::json::object();

  auto operator==(const ReplaceMaskSourceChange&) const -> bool = default;
};

struct ReplaceMaskAssetChange {
  NodeId         node_id;
  MaskId         mask_id;
  nlohmann::json before_source = nlohmann::json::object();
  nlohmann::json after_source  = nlohmann::json::object();

  auto operator==(const ReplaceMaskAssetChange&) const -> bool = default;
};

struct SetMaskFieldChange {
  NodeId         node_id;
  MaskId         mask_id;
  std::string    field_key;
  nlohmann::json before_value = nullptr;
  nlohmann::json after_value  = nullptr;

  auto operator==(const SetMaskFieldChange&) const -> bool = default;
};

using PipelineEditChange =
    std::variant<SetParameterChange, SetNodeEnabledChange, SetNodeMixChange, RenameColorGradeChange,
                 AddColorGradeChange, RemoveColorGradeChange, ReconnectColorGradeChange,
                 AddMaskChange, RemoveMaskChange, ReplaceMaskSourceChange, ReplaceMaskAssetChange,
                 SetMaskFieldChange, NodeGraphTopologyChange>;

/**
 * @brief Saved identity used by history rows. Never reads a live document.
 *
 * Display names and localization arguments come from the batch. Deleted nodes
 * remain presentable from this data.
 */
struct PipelineEditHistoryProjection {
  PipelineEditOperationKind operation_kind = PipelineEditOperationKind::SetParameter;
  std::string               presentation_key;
  nlohmann::json            presentation_args = nlohmann::json::object();
  std::string               node_id;
  std::string               node_display_name;
  std::string               adjustment_instance_id;
  std::string               mask_id;
  std::string               mask_display_name;
  std::string               field_key;
  nlohmann::json            before_display_value = nullptr;
  nlohmann::json            after_display_value  = nullptr;
};

/**
 * @brief One user action with ordered typed changes and presentation metadata.
 *
 * The batch is the commit payload. It does not apply itself to a live document.
 *
 * @pre @ref Validate succeeds before hashing or persistence.
 */
struct PipelineEditBatch {
  std::uint32_t                        batch_format_version = kPipelineEditBatchFormatVersion;
  PipelineEditOperationKind            operation_kind = PipelineEditOperationKind::SetParameter;
  std::vector<PipelineEditChange>      changes;
  std::string                          presentation_key;
  nlohmann::json                       presentation_args = nlohmann::json::object();

  /**
   * @brief Build a validated batch.
   *
   * @param operation_kind User-action kind. Must be compatible with @p changes.
   * @param changes Ordered typed changes. Must be non-empty.
   * @param presentation_key Stable localization key. Must be non-empty.
   * @param presentation_args Localization arguments. Must be a JSON object.
   * @throws std::runtime_error when validation fails.
   */
  static auto Make(PipelineEditOperationKind operation_kind, std::vector<PipelineEditChange> changes,
                   std::string presentation_key,
                   nlohmann::json presentation_args = nlohmann::json::object()) -> PipelineEditBatch;

  /**
   * @brief Reject empty batches, incompatible change kinds, incomplete identity,
   *        non-canonical nested JSON, and non-finite numbers.
   *
   * @throws std::runtime_error on the first failed rule. Does not mutate the batch.
   */
  void Validate() const;

  [[nodiscard]] auto CanonicalJSON() const -> nlohmann::json;
  [[nodiscard]] auto ToJSON() const -> nlohmann::json { return CanonicalJSON(); }

  /**
   * @brief Parse and require canonical dump equality with @ref CanonicalJSON.
   *
  * Unknown keys, unknown enums, duplicate-incompatible objects, and non-batch
   * payloads are rejected.
   *
   * @throws std::runtime_error when @p json is not a canonical typed batch.
   */
  static auto FromJSON(const nlohmann::json& json) -> PipelineEditBatch;
};

/**
 * @brief True when @p json is an object that carries @c batch_format_version.
 *
 * Used to distinguish typed batches from non-batch commit payloads.
 */
[[nodiscard]] auto IsPipelineEditBatchJson(const nlohmann::json& json) -> bool;

[[nodiscard]] auto PipelineEditOperationKindText(PipelineEditOperationKind kind) -> std::string_view;
[[nodiscard]] auto PipelineEditOperationKindFromText(std::string_view text)
    -> PipelineEditOperationKind;
[[nodiscard]] auto PipelineEditChangeKindText(PipelineEditChangeKind kind) -> std::string_view;
[[nodiscard]] auto PipelineEditChangeKindOf(const PipelineEditChange& change)
    -> PipelineEditChangeKind;

/**
 * @brief Changes in stored order for forward apply, or reversed for inverse apply.
 *
 * Does not mutate a document and does not copy nested JSON beyond the change
 * values already stored on the batch.
 */
[[nodiscard]] auto OrderedChangesForApply(const PipelineEditBatch& batch,
                                          PipelineEditApplyDirection direction)
    -> std::vector<PipelineEditChange>;

/**
 * @brief History-row identity taken from saved batch data only.
 *
 * @param batch Validated batch. Node display names come from presentation
 *        arguments or nested saved node/mask JSON, never from a live graph.
 */
[[nodiscard]] auto ProjectPipelineEditHistory(const PipelineEditBatch& batch)
    -> PipelineEditHistoryProjection;

}  // namespace alcedo
