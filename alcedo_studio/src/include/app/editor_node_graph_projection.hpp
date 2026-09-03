//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "edit/graph/graph_ids.hpp"
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
 * No Qan objects or document-owned pointers are stored here. The caller owns
 * the session and revision values used to reject stale publications.
 */
struct EditorNodeGraphSnapshot {
  std::uint64_t                         session_generation  = 0;
  std::uint64_t                         projection_revision = 0;
  std::uint64_t                         topology_revision   = 0;
  std::vector<EditorNodeProjection>     nodes;
  std::vector<EditorNodeEdgeProjection> edges;

  auto operator==(const EditorNodeGraphSnapshot&) const -> bool = default;
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
   * @param document Valid PipelineDocument whose image backbone is projected.
   * @param session_generation Session value copied into the snapshot.
   * @param projection_revision Value revision copied into the snapshot.
   * @param topology_revision Topology revision copied into the snapshot.
   * @return A snapshot containing copied node, Mask, and edge values.
   * @throws std::invalid_argument when the document has no valid image backbone
   *         or contains an unsupported backbone node.
   */
  [[nodiscard]] static auto Build(const PipelineDocument& document,
                                  std::uint64_t           session_generation,
                                  std::uint64_t           projection_revision,
                                  std::uint64_t topology_revision) -> EditorNodeGraphSnapshot;

  /**
   * @brief Return whether a snapshot belongs to the active editor session.
   * @param snapshot Candidate snapshot.
   * @param session_generation Active session value.
   */
  [[nodiscard]] static auto AcceptsGeneration(const EditorNodeGraphSnapshot& snapshot,
                                              std::uint64_t session_generation) -> bool;
};

}  // namespace alcedo
