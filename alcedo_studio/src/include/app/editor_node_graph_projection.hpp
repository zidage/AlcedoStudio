//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/i_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/mask/mask_model.hpp"

namespace alcedo {

/// Node categories exposed by the editor graph view.
enum class EditorNodeKind : std::uint8_t {
  Develop = 0,
  ColorGrade,
  Drt,
};

/**
 * @brief Immutable Mask identity shown inside a Color Grade node card.
 *
 * The editor graph view receives only identity and source category. Mask values
 * remain owned by the document and are read by the owning adjustment drawer.
 */
struct EditorNodeMaskProjection {
  MaskId         mask_id;
  MaskSourceKind source_kind                                               = MaskSourceKind::Radial;

  auto           operator==(const EditorNodeMaskProjection&) const -> bool = default;
};

/**
 * @brief Immutable node-card data projected from a PipelineDocument.
 */
struct EditorNodeProjection {
  NodeId                                node_id;
  EditorNodeKind                        node_kind = EditorNodeKind::ColorGrade;
  std::string                           display_name;
  bool                                  deletion_protected = false;
  std::vector<EditorNodeMaskProjection> masks;

  auto operator==(const EditorNodeProjection&) const -> bool = default;
};

/**
 * @brief Immutable scene-image edge data projected from a PipelineDocument.
 */
struct EditorNodeEdgeProjection {
  NodeId source_node_id;
  PortId source_port_id;
  NodeId destination_node_id;
  PortId destination_port_id;

  auto   operator==(const EditorNodeEdgeProjection&) const -> bool = default;
};

/**
 * @brief Complete value snapshot published across the editor boundary.
 *
 * No Qan objects or document-owned pointers are stored here. The session
 * value is stamped by the publisher and identifies the producing
 * image-load session.
 */
struct EditorNodeGraphSnapshot {
  std::uint64_t                         session_generation = 0;
  std::vector<EditorNodeProjection>     nodes;
  std::vector<EditorNodeEdgeProjection> edges;

  auto operator==(const EditorNodeGraphSnapshot&) const -> bool = default;
};

/**
 * @brief Immutable Mask sub-row projected inside one Mask Groups row.
 *
 * Rows are keyed by (NodeId, MaskId). Only the display fields the Mask Groups
 * panel needs are copied: source category, display name, enabled, and opacity.
 * Mask parameter values remain owned by the document and are edited through the
 * existing Mask page.
 */
struct EditorMaskGroupMaskRow {
  NodeId         node_id;
  MaskId         mask_id;
  MaskSourceKind source_kind = MaskSourceKind::Radial;
  std::string    display_name;
  bool           enabled                                                 = true;
  float          opacity                                                 = 1.0F;

  auto           operator==(const EditorMaskGroupMaskRow&) const -> bool = default;
};

/**
 * @brief Immutable Mask Groups row projected from one Color Grade backbone node.
 *
 * Group identity is the Color Grade NodeId; the display name matches the node
 * graph exactly. Every backbone Color Grade produces a row, including grades
 * with no Masks: an empty @ref masks list means "no attached Mask", never a
 * transparent or black input.
 */
struct EditorMaskGroupRow {
  NodeId                              node_id;
  std::string                         display_name;
  bool                                enabled            = true;
  bool                                deletion_protected = false;
  std::vector<EditorMaskGroupMaskRow> masks;

  auto                                operator==(const EditorMaskGroupRow&) const -> bool = default;
};

/**
 * @brief Complete Mask Groups value snapshot published across the editor boundary.
 *
 * Group order is the reverse of scene-image execution: the downstream Color
 * Grade nearest DRT/Post is first and the upstream Color Grade nearest Develop
 * is last. It is never derived from creation time, display name, or canvas
 * position. Develop and DRT/Post are endpoints and never produce group rows.
 */
struct EditorMaskGroupSnapshot {
  std::vector<EditorMaskGroupRow> groups;

  auto operator==(const EditorMaskGroupSnapshot&) const -> bool = default;
};

/**
 * @brief Build the immutable Nodes-page projection for one document state.
 *
 * Nodes follow the unique Develop-to-DRT image backbone. Color Grade Masks
 * follow their document display order. Parameter values, enabled state, mix,
 * and Mask presentation metadata are deliberately not part of this snapshot.
 */
class EditorNodeGraphProjection {
 public:
  /**
   * @brief Map a live node model to Develop, Color Grade, or DRT/Post.
   * @throws std::invalid_argument when @p node is not a supported Nodes-page type.
   */
  [[nodiscard]] static auto KindOf(const INodeModel& node) -> EditorNodeKind;

  /**
   * @brief Copy node identity, kind, display name, and Mask rows in stored order.
   *
   * Mask rows keep document display order. Parameter values, enabled state, mix,
   * and Mask presentation metadata are omitted. Detached Color Grades are valid
   * inputs; callers choose backbone-only or full-graph traversal.
   *
   * @throws std::invalid_argument when @p node is an unsupported type or a Color
   *         Grade whose model cannot be read.
   */
  [[nodiscard]] static auto ProjectNode(const INodeModel& node) -> EditorNodeProjection;

  /**
   * @param document Valid PipelineDocument whose image backbone is projected.
   * @param session_generation Session value copied into the snapshot.
   * @return A snapshot containing copied node, Mask, and edge values.
   * @throws std::invalid_argument when the document has no valid image backbone
   *         or contains an unsupported backbone node.
   */
  [[nodiscard]] static auto Build(const PipelineDocument& document,
                                  std::uint64_t session_generation) -> EditorNodeGraphSnapshot;

  /**
   * @brief Build the immutable Mask Groups projection for one document state.
   *
   * Walks the same validated Develop-to-DRT image backbone as @ref Build and
   * emits one row per backbone Color Grade in downstream-to-upstream display
   * order. Rows carry the NodeId, the exact node display name, the grade
   * enabled flag, and ordered Mask sub-rows. Detached or non-backbone nodes
   * never appear.
   *
   * @throws std::invalid_argument when the document has no valid image backbone
   *         or a backbone Color Grade model cannot be read.
   */
  [[nodiscard]] static auto BuildMaskGroups(const PipelineDocument& document)
      -> EditorMaskGroupSnapshot;
};

}  // namespace alcedo
