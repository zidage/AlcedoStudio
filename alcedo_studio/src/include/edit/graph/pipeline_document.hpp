//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <cstddef>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/image_geometry_model.hpp"
#include "edit/graph/pipeline_graph.hpp"

namespace alcedo {

inline constexpr std::uint32_t kPipelineDocumentFormatVersion = 2;

/**
 * @brief Serializable pipeline: geometry plus DAG. Does not own a GPU workspace.
 *
 * Graph edits set topology_dirty. Parameter Model setters do not.
 */
class PipelineDocument {
 public:
  PipelineDocument() = default;

  [[nodiscard]] auto FormatVersion() const -> std::uint32_t { return format_version_; }
  [[nodiscard]] auto Geometry() -> ImageGeometryModel& { return geometry_; }
  [[nodiscard]] auto Geometry() const -> const ImageGeometryModel& { return geometry_; }
  [[nodiscard]] auto Graph() -> PipelineGraph& { return graph_; }
  [[nodiscard]] auto Graph() const -> const PipelineGraph& { return graph_; }

  [[nodiscard]] auto TopologyDirty() const -> bool { return topology_dirty_; }
  void               MarkTopologyDirty() { topology_dirty_ = true; }
  void               ClearTopologyDirty() { topology_dirty_ = false; }

  [[nodiscard]] auto Develop() -> DevelopNodeModel*;
  [[nodiscard]] auto Develop() const -> const DevelopNodeModel*;
  [[nodiscard]] auto PrimaryGrade() -> ColorGradeNodeModel*;
  [[nodiscard]] auto PrimaryGrade() const -> const ColorGradeNodeModel*;
  [[nodiscard]] auto Drt() -> DrtNodeModel*;
  [[nodiscard]] auto Drt() const -> const DrtNodeModel*;

  /**
   * @brief Insert an adjustment on a ColorGrade node and set topology_dirty.
   */
  void InsertAdjustment(const NodeId& grade_id, std::size_t index, AdjustmentInstanceId instance_id,
                        std::unique_ptr<IOperatorModel> model);

  [[nodiscard]] auto ToJson() const -> nlohmann::json;
  static auto        FromJson(const nlohmann::json& json) -> PipelineDocument;

 private:
  std::uint32_t       format_version_ = kPipelineDocumentFormatVersion;
  ImageGeometryModel  geometry_{};
  PipelineGraph       graph_{};
  bool                topology_dirty_ = true;
};

/**
 * @brief Three-node default: Develop -> Primary Color Grade -> DRT.
 */
[[nodiscard]] auto CreateDefaultPipelineDocument() -> PipelineDocument;

/**
 * @brief True when Apply may remirror the legacy stage adapter into this document.
 *
 * Requires the canonical Develop, Primary Color Grade, and DRT nodes. Extra nodes such as
 * a mask stay in place because remirror writes operator values onto the existing graph.
 */
[[nodiscard]] auto AllowsLegacyStageAdapterRemirror(const PipelineDocument& document) -> bool;

}  // namespace alcedo
