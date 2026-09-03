//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

#include "edit/graph/color_grade_node_model.hpp"
#include "edit/graph/develop_node_model.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/image_geometry_model.hpp"
#include "edit/graph/pipeline_graph.hpp"
#include "edit/history/pipeline_history_format.hpp"
#include "json.hpp"

namespace alcedo {

/// First value reserved for the next automatically named Color Grade.
inline constexpr std::uint64_t kInitialNextColorGradeNameNumber = 2;

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

  /**
   * @brief Return the next automatically assigned Color Grade display-name number.
   */
  [[nodiscard]] auto NextColorGradeNameNumber() const -> std::uint64_t {
    return next_color_grade_name_number_;
  }

  /**
   * @brief Restore an exact automatically assigned Color Grade display-name number.
   * @throws std::invalid_argument when @p number is zero.
   */
  void               SetNextColorGradeNameNumber(std::uint64_t number);

  /**
   * @brief Advance the automatically assigned Color Grade display-name number.
   * @throws std::overflow_error when no larger value can be represented.
   */
  void               ConsumeNextColorGradeNameNumber();

  [[nodiscard]] auto TopologyDirty() const -> bool { return topology_dirty_; }
  void               MarkTopologyDirty() { topology_dirty_ = true; }
  void               ClearTopologyDirty() { topology_dirty_ = false; }

  [[nodiscard]] auto Develop() -> DevelopNodeModel*;
  [[nodiscard]] auto Develop() const -> const DevelopNodeModel*;
  /**
   * @brief First Color Grade used for current-panel routing.
   *
   * Prefers the default `grade.primary` node. When that ID is absent, returns the
   * first Color Grade on the image backbone. Null when the backbone has no Grade.
   */
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
  std::uint64_t       next_color_grade_name_number_ = kInitialNextColorGradeNameNumber;
  bool                topology_dirty_ = true;
};

/**
 * @brief Format the product display name for an automatically assigned Color Grade.
 * @throws std::invalid_argument when @p number is zero.
 */
[[nodiscard]] auto DefaultColorGradeDisplayName(std::uint64_t number) -> std::string;

/// Product Default Color Grade exposure, in EV. Baked by @ref CreateDefaultPipelineDocument.
inline constexpr float kDefaultPipelineExposureEv = 1.5f;
/// Product Default saturation multiplier (legacy UI +30 → 1 + 30/100).
inline constexpr float kDefaultPipelineSaturation = 1.3f;

/**
 * @brief Three-node product Default: Develop -> Primary Color Grade -> DRT.
 *
 * The primary Color Grade is @ref ColorGradeNodeModel::MakeDefault with
 * @ref kDefaultPipelineExposureEv and @ref kDefaultPipelineSaturation applied
 * in this factory. Does not require legacy stage remirror.
 *
 * @return A document that satisfies graph Validate and ValidateImageBackbone.
 */
[[nodiscard]] auto CreateDefaultPipelineDocument() -> PipelineDocument;

/**
 * @brief Deep copy via JSON round-trip. The clone does not share Model pointers.
 */
[[nodiscard]] auto ClonePipelineDocument(const PipelineDocument& src) -> PipelineDocument;

/**
 * @brief Color Grade nodes on the unique Develop-to-DRT scene-image path, in path order.
 *
 * Empty when the backbone cannot be walked. Off-path Color Grades are omitted.
 */
[[nodiscard]] auto ColorGradesOnImageBackbone(const PipelineDocument& document)
    -> std::vector<const ColorGradeNodeModel*>;

/**
 * @brief True when Apply may remirror the legacy stage adapter into this document.
 *
 * Requires the canonical Develop, Primary Color Grade, and DRT nodes. Extra nodes such as
 * a mask stay in place because remirror writes operator values onto the existing graph.
 */
[[nodiscard]] auto AllowsLegacyStageAdapterRemirror(const PipelineDocument& document) -> bool;

}  // namespace alcedo
