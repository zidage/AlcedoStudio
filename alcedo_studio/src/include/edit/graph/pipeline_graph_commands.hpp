//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <string>
#include <vector>

#include "edit/graph/graph_ids.hpp"
#include "edit/graph/graph_validation.hpp"
#include "edit/graph/pipeline_document.hpp"

namespace alcedo {

/**
 * @brief Insert a Color Grade immediately before @p before_node_id on the image backbone.
 *
 * The new node becomes the scene-image predecessor of @p before_node_id. When
 * @p before_node_id is DRT, the insert is after the last Color Grade or Develop.
 * @p new_id is the stable NodeId of the inserted node. The node is
 * @ref CreateCleanColorGradeNode, not a copy of the product Default look.
 *
 * @pre Caller holds the shared executor render lock. No unrelated Model is copied.
 * @return Empty on success. On failure @p document is left unchanged.
 */
[[nodiscard]] auto AddCleanColorGrade(PipelineDocument& document, const NodeId& before_node_id,
                                      NodeId new_id) -> std::vector<GraphValidationError>;

/**
 * @brief Delete a Color Grade, drop its incident edges, and bridge predecessor to successor.
 *
 * Develop and DRT cannot be removed. Removing the last Color Grade leaves Develop connected
 * to DRT.
 *
 * @pre Caller holds the shared executor render lock. No unrelated Model is copied.
 * @return Empty on success. On failure @p document is left unchanged.
 */
[[nodiscard]] auto RemoveColorGradeAndBridge(PipelineDocument& document, const NodeId& node_id)
    -> std::vector<GraphValidationError>;

/**
 * @brief Move a Color Grade so it sits on the scene-image edge from @p new_predecessor_id
 *        to @p new_successor_id.
 *
 * @pre Caller holds the shared executor render lock. No unrelated Model is copied.
 * @return Empty on success. On failure @p document is left unchanged.
 */
[[nodiscard]] auto ReconnectColorGrade(PipelineDocument& document, const NodeId& node_id,
                                       const NodeId& new_predecessor_id,
                                       const NodeId& new_successor_id)
    -> std::vector<GraphValidationError>;

/**
 * @brief Change the Color Grade UI label. Does not change NodeId.
 *
 * Develop and DRT names are rejected. Empty @p display_name is rejected.
 *
 * @pre Caller holds the shared executor render lock. No unrelated Model is copied.
 * @return Empty on success. On failure @p document is left unchanged.
 */
[[nodiscard]] auto RenameColorGrade(PipelineDocument& document, const NodeId& node_id,
                                    std::string display_name) -> std::vector<GraphValidationError>;

/**
 * @brief Set Color Grade enabled. Does not change NodeId or edges.
 *
 * @pre Caller holds the shared executor render lock. No unrelated Model is copied.
 * @return Empty on success. On failure @p document is left unchanged.
 */
[[nodiscard]] auto SetColorGradeEnabled(PipelineDocument& document, const NodeId& node_id,
                                        bool enabled) -> std::vector<GraphValidationError>;

}  // namespace alcedo
